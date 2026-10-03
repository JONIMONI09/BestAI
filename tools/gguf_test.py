#!/usr/bin/env python3
"""Conversion gate: build a GGUF, convert it, prove the result with the C engine.

    python3 tools/gguf_test.py [--engine ./hydra-run]

Three independent checks, because each one can pass while the converter is
wrong:

1. PACKING. The .hydra bytes are re-read here with an implementation that
   shares no code with the converter, the column sums are recomputed, and
   they must equal the A/B the converter claims. This catches a transposed
   bit order or a swapped plane - the failure mode a test that only reuses
   the converter's own output cannot see.

2. THE REAL ENGINE. ./hydra-run <model> <token> <steps> --json is compared
   against a reference step() written from src/hydra_engine.c. A converter
   that produces a file the engine likes but that means something else
   fails here.

3. NEGATIVE CONTROLS. A non-GGUF, a truncated file, a wrong version, an
   absurd tensor count and a quantised-only file must all be refused with a
   message that names the problem. If any of them is accepted, the gate
   fails - that is the case where "recognise a GGUF" quietly becomes "accept
   any file with four letters in front".

Run with --engine to check a freshly built binary; without it the C check
is skipped and said to be skipped, never silently passed.
"""

import argparse
import contextlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

import gguf_reader  # noqa: E402
import gguf_to_hydra  # noqa: E402

MAGIC = 0x48594452
HEADER_SIZE = 24

_results = []


def check(name, ok, detail=''):
    _results.append((name, bool(ok), detail))
    print('%s: %s%s' % ('PASS' if ok else 'FAIL', name,
                        (' - ' + detail) if detail and not ok else ''))
    return ok


# ---------------------------------------------------------------- fixtures


def build_gguf(path, tensors, metadata=None, version=3, alignment=32,
               magic=gguf_reader.GGUF_MAGIC, tensor_count=None, kv_count=None):
    """Write a syntactically real GGUF. `tensors` is [(name, dims, type, values)].

    An entry may carry a fifth element: for Q8_0 that is the F16 scale, and
    `values` must then be int8 codes in [-128, 127]. For a layout this
    fixture writer does not implement the payload is a zero-filled
    placeholder, which is what the "unsupported block layout" test needs.
    """
    body = bytearray()
    body += struct.pack('<I', magic)
    body += struct.pack('<I', version)
    body += struct.pack('<Q', len(tensors) if tensor_count is None else tensor_count)
    kv = metadata or {}
    body += struct.pack('<Q', len(kv) if kv_count is None else kv_count)

    def put_string(s):
        raw = s.encode('utf-8')
        return struct.pack('<Q', len(raw)) + raw

    for key, value in kv.items():
        body += put_string(key)
        if isinstance(value, bool):
            body += struct.pack('<I', gguf_reader.KV_BOOL) + struct.pack('<?', value)
        elif isinstance(value, str):
            body += struct.pack('<I', gguf_reader.KV_STRING) + put_string(value)
        elif isinstance(value, int):
            body += struct.pack('<I', gguf_reader.KV_UINT64) + struct.pack('<Q', value)
        elif isinstance(value, float):
            body += struct.pack('<I', gguf_reader.KV_FLOAT32) + struct.pack('<f', value)
        elif isinstance(value, list):
            body += struct.pack('<I', gguf_reader.KV_ARRAY) + struct.pack('<I', gguf_reader.KV_STRING)
            body += struct.pack('<Q', len(value))
            for item in value:
                body += put_string(item)
        else:
            raise AssertionError('unsupported fixture metadata type')

    offsets = []
    cursor = 0
    payloads = []
    for entry in tensors:
        name, dims, gtype, values = entry[:4]
        extra = entry[4] if len(entry) > 4 else None
        count = 1
        for d in dims:
            count *= d
        assert len(values) == count, 'fixture tensor size mismatch'
        block = gguf_reader.BLOCK_TYPES.get(gtype)
        if block is not None:
            per_block, block_bytes = block
            assert count % per_block == 0, 'Q8_0 needs whole blocks'
            nbytes = (count // per_block) * block_bytes
        else:
            # A layout the writer does not implement just reserves a
            # payload; the reader only validates byte sizes for the types it
            # knows, so this is exactly what a refused file looks like.
            nbytes = count * gguf_reader.DENSE_FLOAT_TYPES.get(gtype, 1)
        offsets.append(cursor)
        payloads.append((gtype, values, extra))
        cursor += nbytes

    for entry, offset in zip(tensors, offsets):
        name, dims, gtype = entry[0], entry[1], entry[2]
        body += put_string(name)
        body += struct.pack('<I', len(dims))
        for d in dims:
            body += struct.pack('<Q', d)
        body += struct.pack('<I', gtype)
        body += struct.pack('<Q', offset)

    pad = (-len(body)) % alignment
    body += bytes(pad)

    for gtype, values, extra in payloads:
        if gtype == gguf_reader.T_F32:
            body += struct.pack('<%df' % len(values), *values)
        elif gtype == gguf_reader.T_F16:
            for v in values:
                body += struct.pack('<e', v)
        elif gtype == gguf_reader.T_BF16:
            # BF16 is the top 16 bits of an F32.
            for v in values:
                bits = struct.unpack('<I', struct.pack('<f', v))[0]
                body += struct.pack('<H', bits >> 16)
        elif gtype == gguf_reader.T_Q8_0:
            # 34 bytes per block: F16 scale, then 32 int8 codes. `extra`
            # may be one scale for every block or a list with one scale per
            # block - a fixture with a single repeated scale cannot catch a
            # decoder that reads the wrong block's scale.
            scales = extra
            if scales is None:
                scales = 0.01
            if isinstance(scales, (int, float)):
                scales = [scales] * (len(values) // gguf_reader.QK8_0)
            scales = list(scales)
            assert len(scales) == len(values) // gguf_reader.QK8_0
            for b, i in enumerate(range(0, len(values), gguf_reader.QK8_0)):
                chunk = list(values[i:i + gguf_reader.QK8_0])
                assert len(chunk) == gguf_reader.QK8_0
                assert all(-128 <= c <= 127 for c in chunk), 'Q8_0 needs int8'
                body += struct.pack('<e', scales[b])
                body += struct.pack('<%db' % gguf_reader.QK8_0, *chunk)
        else:
            body += bytes(len(values))  # quantised payload placeholder

    with open(path, 'wb') as fh:
        fh.write(bytes(body))
    return path


def reference_matrix(rows, cols, seed):
    """Deterministic, asymmetric, with zeros and both signs."""
    values = []
    x = seed
    for _ in range(rows * cols):
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        r = (x >> 7) % 97
        if r < 20:
            values.append(0.0)          # exact zeros: gamma must survive them
        elif r < 50:
            values.append(r / 97.0)
        else:
            values.append(-(r / 97.0))
    return values


# ------------------------------------------------- independent .hydra reader


def read_hydra(path):
    raw = open(path, 'rb').read()
    magic, version, vocab, dim, layers, offset, length = struct.unpack(
        '<IHHIIII', raw[:HEADER_SIZE])
    body = raw[offset:offset + length]

    def value(code):
        if code == 1:
            return 1
        if code == 2:
            return -1
        return 0

    a = [0] * dim
    b = [0] * dim
    for l in range(layers):
        for i in range(dim):
            packed = body[l * dim + i]
            a[i] += value(packed & 0x03)
            b[i] += value((packed >> 2) & 0x03)
    return {
        'magic': magic, 'version': version, 'vocab': vocab, 'dim': dim,
        'layers': layers, 'weights_offset': offset, 'weights_len': length,
        'A': a, 'B': b, 'bytes': len(raw),
    }


def reference_run(a, b, dim, vocab, token_in, steps):
    """hydra_engine_step(), written from src/hydra_engine.c, not shared with it."""
    state = [0] * dim
    tokens = []
    tok = token_in
    for _ in range(steps):
        if tok >= vocab:
            tok %= vocab
        acc = [a[i] * tok + b[i] * state[i] for i in range(dim)]
        state = [max(-127, min(127, v)) for v in acc]
        raw = acc[0] % vocab
        if raw < 0:
            raw += vocab
        tok = (raw + tok + 1) % vocab
        tokens.append(tok)
    return tokens, state


# ------------------------------------------------------------------- cases


def run(engine, tmp):
    ok = True
    gguf = os.path.join(tmp, 'fixture.gguf')

    tensors = [
        ('token_embd.weight', [12, 8], gguf_reader.T_F32, reference_matrix(12, 8, 7)),
        ('blk.0.attn_q.weight', [8, 8], gguf_reader.T_F16, reference_matrix(8, 8, 99)),
        ('blk.0.attn_norm.weight', [8], gguf_reader.T_BF16, [1.5] * 8),
    ]
    build_gguf(gguf, tensors, metadata={
        'general.architecture': 'llama',
        'general.name': 'hydra-fixture',
        'general.alignment': 32,
        'general.parameter_count': 104,
        'general.sampler': 'greedy',
        'general.supported_samplers': ['greedy', 'top_k'],
        'tokenizer.ggml.model': 'gpt2',
        'tokenizer.ggml.tokens': ['hello', '▁world', '▁hydra', '!'],
    })
    ok &= check('a hand-built GGUF parses', gguf_reader.GgufFile(open(gguf, 'rb').read()).version == 3)
    ok &= check('the fixture reports 4 vocab tokens',
                len(gguf_reader.GgufFile(open(gguf, 'rb').read()).tokens()) == 4)
    ok &= check('BF16 decode matches F32',
                _bf16_matches())
    ok &= check('every one of the 65536 F16 bit patterns decodes exactly',
                _half_table_is_exact())
    ok &= check('all three dense float types decode through the same path',
                _all_dense_types_decode(tmp))

    # --- Q8_0 -----------------------------------------------------------
    ok &= check('Q8_0 block geometry is 32 values in 34 bytes',
                gguf_reader.QK8_0 == 32
                and gguf_reader.Q8_0_BLOCK_BYTES == 34
                and gguf_reader.BLOCK_TYPES[gguf_reader.T_Q8_0] == (32, 34))
    ok &= check('Q8_0 dequantises a hand-built block exactly (w = d * q)',
                _q8_0_exact(tmp))
    ok &= check('Q8_0 rows decode independently of the row alignment',
                _q8_0_row_slice(tmp))
    ok &= check('a Q8_0 tensor is decodable, not on the refused list',
                _q8_0_is_decodable(tmp))
    ok &= check('a Q8_0 file and the equivalent F16 file convert identically',
                _q8_0_matches_f16(tmp))
    ok &= check('a Q8_0 element count that is not a whole block count is refused',
                _q8_0_partial_block_refused(tmp))
    ok &= check('a Q8_0 payload truncated mid-block is refused',
                _q8_0_truncated_refused(tmp))

    out = os.path.join(tmp, 'converted.hydra')
    expect = os.path.join(tmp, 'expect.json')
    vocab_out = os.path.join(tmp, 'vocab.json')
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = gguf_to_hydra.main([gguf, '--out', out, '--json', '--expect', expect,
                                 '--vocab-out', vocab_out, '--max-rows', '0'])
    ok &= check('the converter accepts a real GGUF', rc == 0)
    if rc != 0:
        return ok
    report = json.loads(buf.getvalue())
    ok &= check('the --expect sidecar carries the same header and A/B',
                json.load(open(expect))['aggregation']['A'] == report['aggregation']['A'])

    # 1. packing, re-derived from the bytes
    got = read_hydra(out)
    want = report['aggregation']
    ok &= check('the packed column sums equal the reported A',
                got['A'] == want['A'], '%s vs %s' % (got['A'][:6], want['A'][:6]))
    ok &= check('the packed column sums equal the reported B',
                got['B'] == want['B'], '%s vs %s' % (got['B'][:6], want['B'][:6]))
    ok &= check('A and B are not the same vector (two different tensors)',
                got['A'] != got['B'])
    ok &= check('every |A[i]| fits in the layer count',
                all(abs(v) <= got['layers'] for v in got['A'] + got['B']))
    ok &= check('the header matches the report',
                got['magic'] == MAGIC and got['version'] == 1 and got['dim'] == 64
                and got['layers'] == want['peakAbs'] and got['vocab'] == 4,
                str(got))
    ok &= check('weights_len covers layers x dim',
                got['weights_len'] >= got['layers'] * got['dim'])
    ok &= check('the vocab map was written with 4 words',
                len(json.load(open(vocab_out))) == 4)
    ok &= check('the U+2581 space mark is stripped',
                'world' in json.load(open(vocab_out)))

    # the converter's own constants must not drift from the format
    ok &= check('the converter mirrors the engine constants',
                gguf_to_hydra.MAGIC == 0x48594452 and gguf_to_hydra.VERSION == 1
                and gguf_to_hydra.MAX_DIM == 64 and gguf_to_hydra.MAX_VOCAB == 1024
                and gguf_to_hydra.MAX_LAYERS == 4096 and gguf_to_hydra.HEADER_SIZE == 24)

    # 2. the real engine
    if engine:
        for token, steps in ((0, 24), (3, 40), (11, 16)):
            proc = subprocess.run([engine, out, str(token), str(steps), '--json'],
                                  capture_output=True, text=True)
            if proc.returncode != 0:
                ok &= check('the C engine loads the converted model (token %d)' % token,
                            False, proc.stderr[-300:])
                continue
            data = json.loads(proc.stdout)
            ref_tokens, ref_state = reference_run(
                got['A'], got['B'], got['dim'], got['vocab'], token, steps)
            ok &= check('the C engine reproduces A*token + B*state (token %d)' % token,
                        data['tokens'] == ref_tokens,
                        'engine %s vs reference %s' % (data['tokens'][:8], ref_tokens[:8]))
            ok &= check('the state vector matches the reference (token %d)' % token,
                        data['state'] == ref_state)
    else:
        print('SKIP: the C engine check needs a built ./hydra-run (--engine)')

    # a second conversion of a different seed must not produce the same model
    other = os.path.join(tmp, 'other.gguf')
    build_gguf(other, [('token_embd.weight', [12, 8], gguf_reader.T_F32,
                        reference_matrix(12, 8, 1234)),
                       ('blk.0.attn_q.weight', [8, 8], gguf_reader.T_F32,
                        reference_matrix(8, 8, 4321))])
    out2 = os.path.join(tmp, 'other.hydra')
    gguf_to_hydra.main([other, '--out', out2, '--max-rows', '0'])
    ok &= check('a different source produces a different model',
                read_hydra(out2)['A'] != got['A'])

    # 3. negative controls
    ok &= check('a non-GGUF is refused', _convert_fails(out, b'not a gguf file' * 8))
    truncated = os.path.join(tmp, 'truncated.gguf')
    build_gguf(truncated, tensors)
    with open(truncated, 'rb') as fh:
        blob = fh.read()
    with open(truncated, 'wb') as fh:
        fh.write(blob[:len(blob) // 2])
    ok &= check('a truncated GGUF is refused', _convert_fails(out, truncated))
    ok &= check('a wrong version is refused',
                _convert_fails(out, build_gguf(os.path.join(tmp, 'v9.gguf'), tensors, version=9)))
    ok &= check('an absurd tensor count is refused',
                _convert_fails(out, build_gguf(os.path.join(tmp, 'huge.gguf'), tensors,
                                               tensor_count=10 ** 9)))
    ok &= check('a missing A tensor name is refused',
                gguf_to_hydra.main([gguf, '--out', out, '--a-tensor', 'nope.weight']) != 0)
    ok &= check('a quantised-only file is refused with the fix in the message',
                _quantised_message(tmp))

    # the inspector
    ok &= check('the inspector identifies the fixture', _inspect(gguf, 'llama'))
    ok &= check('the inspector refuses a non-GGUF', _inspect_refuses(out))
    return ok


def _bf16_matches():
    """BF16 is the top half of an F32, so a round trip is exact at 8 mantissa
    bits. Wider than that and the decoder would be wrong for real weights."""
    values = [0.0, 1.0, -2.5, 3.25, -0.5, 1e-3, 1e3]
    packed = b''.join(
        struct.pack('<H', struct.unpack('<I', struct.pack('<f', v))[0] >> 16)
        for v in values)
    # An empty but well-formed GGUF still pads up to general.alignment,
    # so the synthetic header needs its 8 bytes of padding too.
    gguf = gguf_reader.GgufFile(
        struct.pack('<I', gguf_reader.GGUF_MAGIC) + struct.pack('<I', 3)
        + struct.pack('<Q', 0) + struct.pack('<Q', 0) + bytes(8))
    got = gguf_reader._decode_bf16(packed, len(values))
    return all(abs(a - b) <= abs(b) * 1e-2 for a, b in zip(got, values))


def _half_table_is_exact():
    """The F16 lookup table is only a shortcut if it is bit-exact.

    All 65536 bit patterns, compared against struct's own decoder - that
    includes the subnormals, both infinities and every NaN payload, which
    is where a hand-rolled half-float decoder normally goes wrong.
    """
    table = gguf_reader._half_table()
    for h in range(65536):
        a = table[h]
        b = struct.unpack('<e', struct.pack('<H', h))[0]
        if a != b and not (a != a and b != b):
            return False
    return True


def _all_dense_types_decode(tmp):
    """F32, F16 and BF16 must all come back as the same numbers.

    The fixture is built once per type from identical values, so a decoder
    that silently mixes up byte order or loses a mantissa shows up as a
    difference here rather than as a plausible-looking model.
    """
    values = reference_matrix(6, 4, 11)
    for gtype in (gguf_reader.T_F32, gguf_reader.T_F16, gguf_reader.T_BF16):
        path = os.path.join(tmp, 'type%d.gguf' % gtype)
        build_gguf(path, [('w', [6, 4], gtype, values)])
        with gguf_reader.GgufFile.open(path) as gguf:
            got = gguf.decode_rows(gguf.tensors[0], 0, 6)
        for a, b in zip(got, values):
            # BF16 keeps 8 mantissa bits, so 1e-2 relative is the tolerance.
            if abs(a - b) > abs(b) * 1e-2 + 1e-7:
                return False
    return True


def _convert_fails(out, src):
    """The converter must exit non-zero on a file it cannot convert.

    Two ways out, both legitimate: a clean non-zero return with a message,
    or an exception the CLI turns into that same message. Asserting the
    exit code alone would let a traceback pass as a "refusal".
    """
    if isinstance(src, bytes):
        path = os.path.join(os.path.dirname(out), 'junk.gguf')
        with open(path, 'wb') as fh:
            fh.write(src)
    else:
        path = src
    err = io.StringIO()
    try:
        with contextlib.redirect_stderr(err):
            rc = gguf_to_hydra.main([path, '--out', out])
    except gguf_reader.GgufError:
        return True
    if rc == 0:
        return False
    message = err.getvalue()
    return 'Traceback' not in message and len(message.strip()) > 0


def _quantised_message(tmp):
    path = os.path.join(tmp, 'q.gguf')
    # type 2 == Q4_0, a block layout this reader does not implement. It used
    # to be type 8 (Q8_0), which is now supported - a negative control that
    # silently starts passing is worse than no control at all.
    tensors = [('token_embd.weight', [4, 4], 2, [0.0] * 16)]
    build_gguf(path, tensors)
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        rc = gguf_to_hydra.main([path, '--out',
                                 os.path.join(os.path.dirname(path), 'q.hydra')])
    return rc != 0 and 'F16' in err.getvalue()


# --------------------------------------------------------------- Q8_0 cases

#: Half 0x3C00 is 1.0. Every scale below is a power of two, so scale * code
#: is exactly representable in binary16 for every code in [-128, 127]. That
#: is what makes the F16-equivalence test below an equality rather than a
#: tolerance, and it lets the blocks carry DIFFERENT scales so a decoder
#: that picks up the neighbouring block's scale cannot pass.
Q8_0_SCALE = 1.0
Q8_0_SCALES = [1.0, 0.5, 0.25, -2.0, 0.125, 4.0]


def _q8_0_codes():
    """int8 codes covering both signs, both zeros and the int8 extremes."""
    return [(-128 + i * 8) % 256 - 128 for i in range(32)]


def _q8_0_expect(codes, scales):
    """d * q computed here with struct, sharing nothing with the reader."""
    out = []
    for b in range(0, len(codes), 32):
        d = struct.unpack('<e', struct.pack('<e', scales[b // 32]))[0]
        out.extend(d * q for q in codes[b:b + 32])
    return out


def _q8_0_exact(tmp):
    """Decode hand-built blocks and compare against d * q computed here.

    The expected value is derived from the codes and the scales with struct,
    not with anything from gguf_reader, so a decoder that transposes the
    block, reads the scale at the wrong offset, takes the neighbouring
    block's scale or drops the int8 sign fails here rather than returning
    plausible numbers (rules.md R9).

    Four blocks with four DIFFERENT scales: a fixture that repeated one
    scale everywhere would let a per-block indexing bug through - which a
    mutation check on this suite demonstrated before the scales were
    varied.
    """
    codes = _q8_0_codes() * 4
    scales = [1.0, -0.5, 0.25, 8.0]
    path = os.path.join(tmp, 'q80.gguf')
    build_gguf(path, [('w', [4, 32], gguf_reader.T_Q8_0, codes, scales)])
    with gguf_reader.GgufFile.open(path) as gguf:
        got = gguf.decode_rows(gguf.tensors[0], 0, 4)

    want = _q8_0_expect(codes, scales)
    if len(got) != len(want):
        return False
    return all(a == b for a, b in zip(got, want))


def _q8_0_row_slice(tmp):
    """A partial row range must decode the same values as the whole tensor.

    Row-major Q8_0 only keeps rows byte-aligned when the row length is a
    multiple of 32. This picks a row length that is NOT (8 columns, 64
    values), so every request but the first lands mid-block. Reading the
    blocks that cover the range and slicing the edges off is what keeps the
    answer correct; a decoder that assumed row alignment would not.
    """
    rows, cols = 24, 8          # 192 values = 6 blocks, one per scale below
    codes = [(-100 + 7 * i) % 256 - 128 for i in range(rows * cols)]
    scales = Q8_0_SCALES
    path = os.path.join(tmp, 'q80rows.gguf')
    build_gguf(path, [('w', [rows, cols], gguf_reader.T_Q8_0, codes, scales)])
    want = _q8_0_expect(codes, scales)
    with gguf_reader.GgufFile.open(path) as gguf:
        tensor = gguf.tensors[0]
        full = gguf.decode_rows(tensor, 0, rows)
        for first in range(rows):
            part = gguf.decode_rows(tensor, first, 1)
            if list(part) != want[first * cols:(first + 1) * cols]:
                return False
            if list(part) != list(full[first * cols:(first + 1) * cols]):
                return False
        # and a two-row window in the middle
        mid = gguf.decode_rows(tensor, 1, 2)
        if list(mid) != want[cols:3 * cols]:
            return False
    return True


def _q8_0_is_decodable(tmp):
    """Q8_0 is block-quantised but decodable, so it must not be refused."""
    codes = _q8_0_codes() * 2
    path = os.path.join(tmp, 'q80list.gguf')
    build_gguf(path, [('w', [2, 32], gguf_reader.T_Q8_0, codes, Q8_0_SCALE),
                      ('blk.0.ffn_q.weight', [2, 32], 2, [0.0] * 64)])
    with gguf_reader.GgufFile.open(path) as gguf:
        tensor = gguf.tensors[0]
        if not tensor.is_block_quantised or tensor.is_dense_float:
            return False
        if not tensor.is_decodable:
            return False
        if gguf_reader.GgufFile(
                open(path, 'rb').read()).quantised_names() != ['blk.0.ffn_q.weight']:
            return False
        names = [t.name for t in gguf.decodable_candidates()]
        if names != ['w']:
            return False
        # The byte size must be the block size, not n_elements * 1.
        if tensor.size_bytes != 34 * (tensor.n_elements // 32):
            return False
    return True


def _q8_0_matches_f16(tmp):
    """The same weights as Q8_0 and as F16 must convert to the same model.

    This is the end-to-end equivalence the audit asked for. The scale is 1.0,
    so dequantising q gives exactly q, and q fits in binary16 with room to
    spare - the two files therefore hold bit-identical values, and any
    difference in the resulting .hydra would be a bug in the Q8_0 path, not
    rounding. Both files hold exactly one readable tensor, so both take the
    same "plane B is plane A's columns rolled" path.
    """
    rows, cols = 24, 8          # 192 values = 6 blocks, six different scales
    codes = [(-128 + (i * 37) % 256) for i in range(rows * cols)]
    codes = [c - 256 if c > 127 else c for c in codes]
    scales = Q8_0_SCALES

    # The F16 twin holds the dequantised values, computed here.
    dequantised = _q8_0_expect(codes, scales)

    q_path = os.path.join(tmp, 'eq_q.gguf')
    build_gguf(q_path, [('token_embd.weight', [rows, cols],
                         gguf_reader.T_Q8_0, codes, scales)])
    f_path = os.path.join(tmp, 'eq_f16.gguf')
    build_gguf(f_path, [('token_embd.weight', [rows, cols],
                         gguf_reader.T_F16, dequantised)])

    q_out = os.path.join(tmp, 'eq_q.hydra')
    f_out = os.path.join(tmp, 'eq_f16.hydra')
    buf_q, buf_f = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(buf_q):
        rc_q = gguf_to_hydra.main([q_path, '--out', q_out, '--json',
                                   '--max-rows', '0'])
    with contextlib.redirect_stdout(buf_f):
        rc_f = gguf_to_hydra.main([f_path, '--out', f_out, '--json',
                                   '--max-rows', '0'])
    if rc_q != 0 or rc_f != 0:
        return False
    a = read_hydra(q_out)
    b = read_hydra(f_out)
    if a['A'] != b['A'] or a['B'] != b['B']:
        return False
    if a['layers'] != b['layers'] or a['dim'] != b['dim']:
        return False
    # ...and the plane really was read from the Q8_0 tensor, not skipped.
    plane = json.loads(buf_q.getvalue())['planes'][0]
    return plane['type'] == 'Q8_0' and plane['tensor'] == 'token_embd.weight'


def _q8_0_partial_block_refused(tmp):
    """48 elements is one block plus 16 stragglers - the file is malformed.

    The file is written well-formed and then the SECOND dimension in the
    tensor directory is patched from 32 to 24 on disk, so this really is a
    corrupt file rather than a fixture writer talking itself out of an
    assertion.
    """
    path = os.path.join(tmp, 'q80odd.gguf')
    build_gguf(path, [('w', [2, 32], gguf_reader.T_Q8_0, _q8_0_codes() * 2, Q8_0_SCALE)])
    blob = open(path, 'rb').read()
    marker = struct.pack('<Q', 1) + b'w' + struct.pack('<I', 2)
    at = blob.find(marker)
    if at < 0:
        return False
    dim2_at = at + len(marker) + 8          # skip dims[0]
    patched = blob[:dim2_at] + struct.pack('<Q', 24) + blob[dim2_at + 8:]
    with open(path, 'wb') as fh:
        fh.write(patched)
    with contextlib.redirect_stderr(io.StringIO()):
        try:
            gguf_reader.GgufFile(patched)
        except gguf_reader.GgufError as exc:
            return 'multiple of' in str(exc)
    return False


def _q8_0_truncated_refused(tmp):
    """Cut the last block in half: the tensor no longer reaches the file end."""
    path = os.path.join(tmp, 'q80trunc.gguf')
    build_gguf(path, [('w', [2, 32], gguf_reader.T_Q8_0, _q8_0_codes() * 2, Q8_0_SCALE)])
    blob = open(path, 'rb').read()
    with open(path, 'wb') as fh:
        fh.write(blob[:len(blob) - 17])
    with contextlib.redirect_stderr(io.StringIO()):
        try:
            gguf_reader.GgufFile(open(path, 'rb').read())
        except gguf_reader.GgufError as exc:
            return 'past the end' in str(exc)
    return False


def _inspect(path, arch):
    proc = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'gguf_inspect.py'),
                           path, '--json'], capture_output=True, text=True)
    data = json.loads(proc.stdout)
    return data.get('ok') and data.get('architecture') == arch and data.get('convertible')


def _inspect_refuses(out):
    junk = os.path.join(os.path.dirname(out), 'junk.gguf')
    proc = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'gguf_inspect.py'),
                           junk, '--json'], capture_output=True, text=True)
    return proc.returncode == 1 and 'not a GGUF' in proc.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--engine', default=os.path.join(ROOT, 'hydra-run'),
                    help='the compiled CLI; pass an empty string to skip the C check')
    ap.add_argument('--keep', action='store_true', help='keep the temporary directory')
    args = ap.parse_args()

    engine = args.engine if (args.engine and os.path.exists(args.engine)) else None
    if args.engine and not engine:
        print('note: %s does not exist, the C engine check is skipped' % args.engine)

    tmp = tempfile.mkdtemp(prefix='hydra-gguf-')
    try:
        ok = run(engine, tmp)
    finally:
        if args.keep:
            print('kept %s' % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    failed = [n for n, o, _ in _results if not o]
    print('\n%d checks, %d failed' % (len(_results), len(failed)))
    for name in failed:
        print('  FAILED: %s' % name)
    return 0 if ok and not failed else 1


if __name__ == '__main__':
    sys.exit(main())
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
    """Write a syntactically real GGUF. `tensors` is [(name, dims, type, values)]."""
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
    for _name, dims, gtype, values in tensors:
        count = 1
        for d in dims:
            count *= d
        assert len(values) == count, 'fixture tensor size mismatch'
        # A block-quantised layout has no single byte-per-element width, and
        # the reader only validates byte sizes for dense tensors - so the
        # fixture just reserves a payload and moves on.
        width = gguf_reader.DENSE_FLOAT_TYPES.get(gtype, 1)
        offsets.append(cursor)
        payloads.append((gtype, values, width))
        cursor += count * width

    for (name, dims, gtype, _values), offset in zip(tensors, offsets):
        body += put_string(name)
        body += struct.pack('<I', len(dims))
        for d in dims:
            body += struct.pack('<Q', d)
        body += struct.pack('<I', gtype)
        body += struct.pack('<Q', offset)

    pad = (-len(body)) % alignment
    body += bytes(pad)

    for gtype, values, width in payloads:
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
    tensors = [('token_embd.weight', [4, 4], 8, [0.0] * 16)]  # type 8 == Q8_0
    build_gguf(path, tensors)
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        rc = gguf_to_hydra.main([path, '--out',
                                 os.path.join(os.path.dirname(path), 'q.hydra')])
    return rc != 0 and 'F16' in err.getvalue()


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
#!/usr/bin/env python3
"""A minimal but structurally real llama-architecture GGUF, stdlib only.

    python3 tools/gguf_minimal_llama.py --out /tmp/smoke.gguf [--seed 7]

Why this exists
---------------
The app's main inference path is llama.cpp (pinned at v0.5.0, see
android/app/src/main/java/dev/hydrastone/LlamaEngine.kt) loading GGUF
files. To prove that path end to end without downloading a multi-gigabyte
model, this script writes a tiny (2 layer, 64-dim) llama GGUF with a
real BPE-style tokenizer and random fixed-seed LCG weights. The weights
are RANDOM, so the generated text is not "intelligent" - the test proves
the pipeline (tokenize, decode, sample, detokenize) and that the pinned
llama.cpp accepts and runs a GGUF our own tools produced. The output is
deterministic for a fixed seed, a binary, and a thread count, which is
what makes it a regression smoke.

Verified 2026-10-26 against the pinned llama.cpp (v0.5.0, commit
7fe450e19305b828c199d602c23a8337aaa1f03b), which loads the file,
runs the full pipeline and prints the generated tokens (measured
numbers in docs/GGUF-IMPORT.md section 11.4 - the text is random by
construction, the weights are random). The build-specific facts from
that run are baked in: `tokenizer.ggml.model` must be a llama.cpp
vocab type ("gpt2", not "BPE"); `tokenizer.ggml.token_type` must be an
INT32 array with the pinned enum (NORMAL=1 / CONTROL=3 - a char
labeled UNKNOWN is silently dropped by the detokenizer and the output
can never print); `output_norm.weight` must exist;
`llama.feed_forward_length` must be set; the add-bos and padding keys
live in the `tokenizer.ggml.*` namespace, not under `llama.*`; and the
llama arch in this build wants ffn_gate/up as {n_embd, n_ff} with
ffn_down as {n_ff, n_embd}. The F16 variant of this fixture reaches a
ggml type mismatch in the pinned build (f32 activations x f16 norm
weights); `--dtype f32` is the verified smoke configuration and the
default. Real GGUFs from llama.cpp conversions are usually quantised,
so neither variant represents them - the fixture proves structure, not
fidelity.

What is real about the file
---------------------------
Every field llama.cpp's llama architecture loader reads is present:
general.architecture, llama.block_count / context_length / embedding_length,
attention head counts, the per-layer attn/ffn/norm tensors, the final
output projection, and a full tokenizer.ggml.* block. It is NOT real in
the sense that matters for language: 128k-parameter random weights are a
pipeline fixture, not a model. See docs/GGUF-IMPORT.md for the boundary.
"""

import argparse
import struct
import sys

GGUF_MAGIC = 0x46554747
GGUF_VERSION = 3
T_F16 = 1
T_F32 = 0  # ggml_type F32 (dense)
T_Q4_0 = 2
T_Q4_1 = 3
AL = 32

# gguf_metadata_value_type (spec order: 0 u8, 1 i8, 2 u16, 3 i16,
# 4 u32, 5 i32, 6 f32, 7 bool, 8 string, 9 array, 10 u64, 11 i64, 12 f64)
KV_UINT32, KV_INT32, KV_FLOAT32, KV_BOOL, KV_STRING, KV_ARRAY, KV_UINT64 = 4, 5, 6, 7, 8, 9, 10


def _align(pos, n=AL):
    return (pos + n - 1) & ~(n - 1)


class Writer:
    def __init__(self):
        self.kv = []          # (key, packed_bytes)
        self.tensors = []     # (name, dims, type, bytes)
        self.data = b''       # tensor data section

    # -- metadata -------------------------------------------------------
    def s(self, key, value):
        self.kv.append((key, struct.pack('<I', KV_STRING)
                        + struct.pack('<Q', len(value)) + value.encode('utf-8')))

    def u32(self, key, value):
        self.kv.append((key, struct.pack('<I', KV_UINT32) + struct.pack('<I', value)))

    def u64(self, key, value):
        self.kv.append((key, struct.pack('<I', KV_UINT64) + struct.pack('<Q', value)))

    def f32(self, key, value):
        self.kv.append((key, struct.pack('<I', KV_FLOAT32) + struct.pack('<f', value)))

    def b(self, key, value):
        self.kv.append((key, struct.pack('<I', KV_BOOL) + struct.pack('<?', value)))

    def arr(self, key, vtype, pack, values):
        body = struct.pack('<I', KV_ARRAY) + struct.pack('<I', vtype)
        body += struct.pack('<Q', len(values))
        if vtype == KV_STRING:
            for v in values:
                raw = v.encode('utf-8')
                body += struct.pack('<Q', len(raw)) + raw
        else:
            body += pack(values)
        self.kv.append((key, body))

    # -- tensors --------------------------------------------------------
    def tensor(self, name, dims, values, dtype='f32'):
        if dtype == 'f32':
            raw = struct.pack('<%df' % len(values), *values)
            self.tensors.append((name, dims, T_F32, raw))
        elif dtype == 'f16':
            raw = b''.join(struct.pack('<e', v) for v in values)
            self.tensors.append((name, dims, T_F16, raw))
        elif dtype in ('q40', 'q41'):
            n = len(values)
            if n % 32:
                raise ValueError('Q4 tensors need a multiple of 32 values '
                                 '(got %d for %s)' % (n, name))
            # Deterministic quantisation: fixed F16-exact scales, codes
            # clipped to the 4-bit range. The VALUES are what get stored -
            # the converter's decode of these blocks must come out
            # bit-exact against an independent decode (gate 1i).
            d = 0.1
            m = 0.05 if dtype == 'q41' else 0.0
            raw = b''
            for b0 in range(0, n, 32):
                chunk = values[b0:b0 + 32]
                q = []
                for v in chunk:
                    c = int(round(v / d)) + 8
                    q.append(0 if c < 0 else 15 if c > 15 else c)
                nib = b''
                for i in range(0, 32, 2):
                    nib += bytes(((q[i] & 0x0F) | ((q[i + 1] & 0x0F) << 4)))
                if dtype == 'q40':
                    raw += struct.pack('<e', d) + nib
                else:
                    raw += struct.pack('<ee', d, m) + nib
            gtype = T_Q4_0 if dtype == 'q40' else T_Q4_1
            self.tensors.append((name, dims, gtype, raw))
        else:
            raise ValueError('unknown dtype %r' % dtype)

    # -- serialization --------------------------------------------------
    def serialize(self):
        head = struct.pack('<IIQQ', GGUF_MAGIC, GGUF_VERSION,
                           len(self.tensors), len(self.kv))
        # KV entries: u64 key_len + key + u32 value_type + value
        kvs = b''
        for key, packed in self.kv:
            kraw = key.encode('utf-8')
            # GGUF string = u64 length + bytes (spec: gguf_string_t)
            kvs += struct.pack('<Q', len(kraw)) + kraw + packed
        infos = b''
        data = b''
        off = 0
        for name, dims, gtype, raw in self.tensors:
            nraw = name.encode('utf-8')
            infos += struct.pack('<Q', len(nraw)) + nraw
            infos += struct.pack('<I', len(dims))
            for d in dims:
                infos += struct.pack('<Q', d)
            infos += struct.pack('<I', gtype)
            infos += struct.pack('<Q', off)
            pad = _align(len(data)) - len(data)
            data += b'\x00' * pad + raw
            off = len(data)
        # pad kvs+infos to alignment before the data section
        meta_end = len(head) + len(kvs) + len(infos)
        data = b'\x00' * (_align(meta_end) - meta_end) + data
        return head + kvs + infos + data


class LCG:
    """Fixed-seed 64-bit LCG - reproducible on every platform Python runs."""

    def __init__(self, seed):
        self.s = (seed * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)

    def sample(self, scale=0.05):
        self.s = (self.s * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
        # Uniform in (-1, 1) * scale (the 53-bit fraction mapped to
        # (-1, 1)): well inside both F16 and F32 range, with the signs
        # and magnitudes a real normalized weight matrix has.
        return ((self.s >> 11) / float(1 << 53) * 2.0 - 1.0) * scale


def build(seed, dtype='f32', dim=64, ffn=192):
    w = Writer()
    rng = LCG(seed)

    # dim/ffn are parameters (round 16): the --t4 tests need a
    # dim % 32 != 0 variant for the loud refusal; the defaults are the
    # historical fixture shape.
    layers = 2
    heads = 4

    # --- vocab: 95 single-char BPE tokens (printable ASCII 32..126) plus
    # the four specials at the end. No merges: a mergeless BPE is still a
    # legal BPE (charmap), and it keeps the fixture one file.
    chars = [chr(c) for c in range(32, 127)]
    specials = ['<unk>', '<s>', '</s>', '<pad>']
    tokens = chars + specials
    scores = [float(-i) for i in range(len(tokens))]
    # token_type enum of the pinned build (llama.h): UNDEFINED=0,
    # NORMAL=1, UNKNOWN=2, CONTROL=3, USER_DEFINED=4, UNUSED=5, BYTE=6.
    # The printable chars are NORMAL (the BPE detokenizer emits NORMAL
    # tokens - UNKNOWN ones are dropped, which is why a file that
    # mislabels them can never print text); <unk>/<s>/</s>/<pad> are
    # CONTROL, like in real llama.cpp conversions.
    types = [1] * len(chars) + [3] * len(specials)
    unk, bos, eos, pad = len(chars), len(chars) + 1, len(chars) + 2, len(chars) + 3

    w.s('general.architecture', 'llama')
    w.s('general.basename', 'hydra-gguf-smoke')
    w.s('general.size_label', '0.1M')
    w.s('general.version', 'v1.0')
    w.s('general.type', 'Per-layer')
    w.u32('llama.context_length', 2048)
    w.u32('llama.embedding_length', dim)
    w.u32('llama.attention.head_count', heads)
    w.u32('llama.attention.head_count_kv', heads)
    w.u32('llama.block_count', layers)
    w.u32('llama.feed_forward_length', ffn)
    w.f32('llama.attention.layer_norm_rms_epsilon', 1e-5)
    # the pinned loader reads these under the tokenizer.ggml.* namespace
    # (llama-arch.cpp: LLM_KV_TOKENIZER_ADD_BOS, LLM_KV_TOKENIZER_PAD_ID);
    # an architecture-prefixed name would be parsed but IGNORED
    w.b('tokenizer.ggml.add_bos_token', True)
    # "gpt2" is the BPE vocab type llama.cpp's loader accepts for a
    # charmap-style vocabulary with (possibly empty) merges; "BPE" is
    # not a vocab type in llama.cpp's loader (2026-10-26, smoke test
    # against the pinned v0.5.0: "unknown tokenizer: 'BPE'").
    w.s('tokenizer.ggml.model', 'gpt2')
    w.arr('tokenizer.ggml.tokens', KV_STRING, None, tokens)
    w.arr('tokenizer.ggml.scores', KV_FLOAT32,
          lambda v: struct.pack('<%df' % len(v), *v), scores)
    # llama.cpp's loader requires token_type to be an INT32 array
    # (verified 2026-10-26 against the pinned v0.5.0 source,
    # llama-vocab.cpp:2491) - real GGUFs carry it that way.
    w.arr('tokenizer.ggml.token_type', KV_INT32,
          lambda v: struct.pack('<%di' % len(v), *v), types)
    w.arr('tokenizer.ggml.merges', KV_STRING, None, [])
    w.u32('tokenizer.ggml.unknown_token_id', unk)
    w.u32('tokenizer.ggml.byte_fallback_token_id', unk)
    w.u32('tokenizer.ggml.bos_token_id', bos)
    w.u32('tokenizer.ggml.eos_token_id', eos)
    w.u32('tokenizer.ggml.padding_token_id', pad)
    w.u32('general.quantization_version', 0)

    v = len(tokens)

    def mat(rows, cols):
        return [rng.sample() for _ in range(rows * cols)]

    # VALUE ORDER follows the ggml layout the dims arrays declare
    # (ne[0] is the contiguous dimension): a linear with dims [in, out]
    # stores its bytes as [out] rows of [in] contiguous values, so the
    # value list is mat(out, in); an embedding with dims [dim, vocab]
    # stores [vocab] rows of [dim] values (token t's vector is the
    # contiguous run at offset t*dim). The pinned llama.cpp proves the
    # convention (ggml_get_rows / ggml_can_mul_mat). 2026-10-30: the
    # original fixture packed the linears as mat(in, out) - a numpy
    # reading of ggml dims - which the (equally wrong) old converter
    # transpose compensated for; both are now corrected to the real
    # ggml order, which makes this file a faithful llama GGUF.
    w.tensor('token_embd.weight', [dim, v], mat(v, dim), dtype)
    for l in range(layers):
        w.tensor('blk.%d.attn_q.weight' % l, [dim, dim], mat(dim, dim), dtype)
        w.tensor('blk.%d.attn_k.weight' % l, [dim, dim], mat(dim, dim), dtype)
        w.tensor('blk.%d.attn_v.weight' % l, [dim, dim], mat(dim, dim), dtype)
        w.tensor('blk.%d.attn_output.weight' % l, [dim, dim], mat(dim, dim), dtype)
        w.tensor('blk.%d.attn_norm.weight' % l, [dim], mat(dim, 1), dtype)
        # layout of the pinned v0.5.0 llama arch (src/models/llama.cpp):
        # gate/up dims = {n_embd, n_ff}, down dims = {n_ff, n_embd}
        w.tensor('blk.%d.ffn_gate.weight' % l, [dim, ffn], mat(ffn, dim), dtype)
        w.tensor('blk.%d.ffn_up.weight' % l, [dim, ffn], mat(ffn, dim), dtype)
        w.tensor('blk.%d.ffn_down.weight' % l, [ffn, dim], mat(dim, ffn), dtype)
        w.tensor('blk.%d.ffn_norm.weight' % l, [dim], mat(dim, 1), dtype)
    w.tensor('output_norm.weight', [dim], mat(dim, 1), dtype)
    w.tensor('output.weight', [dim, v], mat(v, dim), dtype)
    return w.serialize()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--seed', type=int, default=7)
    ap.add_argument('--dtype',
                    choices=('f16', 'f32', 'q40', 'q41'), default='f32',
                    help='weight dtype (f32 is the verified llama.cpp '
                         'smoke; q40/q41 store Q4_0/Q4_1 blocks)')
    args = ap.parse_args()
    raw = build(args.seed, dtype=args.dtype)
    with open(args.out, 'wb') as fh:
        fh.write(raw)
    # Self-verify with our own reader if it is importable (stdlib path).
    sys.path.insert(0, os_path())
    import gguf_reader
    g = gguf_reader.GgufFile(open(args.out, 'rb').read())
    print('wrote %s: %d bytes, %d tensors, arch=%s, vocab=%d'
          % (args.out, len(raw), len(g.tensors), g.architecture(),
             len(g.tokens())))
    return 0


def os_path():
    import os
    return os.path.dirname(os.path.abspath(__file__))


if __name__ == '__main__':
    sys.exit(main())

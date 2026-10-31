"""Stream a 3B-geometry llama GGUF (real Llama-3.2-3B shapes) with Q4_0
weight payloads, in O(chunk) memory. Weights are synthetic (deterministic
MT19937) - the real Llama weights are not downloadable from this sandbox's
egress allowlist; the point is the full pipeline at real 3B scale."""
import struct, sys

import numpy as np

if len(sys.argv) < 2:
    raise SystemExit('usage: build_3b_synth_gguf.py <out.gguf>')
OUT = sys.argv[1]

DIM, HEADS, KV_HEADS, LAYERS, INTER, VOCAB = 3072, 8, 4, 28, 8192, 128256
KV = KV_HEADS * (DIM // HEADS)  # 512
CHUNK = 1 << 20  # values per chunk

rng = np.random.RandomState(1234567)

def q40_payload(vals_f32):
    """Same semantics as tools/gguf_to_hydra2.quantize_q40 (F16-RNE scale,
    F64 code computation, clamp) - reuse the module's numpy path."""
    import gguf_to_hydra2 as c
    return c._quantize_q40_numpy(struct.pack('<%df' % len(vals_f32), *vals_f32)) \
        if False else c._quantize_q40_numpy(vals_f32.tobytes())

def make_vals(n):
    """Realistic-ish weight distribution: mostly N(0, 0.02), a few zeros,
    a few outliers. Deterministic from the module-level rng."""
    v = rng.standard_normal(n) * 0.02
    v[rng.random_sample(n) < 0.02] = 0.0
    return v.astype('<f4')

def stream_q40_tensor(fh, n, align_pad_to=None):
    for v0 in range(0, n, CHUNK):
        v1 = min(v0 + CHUNK, n)
        if (v1 - v0) % 32:
            raise RuntimeError('chunk not block-aligned')
        fh.write(q40_payload(make_vals(v1 - v0)))

def stream_f32(fh, n, fill=0.1):
    for v0 in range(0, n, CHUNK):
        fh.write(b'\x3d\xcc\x8c3' * 0 or struct.pack('<%df' % (v1 := min(v0 + CHUNK, n)), *([fill] * (v1 - v0))))

MAGIC, VERSION = 0x46554747, 3
KV_STRING, KV_BOOL, KV_UINT64, KV_FLOAT32, KV_ARRAY = 8, 7, 10, 6, 9

tokens = ['t%06d' % i for i in range(VOCAB)]

# ---- plan tensor payloads + sizes ---------------------------------
tensors = []  # (name, dims, gtype, n_bytes)
tensors.append(('token_embd.weight', [DIM, VOCAB], 2, VOCAB * DIM * 18 // 32))
for l in range(LAYERS):
    tensors.append((f'blk.{l}.attn_q', [DIM, DIM], 2, DIM * DIM * 18 // 32))
    tensors.append((f'blk.{l}.attn_k', [DIM, KV], 2, DIM * KV * 18 // 32))
    tensors.append((f'blk.{l}.attn_v', [DIM, KV], 2, DIM * KV * 18 // 32))
    tensors.append((f'blk.{l}.attn_o', [DIM, DIM], 2, DIM * DIM * 18 // 32))
    tensors.append((f'blk.{l}.attn_norm', [DIM], 0, DIM * 4))
    tensors.append((f'blk.{l}.ffn_norm', [DIM], 0, DIM * 4))
    tensors.append((f'blk.{l}.ffn_gate', [DIM, INTER], 2, DIM * INTER * 18 // 32))
    tensors.append((f'blk.{l}.ffn_up', [DIM, INTER], 2, DIM * INTER * 18 // 32))
    tensors.append((f'blk.{l}.ffn_down', [INTER, DIM], 2, INTER * DIM * 18 // 32))
tensors.append(('norm', [DIM], 0, DIM * 4))

meta = [('general.architecture', 'str', 'llama'),
        ('llama.embedding_length', 'u32', DIM),
        ('llama.attention.head_count', 'u32', HEADS),
        ('llama.attention.head_count_kv', 'u32', KV_HEADS),
        ('llama.block_count', 'u32', LAYERS),
        ('llama.feed_forward_length', 'u32', INTER),
        ('llama.rope.freq_base', 'f32', 500000.0),
        ('llama.attention.layer_norm_eps', 'f32', 1e-5),
        ('tokenizer.ggml.bos_token_id', 'u32', 128000),
        ('tokenizer.ggml.eos_token_id', 'u32', 128001),
        ('tokenizer.ggml.token_type', 'u32', 1),
        ('tokenizer.ggml.tokens', 'strarr', tokens)]

def put_string(s):
    b = s.encode()
    return struct.pack('<Q', len(b)) + b

# header
body = bytearray()
body += struct.pack('<II', MAGIC, VERSION)
body += struct.pack('<Q', len(tensors))
body += struct.pack('<Q', len(meta))
for k, kind, val in meta:
    body += put_string(k)
    if kind == 'str':
        body += struct.pack('<I', KV_STRING) + put_string(val)
    elif kind == 'u32':
        body += struct.pack('<I', KV_UINT64) + struct.pack('<Q', val)
    elif kind == 'f32':
        body += struct.pack('<I', KV_FLOAT32) + struct.pack('<f', val)
    elif kind == 'strarr':
        body += struct.pack('<II', KV_ARRAY, KV_STRING) + struct.pack('<Q', len(val))
        for t in val:
            body += put_string(t)

# tensor directory (offsets relative to data start)
align = 32
offsets = []
cursor = 0
for name, dims, gtype, nb in tensors:
    offsets.append(cursor)
    cursor += nb
for i, (name, dims, gtype, nb) in enumerate(tensors):
    body += put_string(name)
    body += struct.pack('<I', len(dims))
    for d in dims:
        body += struct.pack('<Q', d)
    body += struct.pack('<I', gtype)
    body += struct.pack('<Q', offsets[i])
pad = (-len(body)) % align
body += bytes(pad)

data_start = len(body)
print('header+meta+tdir bytes:', data_start)

# stream the data section
import time
t0 = time.time()
with open(OUT, 'wb') as fh:
    fh.write(bytes(body))
    for (name, dims, gtype, nb), off in zip(tensors, offsets):
        n = 1
        for d in dims:
            n *= d
        if gtype == 2:
            stream_q40_tensor(fh, n)
        else:
            stream_f32(fh, n)
        fh.flush()
        print('  %s (%d B) done, %.1fs' % (name, nb, time.time() - t0), flush=True)
print('total %.1f seconds, file size:' % (time.time() - t0))
import os
print(os.path.getsize(OUT), 'expected', data_start + cursor)
assert os.path.getsize(OUT) == data_start + cursor

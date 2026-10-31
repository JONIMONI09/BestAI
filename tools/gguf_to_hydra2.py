#!/usr/bin/env python3
"""GGUF -> .hydra2 converter (round 12, M2.2 / M3.1 / M4).

    python3 tools/gguf_to_hydra2.py in.gguf out.hydra2 [--max-experts 16]

Reads a decoder-only **llama** GGUF (F32/F16/BF16/Q8_0/Q4_0/Q4_1
weights) and writes
a complete-contract `.hydra2` file (FORMAT-V2.md, architecture
`moe-transformer-v1`) that the MoE engine (`src/hydra_moe.c`) runs
directly: per-layer GQA attention, expert FFN, RMSNorm, real logits,
plus the embedded vocabulary and BPE merges (role 8, section 3.2.3).

MAPPING (llama -> moe-transformer-v1)
-------------------------------------
  dim / n_heads / n_kv_heads / head_dim / inter / layers
      from llama.embedding_length / attention.head_count /
      attention.head_count_kv / feed_forward_length / block_count
  tok_emb (GGUF dims [dim, vocab])      -> embed [vocab, dim]
  blk.L.attn_q/k/v/o (dims [in, out])   -> attn_q/k/v/o.L [out, in]
  blk.L.attn_norm / ffn_norm            -> attn_norm.L / ffn_norm.L
  blk.L.ffn_gate / ffn_up ([dim, inter])-> ffn_gate/up.L [inter, dim]
  blk.L.ffn_down ([inter, dim])         -> ffn_down.L [dim, inter]
  output norm                           -> final_norm
  output (dims [dim, vocab], optional)  -> lm_head [vocab, dim];
                                     absent => tied embedding
  tokenizer.ggml.tokens                 -> 'vocab' tensor (VOCAB_UTF8, 8)
  tokenizer.ggml.merges                 -> 'merges' tensor (U16_PAIRS, 8)
  llama.rope.freq_base                  -> v2.rope_theta (default 10000.0)

LAYOUT (why there is NO transpose step)
---------------------------------------
GGUF dims are ggml order: ne[0] is the CONTIGUOUS (fastest-varying)
dimension, the reverse of numpy shape order. A linear stored with dims
[in, out] therefore holds its bytes as [out] rows of [in] contiguous
values - already the engine's [out, in] row-major layout - and an
embedding with dims [dim, vocab] holds [vocab] rows of [dim] values,
already the engine's [vocab, dim] embed layout (token t's vector is the
contiguous run at offset t*dim, which is exactly how the engine reads
it: embed + token_in * dim). Every tensor is therefore copied in FILE
ORDER; applying a transpose here would scramble every non-square tensor
(round-13 fix: the original converter treated dims[0] as the row count
and transposed, which is the numpy reading of a ggml tensor). The pinned
llama.cpp source proves the convention (ggml_get_rows reads wte rows as
contiguous ne[0] blocks; ggml_can_mul_mat requires w->ne[0] == in).

DENSE FFN -> EXPERTS (the masked decomposition, EXACT by construction)
-----------------------------------------------------------------------
The engine's expert geometry is FIXED to the model's `inter` (every
expert is a full-width [inter x dim] gate/up and [dim x inter] down),
so a dense SwiGLU layer `down @ (SiLU(gate@x) * up@x)` is converted to
E full-width experts that PARTITION the intermediate rows: expert e
keeps only the rows [e*c .. (e+1)*c) of gate (c = inter/E) and up and
zeroes the rest; its down matrix is the full dense down unmodified.
The scaling must respect the nonlinearity: the engine computes the
expert output as SiLU(G_e b) * (U_e b), and SiLU(E*g) * u is NOT
E * (SiLU(g) * u). Therefore
    G_e = gate block e          (unscaled)
    U_e = E * (up block e)      (scaled by E)
which makes the per-row contribution exact: with a UNIFORM router
(all-zero router rows -> every logit 0 -> softmax weight 1/E) and
top_k = E the engine computes
  sum_e (1/E) * (SiLU(g_e) * (E * u_e)) = sum_e SiLU(g_e) * u_e
  = the dense result,
up to F32 rounding of the per-expert summation order (a few ulp) - the
only approximation in the weight path. (Round-13 fix: the original
converter scaled BOTH the gate and the up rows by E; because SiLU is
nonlinear, every E > 1 model then computed a different function than
the source FFN. The E = 1 default was unaffected and is a verbatim
copy of the dense FFN.) Note the storage cost: E experts of FULL
width, i.e. E times the dense FFN bytes per layer; --max-experts N
keeps E = the largest divisor of `inter` in [1, N]. The dense
matrices are additionally kept verbatim in the trunk tensors
`ffn_gate/up/down.L` (source of record).

DOCUMENTED APPROXIMATIONS (stated, not hidden)
----------------------------------------------
* The engine's attention is NON-CAUSAL over the cached positions (the
  fixture engine's semantics; FORMAT-V2.md) while the original llama is
  causal. The converted model is a different function of the history;
  it is a faithful weight transfer, not a bit-exact model transplant.
* RMSNorm epsilon is the engine's fixed 1e-6 (the GGUF's
  llama.attention.layer_norm_rms_epsilon is not carried).
* KV history is bounded by the engine's sink + ring geometry
  (v2.kv_sink 2 / v2.kv_ring 8) - long contexts are truncated.
* BPE: the codec maps single-byte tokens and the merged strings
  (FORMAT-V2.md 3.2.3). Merge rules whose concatenated string is not a
  token are SKIPPED (the file stays valid); a rule is written as
  (id(left), id(right)) with the target id resolved from the
  concatenation.

LOUD REJECTIONS (exit 2, message names the problem)
---------------------------------------------------
  - not a GGUF / bad version / empty file
  - general.architecture is not 'llama'
  - no tokenizer.ggml.tokens
  - a tensor stored in a layout this reader cannot decode
    (Q4_K, Q5_0, MXFP4, ...)
  - a required llama tensor is missing
  - head_count does not divide embedding_length
"""

import argparse
import math
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gguf_reader  # noqa: E402


def die(msg):
    sys.stderr.write('gguf_to_hydra2: %s\n' % msg)
    sys.exit(2)


def u32(v):
    return struct.pack('<I', v)


def f32(v):
    return struct.pack('<f', v)


def crc32(data):
    import zlib
    return zlib.crc32(data) & 0xFFFFFFFF


class StreamTrunk:
    """16-byte-aligned STREAMING trunk writer (round 16).

    Records the tensor-directory entries plus the payload sizes and
    keeps a producer callable per tensor that writes the payload bytes
    to the open output file. The in-memory footprint is O(directory),
    not O(model): the pre-round-16 Trunk held the entire trunk (2.3 GB
    for a 3B, 16 GB for an 8B) in one bytearray, which made the large
    conversions impossible on the machines this round exists for."""

    def __init__(self):
        # (name, role, tid, ndims, elems, off, nbytes, producer)
        self.tensors = []
        self._cur = 0

    def add(self, name, role, tid, ndims, elems, nbytes, producer):
        off = self._cur
        if off % 16:
            off += 16 - off % 16
        self._cur = off + nbytes
        self.tensors.append((name, role, tid, ndims, elems, off,
                             nbytes, producer))
        return off

    @property
    def total_bytes(self):
        return self._cur


class _CrcSink:
    """File-like wrapper that also collects the CRC-32 of everything
    written (the expert payloads in pass 2; the directory is patched
    with the final values after the blob is complete)."""

    def __init__(self, fh):
        self.fh = fh
        self.crc = 0

    def write(self, b):
        self.fh.write(b)
        self.crc = zlib.crc32(b, self.crc)
        return len(b)


def prod_bytes(data):
    """Producer for a payload that is small enough to hold in memory
    (norms, routers, vocab, merges)."""
    def producer(fh):
        fh.write(data)
    return producer


def flat_window_f32(gguf, t, v0, v1, chunk_rows=256):
    """F32 bytes of the flat value range [v0, v1) of a tensor (file
    order), decoded row by row. The window need not be row-aligned:
    the covering rows are decoded and the edges sliced off (the same
    technique the reader uses for the block types). O(row) memory."""
    rows, cols = t.shape()
    r0, r1 = v0 // cols, (v1 - 1) // cols
    out = bytearray()
    for r in range(r0, r1 + 1):
        block = gguf.decode_rows(t, r, 1)
        lo = max(v0 - r * cols, 0)
        hi = min(v1 - r * cols, cols)
        out.extend(struct.pack('<%df' % (hi - lo), *block[lo:hi]))
    return bytes(out)


def prod_flat_f32(gguf, t, chunk_rows=256):
    """Stream the decoded F32 bytes of a tensor (file order) to the
    open file, one row chunk at a time - O(chunk) memory."""
    def producer(fh):
        rows, cols = t.shape()
        for r0 in range(0, rows, chunk_rows):
            nr = min(chunk_rows, rows - r0)
            block = gguf.decode_rows(t, r0, nr)
            fh.write(struct.pack('<%df' % len(block), *block))
    return producer


def prod_t4(gguf, t, chunk_values=32 * 1024):
    """Stream the Q4_0 payload of a trunk tensor (round 16): a Q4_0
    source is a bit-exact raw copy (the file order of a GGUF llama
    tensor IS the engine layout, and the blocks are flat 32-value
    units, so no re-quantization and no transpose); any other
    decodable layout is decoded in chunks of whole 32-value blocks
    and quantized with the same deterministic scheme as
    tools/make_model.py q40_block."""
    if t.ggml_type == 2:  # T_Q4_0
        def producer(fh):
            fh.write(gguf.raw_bytes(t))
        return producer

    def producer(fh):
        n = t.n_elements
        for v0 in range(0, n, chunk_values):
            v1 = min(v0 + chunk_values, n)
            fh.write(quantize_q40(flat_window_f32(gguf, t, v0, v1)))
    return producer


def prod_expert_t4(gguf, gate, up, down, i0, chunk, dim, inter, E):
    """Stream one Q4_0 expert payload ([gate][up][down] as 18-byte
    blocks, round 16 layout 2). A Q4_0 source with scale 1.0 is a
    bit-exact raw slice (dim % 32 == 0 keeps the slice block-aligned);
    anything else (other source layout, or the E > 1 up scaling) is
    decoded, scaled, then quantized in 32-value-aligned chunks. The
    down matrix is unmasked for every expert: its flat stream is the
    full source stream."""
    cache = {}

    def prod_sub(fh, t, v0, v1, scale):
        if t.ggml_type == 2 and scale == 1.0:
            raw = cache.get(t.name)
            if raw is None:
                raw = gguf.raw_bytes(t)
                cache[t.name] = raw
            fh.write(raw[v0 * 18 // 32:v1 * 18 // 32])
            return
        cv = 32 * 1024
        for c0 in range(v0, v1, cv):
            c1 = min(c0 + cv, v1)
            w = flat_window_f32(gguf, t, c0, c1)
            if scale != 1.0:
                a = struct.unpack('<%df' % (len(w) // 4), w)
                w = struct.pack('<%df' % len(a), *(x * scale for x in a))
            fh.write(quantize_q40(w))

    def producer(fh):
        prod_sub(fh, gate, i0, i0 + chunk * dim, 1.0)
        prod_sub(fh, up, i0, i0 + chunk * dim, float(E))
        rows, cols = down.shape()
        prod_sub(fh, down, 0, rows * cols, 1.0)
    return producer


def prod_expert_f32(gguf, gate, up, down, i0, chunk, dim, inter, E):
    """Stream one F32 expert payload (layout 0): the masked
    decomposition of the dense FFN - engine rows i0..i0+chunk kept
    (the UP rows carry the E factor; SiLU is nonlinear), every other
    row zeroed; the down matrix is unmasked. Zero runs go out as
    literal zero bytes; the kept window is decoded row by row, so
    the memory stays O(chunk)."""
    n = inter * dim

    def prod_masked(fh, t, scale):
        fh.write(b'\x00' * (i0 * 4))
        cv = 32 * 1024
        for c0 in range(i0, i0 + chunk * dim, cv):
            c1 = min(c0 + cv, i0 + chunk * dim)
            w = flat_window_f32(gguf, t, c0, c1)
            a = struct.unpack('<%df' % (len(w) // 4), w)
            fh.write(struct.pack('<%df' % len(a), *(x * scale for x in a)))
        fh.write(b'\x00' * ((n - i0 - chunk * dim) * 4))

    def producer(fh):
        prod_masked(fh, gate, 1.0)
        prod_masked(fh, up, float(E))
        rows, cols = down.shape()
        for r0 in range(0, rows, 256):
            nr = min(256, rows - r0)
            block = gguf.decode_rows(down, r0, nr)
            fh.write(struct.pack('<%df' % len(block), *block))
    return producer


def quantize_q40(f32_bytes):
    """Deterministic F32 -> Q4_0 block payload (round 16).

    numpy (when importable) accelerates this ~50x; the results are
    bit-identical (see _quantize_q40_numpy). The pure-Python loop below
    is the reference definition - keep it exact, the fast path follows.
    """
    if _numpy_available():
        return _quantize_q40_numpy(f32_bytes)
    return _quantize_q40_python(f32_bytes)


def _quantize_q40_python(f32_bytes):
    """Deterministic F32 -> Q4_0 block payload (round 16).

    18-byte blocks of 32 consecutive values (flat row-major): F16 scale
    d = round-half-even of max|w| / 7, then 16 bytes carrying two 4-bit
    codes each (value 2j in the LOW nibble of byte j, 2j+1 in the HIGH
    nibble); code = clamp(floor(w/d + 8.5), 0, 15), so the decode is
    w ~ d * (code - 8). d == 0 (all-zero block) -> every code 8 ->
    exact zeros on decode. IDENTICAL to tools/make_model.py
    q40_block (same rounding, same packing) - the C engine decode of
    either is bit-exact against this payload.
    """
    n = len(f32_bytes) // 4
    if n % 32:
        die('cannot quantize %d elements to Q4_0 (need a multiple of 32)'
            % n)
    vals = struct.unpack('<%df' % n, f32_bytes)
    out = bytearray()
    for b in range(n // 32):
        gw = vals[b * 32:(b + 1) * 32]
        mx = max((abs(x) for x in gw), default=0.0)
        d = struct.unpack('<e', struct.pack('<e', mx / 7.0))[0]
        nb = bytearray(16)
        for i, x in enumerate(gw):
            if d == 0.0:
                c = 8
            else:
                c = int(math.floor(x / d + 8.5))
                if c < 0:
                    c = 0
                elif c > 15:
                    c = 15
            if i % 2 == 0:
                nb[i // 2] |= c & 0x0F
            else:
                nb[i // 2] |= (c & 0x0F) << 4
        out += struct.pack('<e', d)
        out += bytes(nb)
    return bytes(out)


def _numpy_available():
    """Probe for numpy without importing it (the reference path must
    run on a bare stdlib interpreter)."""
    import importlib.util
    return importlib.util.find_spec('numpy') is not None


def _quantize_q40_numpy(f32_bytes):
    """Vectorized quantize_q40 with BIT-IDENTICAL results.

    numpy is OPTIONAL (a hard dependency would break CI images without
    it): every rounding step uses the same semantics as the pure-Python
    path - the F16 scale is rounded with IEEE round-half-even (np.float16
    is exactly that), and the code computation x/d + 8.5 runs in F64
    (np.float64), the same width the pure-Python float division uses, so
    floor() sees bit-identical operands. A parity check in
    tools/gguf_to_hydra2_test.py keeps the two locked together.
    """
    import numpy as np  # optional import: caller decides
    n = len(f32_bytes) // 4
    if n % 32:
        die('cannot quantize %d elements to Q4_0 (need a multiple of 32)'
            % n)
    a = np.frombuffer(f32_bytes, dtype='<f4').astype('<f8')
    blocks = a.reshape(-1, 32)
    mx = np.abs(blocks).max(axis=1)
    d16 = np.float16(mx / 7.0)          # IEEE round-half-even
    d = d16.astype('<f8')               # exact (F16 subset of F64)
    z = d == 0
    # divide by 1.0 in the zero-scale rows: x/1 is finite for every F32
    # x, so no NaN/inf (and no RuntimeWarning) is produced before the
    # codes[z] = 8 override replaces those rows entirely.
    d_safe = np.where(z, 1.0, d)
    codes = np.floor(a.reshape(-1, 32) / d_safe[:, None] + 8.5)
    codes = np.clip(codes, 0, 15)
    codes[z] = 8
    codes = codes.astype(np.uint8)
    lo = codes[:, 0::2]
    hi = codes[:, 1::2]
    packed = (lo | (hi << 4)).astype(np.uint8)
    # .view() reinterprets the F16 bit pattern as u16 - .astype() would
    # be a value cast (123.9375 -> 123), corrupting every scale. The
    # output is interleaved per block - 18-byte units of [F16 d][16
    # nibble bytes] - which is the on-disk Q4_0 layout, so the columns
    # must be written together, not scales-then-codes.
    d16 = np.ascontiguousarray(d16, dtype=np.float16)
    bits = d16.view(np.uint16)
    out = np.empty((codes.shape[0], 18), dtype=np.uint8)
    out[:, 0] = bits & 0xFF
    out[:, 1] = bits >> 8
    out[:, 2:] = packed
    return out.tobytes()


def pick_experts(inter, max_experts):
    """The largest divisor of `inter` in [1, max_experts] (1 when none)."""
    for e in range(min(max_experts, inter), 0, -1):
        if inter % e == 0:
            return e
    return 1


def flat_f32(gguf, tensor):
    """Decode every element of a 1-D or 2-D tensor, IN FILE (ggml)
    ORDER, as F32 bytes.

    The file bytes of every tensor ARE the engine layout (linears as
    [out][in], embeddings as [vocab][dim]; see the LAYOUT section of the
    module docstring), so this decodes the whole tensor in file order.
    decode_rows() indexes rows as dims[0] chunks of dims[1] values; for
    a 1-D tensor it is a single row of dims[0] values - either way the
    concatenation is the complete file, in order.
    """
    shape = tensor.shape()
    if shape is None:
        die('tensor %s has %d dimensions; only 1-D and 2-D are supported'
            % (tensor.name, tensor.n_dims))
    rows, cols = shape
    out = bytearray(rows * cols * 4)
    for r0 in range(0, rows, 256):
        nr = min(256, rows - r0)
        block = gguf.decode_rows(tensor, r0, nr)
        struct.pack_into('<%df' % len(block), out, r0 * cols * 4, *block)
    return bytes(out)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Convert a llama GGUF to a .hydra2 MoE model')
    ap.add_argument('in_gguf')
    ap.add_argument('out_hydra2', nargs='?',
                    help='output .hydra2 path (not needed with --probe)')
    ap.add_argument('--probe', action='store_true',
                    help='header-only acceptance probe: run every '
                         'acceptance check, print a JSON report, write '
                         'nothing (used by the server to decide the '
                         'import mode)')
    ap.add_argument('--t4', action='store_true',
                    help='store the large matrices (embed, attn_q/k/v/o, '
                         'ffn_gate/up/down, lm_head) and the expert '
                         'payloads as Q4_0 blocks (round 16, layout 2) '
                         'instead of F32: a Q4_0 source tensor is copied '
                         'bit-exact, any other decodable layout is '
                         'quantized deterministically (F16 d = '
                         'round-half-even(max|w|/7), code = '
                         'clamp(floor(w/d+8.5),0,15), w ~ d*(code-8)). '
                         'Needs dim and inter divisible by 32; the '
                         'result is the phone-sized file (an 8B Q4_0 '
                         'GGUF stays ~4.9 GB instead of a 31.5 GB F32 '
                         '.hydra2).')
    ap.add_argument('--max-experts', type=int, default=1,
                    help='expert count bound for the dense->expert '
                         'decomposition (default 1 = verbatim dense FFN '
                         'as a single expert; E is the largest divisor '
                         'of the FFN width within the bound; costs E '
                         'times the FFN storage per layer)')
    args = ap.parse_args(argv)
    if not args.probe and args.out_hydra2 is None:
        ap.error('out_hydra2 is required unless --probe is given')

    try:
        gguf = gguf_reader.GgufFile.open(args.in_gguf)
    except (OSError, gguf_reader.GgufError) as e:
        die(str(e))
    with gguf:
        meta = gguf.metadata

        arch = gguf.architecture()
        if arch != 'llama':
            die("architecture is %r - only decoder-only 'llama' GGUFs are "
                'supported (this converter maps the llama tensor names)'
                % arch)

        tokens = gguf.tokens()
        if not tokens:
            die("no tokenizer.ggml.tokens - the v2 vocabulary tensor "
                '(section 3.2.3) cannot be built')
        vocab = len(tokens)

        def need_u32(key):
            v = meta.get(key)
            if not isinstance(v, int) or v < 1:
                die('metadata %s is missing or not a positive integer' % key)
            return v

        dim = need_u32('llama.embedding_length')
        n_heads = need_u32('llama.attention.head_count')
        n_kv = need_u32('llama.attention.head_count_kv')
        layers = need_u32('llama.block_count')
        inter = need_u32('llama.feed_forward_length')
        if dim % n_heads != 0:
            die('embedding_length %d is not divisible by head_count %d'
                % (dim, n_heads))
        if not 1 <= n_kv <= n_heads:
            die('head_count_kv %d out of range (1..head_count %d)'
                % (n_kv, n_heads))
        head_dim = dim // n_heads
        kvdim = n_kv * head_dim

        rope_theta = 10000.0
        rf = meta.get('llama.rope.freq_base')
        if isinstance(rf, (int, float)) and rf > 0:
            rope_theta = float(rf)
        bos = meta.get('tokenizer.ggml.bos_token_id')
        if not (isinstance(bos, int) and 0 <= bos < vocab):
            bos = 0

        E = pick_experts(inter, args.max_experts)
        chunk = inter // E

        # ---- find + validate every tensor we need ---------------------
        # Two naming conventions exist in the wild: the official llama
        # GGUF (tok_emb / attn_o / norm / output) and the pytorch-style
        # names of this repository's pinned llama build
        # (token_embd.weight / attn_output.weight / output_norm.weight).
        # Both are accepted, loud-when-neither.
        def find(name):
            t = gguf.find(name)
            if t is None:
                die('required tensor %r is missing from the GGUF' % name)
            if not t.is_decodable:
                die('tensor %r is %s - a block-quantised layout this '
                    'reader does not implement; re-export the model in '
                    'F16 or F32 (llama.cpp: --convert-f16) and convert '
                    'again' % (name, t.type_name))
            return t

        def find_any(*names):
            for n in names:
                if gguf.find(n) is not None:
                    return find(n)
            die('required tensor is missing - looked for %s'
                % ' / '.join(repr(n) for n in names))

        tok_emb = find_any('tok_emb', 'token_embd.weight')
        if tok_emb.n_dims != 2 or list(tok_emb.dims) != [dim, vocab]:
            die('tok_emb is %s, expected [%d, %d]'
                % (tok_emb.dims, dim, vocab))
        output = gguf.find('output') or gguf.find('output.weight')
        if output is not None:
            if output.n_dims != 2 or list(output.dims) != [dim, vocab]:
                die('output is %s, expected [%d, %d]'
                    % (output.dims, dim, vocab))
            if not output.is_decodable:
                die('tensor output is %s - undecodable; re-export in '
                    'F16/F32' % output.type_name)
        norm_t = gguf.find('norm') or gguf.find('output_norm.weight')
        if norm_t is None:
            die("required output norm is missing - looked for 'norm' / "
                "'output_norm.weight'")

        if args.t4:
            if dim % 32 or inter % 32:
                die('t4 mode needs dim and inter divisible by 32 (got '
                    'dim %d, inter %d) - a Q4_0 row holds 32 values'
                    % (dim, inter))

        # ---- streaming write plan (round 16) -------------------------
        # Pass 1 records the payload sizes and one producer callable
        # per payload (O(directory) memory); pass 2 (at the bottom of
        # main) streams the bytes to disk. The pre-round-16 path held
        # the whole trunk plus every expert payload in memory (~2.5 GB
        # for a 3B) and could not convert the large models at all.
        trunk = StreamTrunk()
        fe = 0
        expert_dir = []
        expert_producers = []
        # every tensor whose payload dominates the file: the probe
        # reports whether all of them are Q4_0 (the bit-exact t4 path)
        weight_names = {tok_emb.name}
        if args.t4:
            # 18 bytes per 32 values, per sub-matrix, three sub-matrices
            # ([gate][up][down]) - the parentheses matter: 18 // 32 is 0.
            expert_len = 3 * (inter * dim * 18 // 32)
        else:
            expert_len = 3 * inter * dim * 4

        # ---- embed (file order: already [vocab][dim]) -----------------
        if args.t4:
            trunk.add('embed', 2, 0x0030, 2, vocab * dim,
                      vocab * dim * 18 // 32, prod_t4(gguf, tok_emb))
        else:
            trunk.add('embed', 2, 0x0012, 2, vocab * dim,
                      vocab * dim * 4, prod_flat_f32(gguf, tok_emb))

        # ---- per-layer blocks -----------------------------------------
        for l in range(layers):
            q = find_any('blk.%d.attn_q' % l,
                         'blk.%d.attn_q.weight' % l)
            k = find_any('blk.%d.attn_k' % l,
                         'blk.%d.attn_k.weight' % l)
            v = find_any('blk.%d.attn_v' % l,
                         'blk.%d.attn_v.weight' % l)
            o = find_any('blk.%d.attn_o' % l,
                         'blk.%d.attn_output.weight' % l)
            an = find_any('blk.%d.attn_norm' % l,
                          'blk.%d.attn_norm.weight' % l)
            fn = find_any('blk.%d.ffn_norm' % l,
                          'blk.%d.ffn_norm.weight' % l)
            gate = find_any('blk.%d.ffn_gate' % l,
                            'blk.%d.ffn_gate.weight' % l)
            up = find_any('blk.%d.ffn_up' % l,
                          'blk.%d.ffn_up.weight' % l)
            down = find_any('blk.%d.ffn_down' % l,
                            'blk.%d.ffn_down.weight' % l)
            # GGUF llama stores linears with ggml dims [in, out]; the
            # FILE ORDER of such a tensor is already [out][in] row-major
            # (the engine layout) - validate the dims, copy in file
            # order, no transpose.
            weight_names.update(t.name for t in (q, k, v, o, gate, up, down))
            for t, exp in ((q, [dim, dim]), (k, [dim, kvdim]),
                           (v, [dim, kvdim]), (o, [dim, dim]),
                           (an, [dim]), (fn, [dim]),
                           (gate, [dim, inter]), (up, [dim, inter]),
                           (down, [inter, dim])):
                if t.n_dims != len(exp) or list(t.dims) != exp:
                    die('tensor %s is %s, expected %s'
                        % (t.name, t.dims, exp))
            for nm, t, ne in (('attn_q.%d' % l, q, dim * dim),
                              ('attn_k.%d' % l, k, kvdim * dim),
                              ('attn_v.%d' % l, v, kvdim * dim),
                              ('attn_o.%d' % l, o, dim * dim)):
                if args.t4:
                    trunk.add(nm, 7, 0x0030, 2, ne, ne * 18 // 32,
                              prod_t4(gguf, t))
                else:
                    trunk.add(nm, 7, 0x0012, 2, ne, ne * 4,
                              prod_flat_f32(gguf, t))
            trunk.add('attn_norm.%d' % l, 3, 0x0012, 1, dim, dim * 4,
                      prod_bytes(flat_f32(gguf, an)))
            trunk.add('ffn_norm.%d' % l, 3, 0x0012, 1, dim, dim * 4,
                      prod_bytes(flat_f32(gguf, fn)))

            # uniform router: every logit is 0 -> softmax 1/E (exact)
            trunk.add('router.%d' % l, 1, 0x0012, 2, E * dim,
                      E * dim * 4, prod_bytes(b'\x00' * (E * dim * 4)))

            # dense FFN kept verbatim in the trunk (source of record;
            # the engine routes through the experts)
            for nm, t, ne in (('ffn_gate.%d' % l, gate, inter * dim),
                              ('ffn_up.%d' % l, up, inter * dim),
                              ('ffn_down.%d' % l, down, dim * inter)):
                if args.t4:
                    trunk.add(nm, 0, 0x0030, 2, ne, ne * 18 // 32,
                              prod_t4(gguf, t))
                else:
                    trunk.add(nm, 0, 0x0012, 2, ne, ne * 4,
                              prod_flat_f32(gguf, t))

            # the masked decomposition: E full-width experts that
            # partition the intermediate rows (chunk = inter/E rows
            # each; E == 1 -> the dense FFN verbatim). SiLU is
            # nonlinear, so only the UP rows carry the E factor (see
            # the module docstring): G_e = block, U_e = E * block.
            # All expert payloads are the same size, so the offsets
            # are (l*E + e) * expert_len - the directory is known
            # before a single payload byte is written (the CRCs are
            # patched in after pass 2, below).
            for e in range(E):
                i0 = e * chunk * dim
                if args.t4:
                    expert_producers.append(
                        prod_expert_t4(gguf, gate, up, down, i0, chunk,
                                       dim, inter, E))
                else:
                    expert_producers.append(
                        prod_expert_f32(gguf, gate, up, down, i0, chunk,
                                        dim, inter, E))
                expert_dir.append((l, e, (l * E + e) * expert_len,
                                   expert_len, fe,
                                   0x0030 if args.t4 else 0x0012, 0))
                fe += 1

        # ---- final norm + lm_head -------------------------------------
        # the engine reads final_norm as a 1-D vector of length dim - a
        # tensor that merely EXISTS with other shape would open and
        # compute garbage, so validate it (round-13, Qodo #24).
        if (norm_t.n_dims != 1 or list(norm_t.dims) != [dim]):
            die('output norm %s is %s, expected 1-D [dim=%d]'
                % (norm_t.name, list(norm_t.dims), dim))

        if args.probe:
            # header-only verdict: everything above was checked without
            # touching a payload byte; the server uses this to offer the
            # right import mode before spending a conversion.
            import json as _json
            all_q4_0 = all(gguf.find(n).ggml_type == gguf_reader.T_Q4_0
                           for n in weight_names)
            report = {
                'ok': True,
                'architecture': arch,
                'dim': dim,
                'n_heads': n_heads,
                'n_kv_heads': n_kv,
                'layers': layers,
                'inter': inter,
                'vocab': vocab,
                'experts': E,
                'has_output_tensor': output is not None,
                't4': {
                    # dim % 32 == 0 is the loud-refusal condition; t4 is
                    # the size-correct mode, all_q4_0 is the bit-exact one
                    'ok': dim % 32 == 0,
                    'all_q4_0': all_q4_0,
                },
                'source_bytes': os.path.getsize(args.in_gguf),
            }
            print(_json.dumps(report))
            return 0

        trunk.add('final_norm', 3, 0x0012, 1, dim, dim * 4,
                  prod_bytes(flat_f32(gguf, norm_t)))
        if output is not None:
            if args.t4:
                trunk.add('lm_head', 9, 0x0030, 2, vocab * dim,
                          vocab * dim * 18 // 32,
                          prod_t4(gguf, output))
            else:
                trunk.add('lm_head', 9, 0x0012, 2, vocab * dim,
                          vocab * dim * 4,
                          prod_flat_f32(gguf, output))

        # ---- vocabulary + merges (role 8, FORMAT-V2.md 3.2.3) ----------
        # Token normalisation (round-13, Qodo #15): tokenizer models
        # write the leading space as a GLYPH (llama/SPM: '\u2581...',
        # T5: '\u0120...') or as an escaped byte (GPT-2: '<0xNN>'). The
        # engine's byte->token map is BYTE-OPTIMAL single-char tokens,
        # so a glyph-stored space has no token at all and --text-prompt
        # refuses on every such model. Store the byte-equivalent string
        # instead (each glyph/escape names exactly one byte, at any
        # position in the token), which makes decoded output and text
        # prompts work for both families. Normalisation happens before
        # every id lookup, so merges (raw "left right" strings) are
        # normalised the same way.
        def normalize_token(t):
            # The space GLYPH (SPM/llama '\u2581', T5 '\u0120') and the
            # escaped byte '<0xNN>' (GPT-2) each stand for exactly one
            # byte - a space or the named byte - at ANY position in the
            # token string (a merge target can carry the glyph in the
            # middle: 'a' + '\u2581b' -> the token 'a\u2581b' means
            # 'a b'). Map every occurrence to the byte it names.
            if '\u2581' in t or '\u0120' in t or '<0x' in t:
                out = []
                i = 0
                while i < len(t):
                    if t.startswith('<0x', i) and i + 5 <= len(t) \
                            and t[i + 4] == '>':
                        try:
                            out.append(chr(int(t[i + 3:i + 4], 16)))
                            i += 5
                            continue
                        except ValueError:
                            pass
                    ch = t[i]
                    out.append(' ' if ch in ('\u2581', '\u0120') else ch)
                    i += 1
                return ''.join(out)
            return t

        tokens = [normalize_token(t) for t in tokens]
        # The engine's vocab loader (moe_load_vocab) REQUIRES: no empty
        # token, no embedded NUL, no duplicates. A GGUF that violates
        # any of these is rejected here at conversion time with a loud
        # message instead of producing a .hydra2 the engine refuses to
        # open (round-13, Qodo #2).
        seen = {}
        for i, t in enumerate(tokens):
            b = t.encode('utf-8')
            if b == b'':
                die('token %d is empty - the engine vocab loader cannot '
                    'store it' % i)
            if b'\x00' in b:
                die('token %d contains an embedded NUL byte - the '
                    'NUL-terminated vocab format cannot store it' % i)
            if t in seen:
                die('token %d (%r) duplicates token %d'
                    % (i, t, seen[t]))
            seen[t] = i
        # Byte-coverage report (not a refusal): bytes without a
        # single-byte token cannot be mapped by --text-prompt; the
        # engine still refuses them per-byte with a named message.
        covered = set()
        for t in tokens:
            b = t.encode('utf-8')
            if len(b) == 1:
                covered.add(b[0])
        if len(covered) < 256:
            missing = [b for b in range(256) if b not in covered]
            print('[gguf_to_hydra2] WARNING: %d of 256 bytes have no '
                  'single-byte token (first: %s); --text-prompt is '
                  'limited to the covered bytes; decoded output keeps '
                  'the stored (normalised) token strings'
                  % (256 - len(covered),
                     ', '.join('0x%02x' % b for b in missing[:8])))
        vocab_blob = b''.join(t.encode('utf-8') + b'\x00' for t in tokens)
        trunk.add('vocab', 8, 0x0050, 1, vocab, len(vocab_blob),
                  prod_bytes(vocab_blob))
        id_of = {t: i for i, t in enumerate(tokens)}
        merges = meta.get('tokenizer.ggml.merges')
        rules = []
        skipped = 0
        if isinstance(merges, list):
            for m in merges:
                if not isinstance(m, str) or ' ' not in m:
                    skipped += 1
                    continue
                left, _, right = m.partition(' ')
                li = id_of.get(normalize_token(left))
                ri = id_of.get(normalize_token(right))
                ti = id_of.get(normalize_token(left + right))
                if li is None or ri is None or ti is None:
                    skipped += 1  # not expressible as a token triple
                    continue
                rules.append((li, ri))
        if rules:
            # The merge rule format is 16-bit (left, right) token pairs
            # (FORMAT-V2.md 3.2.3). struct.pack('<HH') would RAISE for
            # an operand above 65535 (an uncaught traceback in the
            # middle of the conversion, after the expensive decoding) -
            # refuse before that point, loudly (round-13, Qodo #9).
            big = [id for pair in rules for id in pair if id > 0xFFFF]
            if big:
                die('merge operands exceed the 16-bit pair format: '
                    'token ids %s are above 65535 (vocab %d tokens); '
                    'this converter supports up to 65535 tokens per '
                    'merge rule' % (', '.join(str(i) for i in big[:5]),
                                    vocab))
            merges_blob = b''.join(struct.pack('<HH', a, b)
                                   for a, b in rules)
            trunk.add('merges', 8, 0x0051, 1, len(rules),
                      len(merges_blob), prod_bytes(merges_blob))

        # ---- metadata --------------------------------------------------
        meta_pairs = [
            ('v2.architecture', b'moe-transformer-v1'),
            ('v2.layers', u32(layers)),
            ('v2.moe_layers', u32(layers)),
            ('v2.top_k', u32(E)),
            ('v2.experts_per_layer', u32(E)),
            ('v2.router_dtype', u32(0x12)),
            ('v2.expert_bytes_typ',
             struct.pack('<Q', expert_len)),
            ('v2.router_runs_ahead', u32(1)),
            ('v2.dim', u32(dim)),
            ('v2.vocab_size', u32(vocab)),
            ('v2.n_heads', u32(n_heads)),
            ('v2.n_kv_heads', u32(n_kv)),
            ('v2.head_dim', u32(head_dim)),
            ('v2.inter', u32(inter)),
            ('v2.expert_layout',
             u32(2 if args.t4 else 0)),
            ('v2.rope_theta', f32(rope_theta)),
            ('v2.kv_sink', u32(2)),
            ('v2.kv_ring', u32(8)),
            ('v2.tied_embedding', u32(0 if output is not None else 1)),
            ('v2.bos_token', u32(bos)),
        ]
        meta_b = struct.pack('<I', len(meta_pairs))
        for key, val in meta_pairs:
            kb = key.encode('ascii')
            meta_b += struct.pack('<II', len(kb), len(val)) + kb + val

        # ---- pass 1 geometry: directories + section layout -----------
        # Mirrors make_model._assemble_v2.place() with SIZES ONLY -
        # every offset and the final file size are known before a
        # payload byte is written, so the header can go out first.
        tensor_dir = b''
        for (name, role, tid, ndims, elems, off, nbytes, _prod) in trunk.tensors:
            tensor_dir += name.encode('ascii').ljust(32, b'\x00')
            tensor_dir += struct.pack('<HHHH', role, tid, ndims, 0)
            tensor_dir += struct.pack('<Q', off)
            tensor_dir += struct.pack('<Q', elems)
            tensor_dir += struct.pack('<Q', nbytes)
        assert len(tensor_dir) == 64 * len(trunk.tensors)

        def ed_bytes(crcs):
            out = b''
            for idx, (l, e, off, length, feid, tid, _crc) in enumerate(
                    expert_dir):
                out += struct.pack('<II', l, e)
                out += struct.pack('<Q', off)
                out += struct.pack('<Q', length)
                out += struct.pack('<Q', feid)
                out += struct.pack('<II', tid,
                                   0 if crcs is None else crcs[idx])
                out += struct.pack('<Q', 0)
            return out

        ed0 = ed_bytes(None)
        assert len(ed0) == 48 * len(expert_dir)

        sec = 0

        def place_size(blob_len):
            nonlocal sec
            off = sec
            if off % 16:
                off += 16 - off % 16
            sec = off + blob_len
            return 128 + off

        meta_off = place_size(len(meta_b))
        td_off = place_size(len(tensor_dir))
        ed_off = place_size(len(ed0))
        trunk_off = place_size(trunk.total_bytes)
        experts_off = place_size(fe * expert_len)
        file_size = 128 + sec

        hdr = bytearray(128)
        hdr[0:8] = b'HYDRA2\x00\x00'
        struct.pack_into('<H', hdr, 8, 2)     # version_major
        struct.pack_into('<H', hdr, 10, 0)    # version_minor
        struct.pack_into('<I', hdr, 12, 128)  # header_bytes
        struct.pack_into('<Q', hdr, 16, file_size)
        struct.pack_into('<Q', hdr, 24, meta_off)
        struct.pack_into('<Q', hdr, 32, len(meta_b))
        struct.pack_into('<Q', hdr, 40, td_off)
        struct.pack_into('<Q', hdr, 48, len(trunk.tensors))
        struct.pack_into('<Q', hdr, 56, len(tensor_dir))
        struct.pack_into('<Q', hdr, 64, ed_off)
        struct.pack_into('<Q', hdr, 72, len(expert_dir))
        struct.pack_into('<Q', hdr, 80, len(ed0))
        struct.pack_into('<Q', hdr, 88, trunk_off)
        struct.pack_into('<Q', hdr, 96, trunk.total_bytes)
        struct.pack_into('<Q', hdr, 104, experts_off)
        struct.pack_into('<Q', hdr, 112, fe * expert_len)
        struct.pack_into('<I', hdr, 120, zlib.crc32(bytes(hdr[0:120])))
        struct.pack_into('<I', hdr, 124, 0)   # reserved

        # ---- pass 2: stream the payloads to disk ---------------------
        parent = os.path.dirname(os.path.abspath(args.out_hydra2))
        os.makedirs(parent, exist_ok=True)
        tmp = args.out_hydra2 + '.tmp'
        crcs = []
        with open(tmp, 'wb') as fh:
            fh.write(bytes(hdr))
            fh.write(b'\x00' * 0)  # (sections start right after the hdr)
            sec2 = 0

            def put(blob):
                nonlocal sec2
                if sec2 % 16:
                    fh.write(b'\x00' * (16 - sec2 % 16))
                    sec2 += 16 - sec2 % 16
                fh.write(blob)
                sec2 += len(blob)

            put(meta_b)
            put(tensor_dir)
            put(ed0)
            assert sec2 == trunk_off - 128
            # the directory offsets are RELATIVE to the trunk start
            tcur = 0
            for (name, role, tid, ndims, elems, off, nbytes, prod) in trunk.tensors:
                pad = off - tcur
                assert 0 <= pad < 16, name
                if pad:
                    fh.write(b'\x00' * pad)
                tcur = off
                prod(fh)
                tcur = off + nbytes
                sec2 = (trunk_off - 128) + tcur
            # align to the experts section start (place() padded the
            # unaligned trunk tail in the in-memory path too)
            pad = (experts_off - 128) - sec2
            assert 0 <= pad < 16
            if pad:
                fh.write(b'\x00' * pad)
                sec2 += pad
            assert sec2 == experts_off - 128
            for prod in expert_producers:
                sink = _CrcSink(fh)
                prod(sink)
                crcs.append(sink.crc)
                sec2 += expert_len
            assert sec2 == experts_off - 128 + fe * expert_len
            # patch the expert directory with the real payload CRCs
            # (the header CRC covers bytes 0..119 only, so it stays
            # valid after this patch)
            fh.seek(ed_off)
            fh.write(ed_bytes(crcs))
        os.replace(tmp, args.out_hydra2)
        print('[gguf_to_hydra2] %s -> %s: %d bytes (llama %d layers, '
              'dim %d, heads %d/%d, inter %d, vocab %d, %d experts/layer '
              'x top_k %d, %d merge rules written, %d skipped)'
              % (args.in_gguf, args.out_hydra2, file_size, layers, dim,
                 n_heads, n_kv, inter, vocab, E, E, len(rules), skipped))


if __name__ == '__main__':
    main()

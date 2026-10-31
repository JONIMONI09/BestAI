#!/usr/bin/env python3
"""Write a Hydra-Stone starter model.

    python3 tools/make_model.py models/starter.hydra
    python3 tools/make_model.py out.hydra2 --v2 [--layers 2] [--experts 4]
        [--top-k 2] [--hidden 8]

This is the format reference: it writes the 24-byte v1 header followed by the
packed ternary block exactly as tools/hydra_train.js and the C loader agree
on it, so a file it produces is a file the engine loads.

The weights are a deterministic seeded pattern rather than noise, because a
model whose behaviour cannot be re-derived is a bad thing to ship inside an
APK and to hand to somebody debugging a first run. The pattern alternates
+1/-1 in plane w1 and 0/+1 in plane w2 with a fixed LCG, which gives the
engine a non-trivial aggregation instead of the degenerate all-zero case that
a zeroed buffer would produce.

DIMENSION 0 IS THE ONE THAT MATTERS. The token decoder reads a single value:

    acc[0] = A[0]*token + B[0]*state[0]

so a model whose column-0 sums are both zero is *state-blind*: the recurrence
contributes nothing, the output depends only on the seed token, and
hydra_engine_prefill() can have no observable effect at all. A previous
version of this generator produced exactly that (B[0] == 0) while still being
correct, which is why it shipped a starter model that looped 471, 42, 471, 42
forever and made the CI prompt check fail with the misleading message "a longer
prompt must change the output" - the prompt path was fine, the fixture was not.

build() therefore GUARANTEES A[0] != 0 and B[0] != 0 by construction, and the
guarantee is asserted rather than assumed.

It is NOT a trained model and does not claim to be one: 4 layers x 64 dims is
a working starter, not a language model. Training a real one is what
tools/hydra_train.js and POST /api/train are for.
"""

import argparse
import math
import os
import struct

MAGIC = 0x48594452  # "HYDR"
VERSION = 1
HEADER_SIZE = 24
VOCAB = 512
DIM = 64
LAYERS = 4


def lcg(state):
    """The same 31-bit LCG tests/test_engine.c uses, so the pattern here is
    reproducible from the C side as well."""
    return (1103515245 * state + 12345) & 0x7FFFFFFF


def build(vocab=VOCAB, dim=DIM, layers=LAYERS):
    """Return the complete file as bytes."""
    header = struct.pack('<IHHIIII', MAGIC, VERSION, vocab, dim, layers,
                         HEADER_SIZE, dim * layers)
    state = 20240701
    body = bytearray(dim * layers)
    for i in range(len(body)):
        state = lcg(state)
        w1 = (state >> 16) % 3          # 0, 1 or 2 -> 0, +1 or -1
        state = lcg(state)
        w2 = ((state >> 16) % 3 + 1) % 3  # shifted, so plane w2 is not a copy
        body[i] = (w1 & 0x03) | ((w2 & 0x03) << 2)

    # Force a non-zero column-0 sum in BOTH planes (see the module docstring).
    # Setting one entry to +1 is not enough: the remaining layers can cancel
    # it. So the last layer's column-0 entry is chosen so that the *total* is
    # non-zero - take +1 unless that would cancel a sum of exactly -1, in
    # which case take -1. Only that one entry in each plane is touched, so the
    # file is still a pure function of (vocab, dim, layers).
    value = {0: 0, 1: 1, 2: -1, 3: 0}          # the C loader's codeToValue
    code = {0: 0, 1: 1, 2: -1}

    last = (layers - 1) * dim                    # (layer layers-1, dim 0)
    for shift, mask in ((0, 0xFC), (2, 0xF3)):   # plane w1, then plane w2
        others = sum(value[(body[l * dim] >> shift) & 0x03]
                     for l in range(layers - 1))
        want = -1 if others == -1 else 1         # never let the total reach 0
        enc = next(c for c, v in code.items() if v == want)
        body[last] = (body[last] & mask) | (enc << shift)

    # Assert the invariant the whole starter model depends on, so a future edit
    # to the pattern cannot quietly reintroduce a state-blind model.
    col_a = sum(value[body[l * dim] & 0x03] for l in range(layers))
    col_b = sum(value[(body[l * dim] >> 2) & 0x03] for l in range(layers))
    assert col_a != 0 and col_b != 0, (
        'column 0 must reach the decoder: A[0]=%d B[0]=%d' % (col_a, col_b))

    return header + bytes(body)



# ---------------------------------------------------------------------------
# v2 .hydra2 sparse-MoE fixture (docs/FORMAT-V2.md).
#
# This writes the container the v2 reader (src/hydra_v2_container.c)
# validates: 128-byte header, metadata K/V block, 64-byte tensor directory
# entries over the resident trunk, 48-byte expert directory entries over the
# streamed payloads. Everything is a deterministic function of the seed -
# the same guarantee the v1 builder makes, and the C test
# tests/test_hydra2_container.c cross-checks the layout field by field.
#
# The metadata K/V wire format (u32 count, then u32 key_len, u32 val_len,
# key, value per pair) is the one src/hydra_v2_container.c implements; it is
# recorded in FORMAT-V2.md section 4.4.
# ---------------------------------------------------------------------------

import struct as _struct  # already imported above; alias kept local


def _crc32(data):
    import zlib
    return zlib.crc32(data) & 0xFFFFFFFF


def _pad16(b):
    if len(b) % 16:
        b = b + b'\x00' * (16 - len(b) % 16)
    return b


def build_v2(layers=2, experts=4, top_k=2, hidden=8, vocab=16,
             expert_elems=16, seed=20240702):
    """Return the complete .hydra2 file as bytes (all F32 payloads)."""
    if not 1 <= layers <= 64:
        raise ValueError('layers must be 1..64')
    if not 1 <= experts <= 64:
        raise ValueError('experts must be 1..64')
    if not 1 <= top_k <= experts:
        raise ValueError('top_k must be 1..experts')

    state = [seed]

    def f32s(n):
        out = bytearray()
        for _ in range(n):
            state[0] = lcg(state[0])
            # small deterministic logits/weights in [-1, 1]
            v = ((state[0] >> 8) % 4096) / 2048.0 - 1.0
            out += _struct.pack('<f', v)
        return bytes(out)

    # ---- trunk tensors (resident): per-layer router + embedding + norm
    trunk = bytearray()
    tensors = []  # (name, role, type_id, n_dims, n_elements, payload)

    def add_trunk(name, role, n_dims, elems, payload, type_id=0x12):
        off = len(trunk)
        if off % 16:
            pad = 16 - off % 16
            trunk.extend(b'\x00' * pad)
            off += pad
        trunk.extend(payload)
        tensors.append((name, role, type_id, n_dims, elems, off,
                        len(payload)))

    for l in range(layers):
        add_trunk('router.%d' % l, 1, 2, experts * hidden,
                  f32s(experts * hidden))
    add_trunk('embed', 2, 2, vocab * hidden, f32s(vocab * hidden))
    add_trunk('norm', 3, 1, hidden, f32s(hidden))
    trunk = bytes(trunk)

    # ---- expert payloads (streamed), sorted (layer, expert_id),
    # offsets contiguous in that order (FORMAT-V2.md 4.3).
    expert_payloads = []
    for l in range(layers):
        for e in range(experts):
            expert_payloads.append(f32s(expert_elems))
    expert_dir = []
    exp_off = 0
    first_element = 0
    for l in range(layers):
        for e in range(experts):
            payload = expert_payloads[l * experts + e]
            expert_dir.append((l, e, exp_off, len(payload), first_element,
                               0x12, _crc32(payload)))
            exp_off += len(payload)
            first_element += expert_elems

    # ---- metadata K/V block
    meta_pairs = [
        ('v2.architecture', b'moe-transformer-v1'),
        ('v2.layers', _struct.pack('<I', layers)),
        ('v2.moe_layers', _struct.pack('<I', layers)),
        ('v2.top_k', _struct.pack('<I', top_k)),
        ('v2.experts_per_layer', _struct.pack('<I', experts)),
        ('v2.router_dtype', _struct.pack('<I', 0x12)),
        ('v2.expert_bytes_typ', _struct.pack('<Q', expert_elems * 4)),
        ('v2.router_runs_ahead', _struct.pack('<I', 1)),
    ]
    meta = _struct.pack('<I', len(meta_pairs))
    for k, v in meta_pairs:
        kb = k.encode('ascii')
        meta += _struct.pack('<II', len(kb), len(v)) + kb + v
    meta = meta.encode('latin1') if isinstance(meta, str) else bytes(meta)

    # ---- assemble (shared with build_moe_full) ----
    experts_blob = b''.join(expert_payloads)
    assert len(experts_blob) == exp_off
    return _assemble_v2(bytes(meta), tensors, expert_dir, trunk,
                         experts_blob)


def _assemble_v2(meta, tensors, expert_dir, trunk, experts_blob):
    """Pack header + sections for a .hydra2 file. The byte layout is
    the FORMAT-V2.md section 4 layout; build_v2 and build_moe_full both
    use it so the two writers cannot drift apart.
    tensors: (name, role, type_id, n_dims, n_elements, offset)
    expert_dir: (layer, expert_id, offset, length, first_element, tid, crc)
    """
    hdr = bytearray(128)
    hdr[0:8] = b'HYDRA2\x00\x00'
    _struct.pack_into('<H', hdr, 8, 2)    # version_major
    _struct.pack_into('<H', hdr, 10, 0)   # version_minor
    _struct.pack_into('<I', hdr, 12, 128) # header_bytes
    _struct.pack_into('<I', hdr, 120, 0)  # header_crc32 (filled below)
    _struct.pack_into('<I', hdr, 124, 0)  # reserved

    tensor_dir = b''
    for (name, role, tid, ndims, elems, off, plen) in tensors:
        # n_bytes is the REAL payload size: elems*4 for the fixed-size
        # types, the byte count for VOCAB_UTF8 (variable).
        nb = plen
        tensor_dir += name.encode('ascii').ljust(32, b'\x00')
        tensor_dir += _struct.pack('<HHHH', role, tid, ndims, 0)
        tensor_dir += _struct.pack('<Q', off)
        tensor_dir += _struct.pack('<Q', elems)
        tensor_dir += _struct.pack('<Q', nb)
    assert len(tensor_dir) == 64 * len(tensors)

    expert_dir_b = b''
    for (l, e, off, length, fe, tid, crc) in expert_dir:
        expert_dir_b += _struct.pack('<II', l, e)
        expert_dir_b += _struct.pack('<Q', off)
        expert_dir_b += _struct.pack('<Q', length)
        expert_dir_b += _struct.pack('<Q', fe)
        expert_dir_b += _struct.pack('<II', tid, crc)
        expert_dir_b += _struct.pack('<Q', 0)  # reserved
    assert len(expert_dir_b) == 48 * len(expert_dir)


    def place(blob):
        nonlocal sections
        off = 128 + len(sections)   # offsets are file-absolute (header is 128)
        if off % 16:
            pad = 16 - off % 16
            sections += b'\x00' * pad
            off += pad
        sections += blob
        return off, len(blob)

    sections = b''
    meta_off, meta_len = place(bytes(meta))
    td_off, td_len = place(bytes(tensor_dir))
    ed_off, ed_len = place(bytes(expert_dir_b))
    trunk_off, trunk_len = place(bytes(trunk))
    experts_off, experts_len = place(bytes(experts_blob))
    file_size = 128 + len(sections)

    _struct.pack_into('<Q', hdr, 16, file_size)
    _struct.pack_into('<Q', hdr, 24, meta_off)
    _struct.pack_into('<Q', hdr, 32, meta_len)
    _struct.pack_into('<Q', hdr, 40, td_off)
    _struct.pack_into('<Q', hdr, 48, len(tensors))
    _struct.pack_into('<Q', hdr, 56, td_len)
    _struct.pack_into('<Q', hdr, 64, ed_off)
    _struct.pack_into('<Q', hdr, 72, len(expert_dir))
    _struct.pack_into('<Q', hdr, 80, ed_len)
    _struct.pack_into('<Q', hdr, 88, trunk_off)
    _struct.pack_into('<Q', hdr, 96, trunk_len)
    _struct.pack_into('<Q', hdr, 104, experts_off)
    _struct.pack_into('<Q', hdr, 112, experts_len)
    _struct.pack_into('<I', hdr, 120, _crc32(bytes(hdr[0:120])))

    return bytes(hdr) + sections


def build_moe_full(layers=1, experts=4, top_k=2, dim=8, n_heads=2,
                   n_kv_heads=1, head_dim=4, inter=4, vocab=16,
                   rope_theta=10000.0, kv_sink=2, kv_ring=8,
                   tied_embedding=1, bos_token=0, seed=20240703,
                   ternary_group=0, include_vocab=True,
                   attn_group=0, q4=False):
    """Return a COMPLETE-CONTRACT .hydra2 file for the moe-transformer-v1
    inference engine (src/hydra_moe.c, round 12):

      - every trunk tensor the forward pass reads, with the MoE roles
        (7=ATTN for attn_q/k/v/o, 3=NORM for the per-layer and final
        norms, 1=ROUTER, 2=EMBEDDING, 9=LM_HEAD when untied),
      - the v2.* inference metadata the engine enforces,
      - the expert payloads with the engine's flat F32 geometry
        [gate | up | down] = 3 * inter * dim elements per expert, or
        (ternary_group in {64, 128}) the same three sub-matrices as
        TERNARY2 + per-group F32 scales (v2.expert_layout 1): each
        sub-matrix block is [payload bytes][scale F32 array], the codes
        use the exact TERNARY2 table (00=0, 01=+1, 10=-1, 11=0, 2 codes
        per byte, element 2j in the low bits) and one scale covers a
        group of `group` consecutive columns of one row (the last group
        may be partial); scale = max|w| of the group, code +1 when
        w >= 0.5*scale, -1 when w <= -0.5*scale, else 0 (all-zero group
        -> scale 0.0, code 0).

    All values are the same deterministic LCG stream as build_v2 (a
    different seed), so a given parameter set always produces the same
    bytes - the C test suite pins the token stream of one such file.
    """
    if not 1 <= layers <= 16:
        raise ValueError('layers must be 1..16')
    if ternary_group not in (0, 64, 128):
        raise ValueError('ternary_group must be 0, 64 or 128')
    if not 1 <= experts <= 16:
        raise ValueError('experts must be 1..16')
    if not 1 <= top_k <= experts:
        raise ValueError('top_k must be 1..experts')
    if not 1 <= n_kv_heads <= n_heads:
        raise ValueError('n_kv_heads must be 1..n_heads')
    if n_heads % n_kv_heads != 0:
        # GQA block mapping (head i -> kv head i / (n_heads /
        # n_kv_heads)) only exists for divisible counts; the engine
        # refuses such files at open (round 13, Qodo #26).
        raise ValueError('n_heads must be divisible by n_kv_heads (GQA)')
    if n_heads * head_dim != dim:
        raise ValueError('n_heads * head_dim must equal dim')
    if not 2 <= vocab <= 256:
        raise ValueError('vocab must be 2..256')
    if q4 and (dim % 32 or inter % 32):
        raise ValueError('q4 mode needs dim and inter divisible by 32 '
                         '(a Q4_0 row holds whole 32-value blocks)')

    state = [seed]

    def f32s(n):
        out = bytearray()
        for _ in range(n):
            state[0] = lcg(state[0])
            v = ((state[0] >> 8) % 4096) / 2048.0 - 1.0
            out += _struct.pack('<f', v)
        return bytes(out)

    # ---- trunk tensors -------------------------------------------------
    trunk = bytearray()
    tensors = []

    def add_trunk(name, role, n_dims, elems, payload, type_id=0x12):
        off = len(trunk)
        if off % 16:
            pad = 16 - off % 16
            trunk.extend(b'\x00' * pad)
            off += pad
        trunk.extend(payload)
        tensors.append((name, role, type_id, n_dims, elems, off,
                        len(payload)))

    def f32_vals(n):
        out = []
        for _ in range(n):
            state[0] = lcg(state[0])
            out.append(((state[0] >> 8) % 4096) / 2048.0 - 1.0)
        return out

    def q40_block(vals, rows, cols):
        """Q4_0 block encoding of a row-major (rows x cols) F32 matrix
        (round 16, FORMAT-V2.md 3.2): 18-byte blocks of 32 consecutive
        values in row-major order - F16 scale d = round-half-even of
        max|w| / 7 (struct '<e'), then 16 bytes carrying two 4-bit
        codes each (value 2j in the LOW nibble of byte j, 2j+1 in the
        HIGH nibble); code = clamp(floor(w/d + 8.5), 0, 15)
        (round-half-up, writer's convention), so w ~ d * (code - 8).
        d == 0 (all-zero block) -> every code 8 -> exact zeros on
        decode. cols must be a multiple of 32."""
        n = rows * cols
        if n % 32:
            raise ValueError('q40 sub-matrix rows*cols must be a '
                             'multiple of 32')
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

    def ternary_flat(vals, group):
        """FLAT group convention for plain trunk tensors (FORMAT-V2.md
        3.2.1): one F32 scale per `group` consecutive elements in
        row-major order; the last group may be partial (ceil(n/group)
        scales). Returns (payload, scale_bytes, ngroups): the tensor
        stores the codes only, the scales ride in the paired F32
        tensor '<name>.scale'."""
        n = len(vals)
        if n % 2:
            raise ValueError('ternary trunk tensor n must be even')
        ngroups = (n + group - 1) // group
        codes = bytearray(n // 2)
        scales = []
        for g in range(ngroups):
            gw = vals[g * group:(g + 1) * group]
            s = max((abs(x) for x in gw), default=0.0)
            for i, x in enumerate(gw):
                if s == 0.0:
                    t = 0
                elif x >= 0.5 * s:
                    t = 1
                elif x <= -0.5 * s:
                    t = 2
                else:
                    t = 0
                k = g * group + i
                codes[k // 2] |= (t & 0x3) << ((k % 2) * 2)
            scales.append(s)
        return (bytes(codes),
                b''.join(_struct.pack('<f', x) for x in scales),
                ngroups)

    def attn_tid(group):
        return 0x40 if group == 64 else 0x41

    def q4s(rows, cols):
        """Q4_0 payload for a (rows x cols) matrix drawn from the SAME
        LCG stream f32s() would fill (values identical, only encoded
        as 4-bit blocks). The small vectors (norms, router) stay F32 -
        the same convention real GGUFs use (small tensors float)."""
        vals = f32_vals(rows * cols)
        return q40_block(vals, rows, cols)

    add_trunk('embed', 2, 2, vocab * dim,
              q4s(vocab, dim) if q4 else f32s(vocab * dim),
              0x0030 if q4 else 0x12)
    for l in range(layers):
        attn_specs = (('attn_q.%d' % l, dim, dim),
                      ('attn_k.%d' % l, n_kv_heads * head_dim, dim),
                      ('attn_v.%d' % l, n_kv_heads * head_dim, dim),
                      ('attn_o.%d' % l, dim, dim))
        for nm, r, c in attn_specs:
            if not attn_group:
                if q4:
                    add_trunk(nm, 7, 2, r * c, q4s(r, c), 0x0030)
                else:
                    add_trunk(nm, 7, 2, r * c, f32s(r * c))
            else:
                vals = f32_vals(r * c)
                p, sc, ng = ternary_flat(vals, attn_group)
                add_trunk(nm, 7, 1, r * c, p, attn_tid(attn_group))
                add_trunk(nm + '.scale', 3, 1, ng, sc, 0x12)
        add_trunk('attn_norm.%d' % l, 3, 1, dim, f32s(dim))
        add_trunk('ffn_norm.%d' % l, 3, 1, dim, f32s(dim))
        add_trunk('router.%d' % l, 1, 2, experts * dim, f32s(experts * dim))
        add_trunk('ffn_gate.%d' % l, 0, 2, inter * dim,
                  q4s(inter, dim) if q4 else f32s(inter * dim),
                  0x0030 if q4 else 0x12)
        add_trunk('ffn_up.%d' % l, 0, 2, inter * dim,
                  q4s(inter, dim) if q4 else f32s(inter * dim),
                  0x0030 if q4 else 0x12)
        add_trunk('ffn_down.%d' % l, 0, 2, dim * inter,
                  q4s(dim, inter) if q4 else f32s(dim * inter),
                  0x0030 if q4 else 0x12)
    add_trunk('final_norm', 3, 1, dim, f32s(dim))
    if not tied_embedding:
        add_trunk('lm_head', 9, 2, vocab * dim,
                  q4s(vocab, dim) if q4 else f32s(vocab * dim),
                  0x0030 if q4 else 0x12)

    # ---- vocabulary + BPE merges (role 8, FORMAT-V2.md 3.2.3) --------
    # Deterministic mini-vocabulary: single letters a..e, the BPE words
    # "ab"/"cd"/"bc", then generic "tok%04d" strings. Merge rules, in
    # priority order: (a,b)->"ab", (c,d)->"cd", (b,c)->"bc". A rule is
    # emitted only when its target token exists, so the construction
    # stays valid for every vocab size; below 8 tokens there are no
    # merges and the 'merges' tensor is omitted. The payload is
    # byte-identical for a given vocab, and it does NOT consume LCG
    # state, so the numeric fixture (and every pinned stream) is
    # unaffected by include_vocab.
    if include_vocab:
        def vocab_string(i):
            if i < 5:
                return chr(ord('a') + i)
            if i == 5:
                return 'ab'
            if i == 6:
                return 'cd'
            if i == 7:
                return 'bc'
            return 'tok%04d' % i
        words = [vocab_string(i).encode('utf-8') for i in range(vocab)]
        vocab_blob = b''.join(w + b'\x00' for w in words)
        add_trunk('vocab', 8, 1, vocab, vocab_blob, 0x0050)
        pair = lambda a, b, t: (a, b) if t < vocab else None
        rules = [r for r in
                 (pair(0, 1, 5), pair(2, 3, 6), pair(1, 2, 7))
                 if r is not None]
        if rules:
            merges_blob = b''.join(
                _struct.pack('<HH', a, b) for a, b in rules)
            add_trunk('merges', 8, 1, len(rules), merges_blob, 0x0051)

    # ---- expert payloads (streamed), sorted (layer, expert_id) --------
    # Same LCG draw as f32s() (values are identical, only collected as
    # floats first) so the F32 byte stream stays bit-identical to the
    # pre-ternary writer.
    expert_elems = 3 * inter * dim
    expert_type = 0x40 if ternary_group == 64 else (
        0x41 if ternary_group == 128 else (
            0x0030 if q4 else 0x12))

    def ternary_block(vals, rows, cols, group):
        """TERNAry2 + block-scale encoding of a row-major (rows x cols)
        F32 matrix (FORMAT-V2.md 3.2, R12 stage 5). Returns
        (payload_bytes, scale_bytes): the payload is the exact TERNARY2
        packing (2 codes/byte, element 2j in the LOW bits,
        00=0, 01=+1, 10=-1, 11=0) and the scales are F32 in row-major
        [row][group] order, one per group of `group` consecutive
        columns (last group may be partial)."""
        n = rows * cols
        if n % 2:
            raise ValueError('ternary sub-matrix rows*cols must be even '
                             '(TERNAry2 = 2 elements/byte)')
        ngroups = (cols + group - 1) // group
        codes = bytearray(n // 2)
        scales = []
        for r in range(rows):
            for g in range(ngroups):
                gw = vals[r * cols + g * group: r * cols + min(cols, (g + 1) * group)]
                s = max((abs(x) for x in gw), default=0.0)
                for i, x in enumerate(gw):
                    if s == 0.0:
                        t = 0
                    elif x >= 0.5 * s:
                        t = 1
                    elif x <= -0.5 * s:
                        t = 2
                    else:
                        t = 0
                    k = r * cols + g * group + i
                    # EXACT v1 TERNARY2 packing (2 codes/byte): element 2j
                    # in the LOW 2 bits of byte j, element 2j+1 in bits 2-3.
                    codes[k // 2] |= (t & 0x3) << ((k % 2) * 2)
                scales.append(s)
        return (bytes(codes),
                b''.join(_struct.pack('<f', x) for x in scales))

    expert_payloads = []
    for l in range(layers):
        for e in range(experts):
            vals = f32_vals(expert_elems)
            if not ternary_group and not q4:
                expert_payloads.append(
                    b''.join(_struct.pack('<f', v) for v in vals))
            else:
                gate = vals[0:inter * dim]
                up = vals[inter * dim:2 * inter * dim]
                down = vals[2 * inter * dim:]
                parts = []
                for mv, r, c in ((gate, inter, dim), (up, inter, dim),
                                 (down, dim, inter)):
                    if q4:
                        # layout 2: raw [gate][up][down] Q4_0 blocks,
                        # no interleaved scales (the scale rides in
                        # every 18-byte block).
                        parts.append(q40_block(mv, r, c))
                    else:
                        p, sc = ternary_block(mv, r, c, ternary_group)
                        parts.append(p)
                        parts.append(sc)
                expert_payloads.append(b''.join(parts))
    expert_dir = []
    exp_off = 0
    first_element = 0
    for l in range(layers):
        for e in range(experts):
            payload = expert_payloads[l * experts + e]
            expert_dir.append((l, e, exp_off, len(payload), first_element,
                               expert_type, _crc32(payload)))
            exp_off += len(payload)
            first_element += expert_elems

    # ---- metadata K/V block (container-required + v2.* inference) -----
    meta_pairs = [
        ('v2.architecture', b'moe-transformer-v1'),
        ('v2.layers', _struct.pack('<I', layers)),
        ('v2.moe_layers', _struct.pack('<I', layers)),
        ('v2.top_k', _struct.pack('<I', top_k)),
        ('v2.experts_per_layer', _struct.pack('<I', experts)),
        ('v2.router_dtype', _struct.pack('<I', 0x12)),
        ('v2.expert_bytes_typ', _struct.pack('<Q', len(expert_payloads[0]))),
        ('v2.router_runs_ahead', _struct.pack('<I', 1)),
        ('v2.dim', _struct.pack('<I', dim)),
        ('v2.vocab_size', _struct.pack('<I', vocab)),
        ('v2.n_heads', _struct.pack('<I', n_heads)),
        ('v2.n_kv_heads', _struct.pack('<I', n_kv_heads)),
        ('v2.head_dim', _struct.pack('<I', head_dim)),
        ('v2.inter', _struct.pack('<I', inter)),
        ('v2.expert_layout', _struct.pack('<I',
                                 1 if ternary_group else (2 if q4 else 0))),
        ('v2.rope_theta', _struct.pack('<f', rope_theta)),
        ('v2.kv_sink', _struct.pack('<I', kv_sink)),
        ('v2.kv_ring', _struct.pack('<I', kv_ring)),
        ('v2.tied_embedding', _struct.pack('<I', tied_embedding)),
        ('v2.bos_token', _struct.pack('<I', bos_token)),
    ]
    meta = _struct.pack('<I', len(meta_pairs))
    for k, v in meta_pairs:
        kb = k.encode('ascii')
        meta += _struct.pack('<II', len(kb), len(v)) + kb + v
    meta = bytes(meta)

    trunk = bytes(trunk)
    return _assemble_v2(meta, tensors, expert_dir, trunk,
                        b''.join(expert_payloads))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('out', nargs='?', default='models/starter.hydra')
    ap.add_argument('--dim', type=int, default=None,
                    help='model dimension: v1 default %d; moe-full '
                         'defaults to --n-heads * --head-dim when omitted'
                         % DIM)
    ap.add_argument('--layers', type=int, default=LAYERS)
    ap.add_argument('--vocab', type=int, default=VOCAB)
    ap.add_argument('--v2', action='store_true',
                    help='write a .hydra2 sparse-MoE container instead of '
                         'a v1 starter model')
    ap.add_argument('--experts', type=int, default=4,
                    help='v2: experts per layer')
    ap.add_argument('--top-k', type=int, default=2,
                    help='v2: routing top_k (<= experts)')
    ap.add_argument('--hidden', type=int, default=8,
                    help='v2: hidden size of the fixture model')
    ap.add_argument('--moe-full', action='store_true',
                    help='v2: complete-contract fixture for the '
                         'moe-transformer-v1 inference engine')
    ap.add_argument('--n-heads', type=int, default=2,
                    help='moe-full: attention heads')
    ap.add_argument('--n-kv-heads', type=int, default=1,
                    help='moe-full: kv heads (GQA)')
    ap.add_argument('--head-dim', type=int, default=4,
                    help='moe-full: head dimension (n_heads*head_dim == --dim-of)')
    ap.add_argument('--inter', type=int, default=4,
                    help='moe-full: FFN width')
    ap.add_argument('--moe-no-vocab', action='store_true',
                    help='moe-full: omit the vocabulary/merges tensors '
                         '(inference-only file; no text codec)')
    ap.add_argument('--moe-ternary', type=int, nargs='?', const=64,
                    default=0, choices=[64, 128],
                    help='moe-full: store the expert payloads as '
                         'TERNARY2 + per-group F32 scales (group 64 or '
                         '128; default 64 when the flag is bare)')
    ap.add_argument('--moe-attn-ternary', type=int, nargs='?', const=64,
                    default=0, choices=[64, 128],
                    help='moe-full: store the attention matrices '
                         '(attn_q/k/v/o) as TERNARY2 with a paired F32 '
                         '<name>.scale trunk tensor (flat group '
                         'convention, group 64 or 128)')
    ap.add_argument('--moe-q4', action='store_true',
                    help='moe-full: store the large matrices (embed, '
                         'attn_q/k/v/o, ffn_gate/up/down, lm_head) as '
                         'Q4_0 blocks (18 bytes per 32 values, F16 '
                         'scale, w = d*(q-8)) and the experts as layout '
                         '2 (raw [gate][up][down] Q4_0 payloads); norms '
                         'and router stay F32. Needs dim and inter '
                         'divisible by 32 (round 16).')
    args = ap.parse_args()

    if args.moe_full:
        if not 1 <= args.layers <= 16:
            ap.error('moe-full --layers must be between 1 and 16')
        if not 1 <= args.experts <= 16:
            ap.error('moe-full --experts must be between 1 and 16')
        if not 1 <= args.top_k <= args.experts:
            ap.error('moe-full --top-k must be 1..--experts')
        moe_dim = (args.dim if args.dim is not None
                   else args.n_heads * args.head_dim)
        data = build_moe_full(layers=args.layers, experts=args.experts,
                              top_k=args.top_k,
                              dim=moe_dim,
                              n_heads=args.n_heads,
                              n_kv_heads=args.n_kv_heads,
                              head_dim=args.head_dim,
                              inter=args.inter,
                              ternary_group=args.moe_ternary,
                              include_vocab=not args.moe_no_vocab,
                              attn_group=args.moe_attn_ternary,
                              q4=args.moe_q4)
        parent = os.path.dirname(os.path.abspath(args.out))
        os.makedirs(parent, exist_ok=True)
        tmp = args.out + '.tmp'
        with open(tmp, 'wb') as fh:
            fh.write(data)
        os.replace(tmp, args.out)
        print('[make_model] %s: %d bytes (moe-full: %d layers x %d experts, '
              'top_k %d, dim %d, inter %d%s)'
              % (args.out, len(data), args.layers, args.experts,
                 args.top_k, args.n_heads * args.head_dim, args.inter,
                 ', experts TERNARY2+S%d' % args.moe_ternary
                 if args.moe_ternary else ''))
        return

    if args.v2:
        if not 1 <= args.layers <= 64:
            ap.error('v2 --layers must be between 1 and 64')
        if not 1 <= args.experts <= 64:
            ap.error('v2 --experts must be between 1 and 64')
        if not 1 <= args.top_k <= args.experts:
            ap.error('v2 --top-k must be between 1 and --experts')
        if not 1 <= args.hidden <= 256:
            ap.error('v2 --hidden must be between 1 and 256')
        data = build_v2(layers=args.layers, experts=args.experts,
                        top_k=args.top_k, hidden=args.hidden)
        parent = os.path.dirname(os.path.abspath(args.out))
        os.makedirs(parent, exist_ok=True)
        tmp = args.out + '.tmp'
        with open(tmp, 'wb') as fh:
            fh.write(data)
        os.replace(tmp, args.out)
        print('[make_model] %s: %d bytes (v2: %d layers x %d experts, '
              'top_k %d, hidden %d)'
              % (args.out, len(data), args.layers, args.experts,
                 args.top_k, args.hidden))
        return

    if args.dim is None:
        args.dim = DIM
    if not 1 <= args.dim <= 64:
        ap.error('--dim must be between 1 and 64 (HYDRA_EMBED_DIM)')
    if not 1 <= args.layers <= 4096:
        ap.error('--layers must be between 1 and 4096 (HYDRA_MAX_LAYERS)')
    if not 2 <= args.vocab <= 1024:
        ap.error('--vocab must be between 2 and 1024 (HYDRA_MAX_VOCAB)')

    data = build(args.vocab, args.dim, args.layers)
    parent = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(parent, exist_ok=True)
    tmp = args.out + '.tmp'
    with open(tmp, 'wb') as fh:
        fh.write(data)
    os.replace(tmp, args.out)
    print('[make_model] %s: %d bytes (vocab %d, dim %d, layers %d)'
          % (args.out, len(data), args.vocab, args.dim, args.layers))


if __name__ == '__main__':
    main()
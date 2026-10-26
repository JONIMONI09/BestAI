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

    def add_trunk(name, role, n_dims, elems, payload):
        off = len(trunk)
        if off % 16:
            pad = 16 - off % 16
            trunk.extend(b'\x00' * pad)
            off += pad
        trunk.extend(payload)
        tensors.append((name, role, 0x12, n_dims, elems, off))

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

    # ---- assemble
    hdr = bytearray(128)
    hdr[0:8] = b'HYDRA2\x00\x00'
    _struct.pack_into('<H', hdr, 8, 2)    # version_major
    _struct.pack_into('<H', hdr, 10, 0)   # version_minor
    _struct.pack_into('<I', hdr, 12, 128) # header_bytes
    _struct.pack_into('<I', hdr, 120, 0)  # header_crc32 (filled below)
    _struct.pack_into('<I', hdr, 124, 0)  # reserved

    tensor_dir = b''
    for (name, role, tid, ndims, elems, off) in tensors:
        nb = elems * 4
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

    experts_blob = b''.join(expert_payloads)
    assert len(experts_blob) == exp_off

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


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('out', nargs='?', default='models/starter.hydra')
    ap.add_argument('--dim', type=int, default=DIM)
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
    args = ap.parse_args()

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
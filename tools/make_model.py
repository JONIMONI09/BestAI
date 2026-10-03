#!/usr/bin/env python3
"""Write a Hydra-Stone starter model.

    python3 tools/make_model.py models/starter.hydra

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


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('out', nargs='?', default='models/starter.hydra')
    ap.add_argument('--dim', type=int, default=DIM)
    ap.add_argument('--layers', type=int, default=LAYERS)
    ap.add_argument('--vocab', type=int, default=VOCAB)
    args = ap.parse_args()

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
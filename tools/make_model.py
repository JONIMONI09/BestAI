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
engine a non-trivial aggregation (A and B both non-zero) instead of the
degenerate all-zero case that a zeroed buffer would produce.

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
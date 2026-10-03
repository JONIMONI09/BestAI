#!/usr/bin/env python3
"""Measure the GGUF converter on a realistically sized tensor.

    python3 tools/gguf_speed.py [--rows 4096] [--cols 4096] [--type F32]
    python3 tools/gguf_speed.py --rows 32000 --cols 4096   # a real embedding

Prints wall time, throughput and peak RSS, because "it converts" says
nothing about whether it is fast enough for a 4 GB download. The numbers
docs/GGUF-IMPORT.md quotes come from this tool - re-run it rather than
quoting a stale value.

The fixture is written with array repetition instead of a Python list, so
the harness itself does not become the thing being measured.
"""

import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gguf_reader  # noqa: E402
import gguf_to_hydra  # noqa: E402

TYPES = {'F32': gguf_reader.T_F32, 'F16': gguf_reader.T_F16, 'BF16': gguf_reader.T_BF16}

#: A repeating, asymmetric pattern: both signs and exact zeros, so gamma
#: is exercised on something that is not already quantised.
PATTERN = (0.0, 0.25, -0.5, 0.75, -1.0, 0.125, 0.0, -0.25)

WIDTH = None


def write_gguf(path, rows, cols, gtype, alignment=32):
    """Write one F32/F16/BF16 tensor of rows x cols, cheaply."""
    if gtype == gguf_reader.T_BF16:
        # Not a native pack format: BF16 is the top half of an F32.
        pat = b''.join(
            struct.pack('<H', struct.unpack('<I', struct.pack('<f', v))[0] >> 16)
            for v in PATTERN)
        unit = 8
    else:
        fmt = 'f' if gtype == gguf_reader.T_F32 else 'e'
        pat = struct.pack('<8%s' % fmt, *PATTERN)
        unit = 8
    repeats = cols // unit + 1
    one_row = (pat * repeats)[:cols * gguf_reader.DENSE_FLOAT_TYPES[gtype]]

    body = bytearray()
    body += struct.pack('<I', gguf_reader.GGUF_MAGIC)
    body += struct.pack('<I', 3)
    body += struct.pack('<Q', 1)
    body += struct.pack('<Q', 0)
    name = b'token_embd.weight'
    body += struct.pack('<Q', len(name)) + name
    body += struct.pack('<I', 2)
    body += struct.pack('<Q', rows) + struct.pack('<Q', cols)
    body += struct.pack('<I', gtype) + struct.pack('<Q', 0)
    body += bytes((-len(body)) % alignment)

    count = rows * cols
    width = gguf_reader.DENSE_FLOAT_TYPES[gtype]
    with open(path, 'wb') as fh:
        fh.write(bytes(body))
        written = 0
        block_rows = max(1, (16 << 20) // (cols * width))
        while written < rows:
            n = min(block_rows, rows - written)
            fh.write(one_row * n)
            written += n
        assert count == rows * cols
    return os.path.getsize(path)


def peak_rss_kb():
    try:
        with open('/proc/self/status') as fh:
            for line in fh:
                if line.startswith('VmHWM:'):
                    return int(line.split()[1])
    except OSError:
        pass
    return -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rows', type=int, default=4096)
    ap.add_argument('--cols', type=int, default=4096)
    ap.add_argument('--type', default='F32', choices=sorted(TYPES))
    ap.add_argument('--dim', type=int, default=64)
    ap.add_argument('--max-rows', type=int, default=0,
                    help='rows actually read by the converter (default: all)')
    args = ap.parse_args()

    gtype = TYPES[args.type]
    tmp = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '.speed.gguf')
    path = os.path.abspath(tmp)
    out = path[:-5] + '.hydra'
    try:
        size = write_gguf(path, args.rows, args.cols, gtype)
        started = time.perf_counter()
        rc = gguf_to_hydra.main([path, '--out', out, '--max-rows', str(args.max_rows)])
        elapsed = time.perf_counter() - started
        if rc != 0:
            return 1

        total = args.rows * args.cols
        read = total if not args.max_rows else min(total, args.max_rows * args.cols)
        print('tensor      : %d x %d %s, %.1f MiB of source floats'
              % (args.rows, args.cols, args.type, size / 1048576.0))
        print('rows read   : %s' % ('all' if not args.max_rows else args.max_rows))
        print('values read : %d (%.1f M)' % (read, read / 1e6))
        print('wall time   : %.3f s' % elapsed)
        print('throughput  : %.2f M values/s' % (read / elapsed / 1e6))
        print('peak RSS    : %.1f MiB' % (peak_rss_kb() / 1024.0))
        print('result      : %s (%d bytes)' % (out, os.path.getsize(out)))
        return 0
    finally:
        for stale in (path, out):
            if os.path.exists(stale):
                os.remove(stale)


if __name__ == '__main__':
    sys.exit(main())
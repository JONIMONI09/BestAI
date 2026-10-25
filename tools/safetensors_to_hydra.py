#!/usr/bin/env python3
"""safetensors -> .hydra converter (Python standard library only).

    python3 tools/safetensors_to_hydra.py model.safetensors --out m.hydra
    python3 tools/safetensors_to_hydra.py model.safetensors --out m.hydra --json

WHAT THIS DOES, EXACTLY
-----------------------
A safetensors file is a container of named float tensors. The Hydra engine
is not a transformer: hydra_engine_step() computes

    acc[i] = A[i] * token + B[i] * state[i]
    token_out = ((acc[0] mod vocab) + token + 1) mod vocab

where A[i] and B[i] are the per-dimension column sums of two ternary
weight planes, folded to int32 at load time (src/hydra_engine.c,
hydra_build_aggregation). So this tool reads the real float weights out of
the safetensors file, ternarises them with the b1.58 absmean rule, sums
the ternary columns, folds them to the 64-wide state, and writes a
version-2 .hydra (4 ternary weights per byte) whose aggregation A/B
reproduces those folded column sums value for value.

The emitted model is a PROJECTION into the recurrent core, not the source
network. The report names which tensors were read, the gamma and density
of each plane, and that the result is a seed model for hydra-train.

SAFETENSORS LAYOUT
------------------
    [8 bytes: little-endian u64 N]   <- header length
    [N bytes: JSON object]           <- {name: {dtype, shape, data_offsets}}
    [data region]                    <- raw tensors

data_offsets are [start, end) RELATIVE TO THE DATA REGION, i.e. the
absolute byte range is [8 + N + start, 8 + N + end). Every tensor's range
is validated at BOTH ends (rules.md R14): start >= 0, end > start, and
8 + N + end <= file_size. A range that runs past the file end, a missing
dtype, a non-integer offset or an unparseable header is refused with a
message that names the field and the bound - never a generic "invalid".

Supported dtypes: F32 (4 B), F16 (2 B, IEEE binary16), BF16 (2 B, the top
16 bits of an F32). Anything else (Q8_0, F64, ...) is refused by name.

TERNARISATION (BitNet b1.58, absmean)
--------------------------------------
    gamma    = (1 / n) * sum(|W_i|)         over the tensor's elements
    W_q      = clip(round(W / (gamma + 1e-8)), -1, +1)

Rounding is half-away-from-zero, stated because Python's built-in round()
is half-to-even and the two disagree on every exact .5 - which is most of a
small, dense matrix. The +1e-8 keeps an all-zero tensor finite (its gamma
is 0) instead of dividing by zero.

round(W/(gamma+eps)) is a THRESHOLD, so the ternary column sums are two
counts per column (never materialising the ternary matrix):

    round(W/s) >= +0.5  <=>  W >=  s/2
    round(W/s) <= -0.5  <=>  W <= -s/2

with s = gamma + 1e-8. column_sum[j] = count(W >= s/2) - count(W <= -s/2).

PACKING (version 2, 4 ternary weights per byte)
-----------------------------------------------
Per layer the payload is two planes of dim/4 bytes each (docs/FORMAT.md,
"Version 2"):

    [ dim/4 bytes: plane w1, weight j at bits 2j..2j+1 of byte j/4 ]
    [ dim/4 bytes: plane w2, same packing ]

Code table (identical to v1): 00=0, 01=+1, 10=-1, 11=0. The engine reads
exactly this, so the packed bytes are the model - tools/gguf_test.py
re-derives A/B from the bytes with an independent reader and checks the C
engine reproduces the reference step.
"""

import argparse
import json
import math
import os
import struct
import sys

# The format constants. Mirrored from include/hydra_model.h; the
# test gate checks they agree with gguf_to_hydra and the engine.
MAGIC = 0x48594452
VERSION = 2
HEADER_SIZE = 24
MAX_DIM = 64
MAX_VOCAB = 1024
MAX_LAYERS = 4096

CODE_ZERO, CODE_POS, CODE_NEG = 0, 1, 2

#: dtype -> bytes per element. The refused list is everything not here.
DTYPE_BYTES = {'F32': 4, 'F16': 2, 'BF16': 2}

#: Header vocab when the file carries no tokenizer metadata.
DEFAULT_VOCAB = 512

#: absmean epsilon. Keeps an all-zero tensor (gamma = 0) finite.
EPS = 1e-8


class SafetensorsError(Exception):
    """A malformed or unsupported safetensors file. The message names the
    field, the value and the bound it violated (rules.md R27)."""


def _little():
    return sys.byteorder == 'little'


def round_half_away(x):
    """Round half away from zero. round() rounds half to even."""
    return math.floor(x + 0.5) if x >= 0 else math.ceil(x - 0.5)


def _prod(shape):
    n = 1
    for d in shape:
        n *= d
    return n


# ------------------------------------------------------------- container


def load_safetensors(path):
    """Parse and validate a safetensors file.

    Returns (blob, data_base, tensors) where blob is the whole file as
    bytes, data_base is 8 + header_length (the byte index of the data
    region), and tensors maps name -> {dtype, shape, start, end}. The
    data region is NOT copied: the caller slices blob at data_base +
    offset, which is O(1) and keeps memory flat for a large file.
    """
    with open(path, 'rb') as fh:
        blob = fh.read()
    size = len(blob)
    if size < 8:
        raise SafetensorsError(
            'file is %d bytes; a safetensors file needs at least 8 for the '
            'header-length prefix' % size)
    (hlen,) = struct.unpack_from('<Q', blob, 0)
    if hlen > size - 8:
        raise SafetensorsError(
            'header length %d is past the end of the file (8 + %d > %d)'
            % (hlen, hlen, size))
    header_raw = blob[8:8 + hlen]
    try:
        header = json.loads(header_raw.decode('utf-8'))
    except (UnicodeDecodeError, ValueError) as exc:
        raise SafetensorsError(
            'the %d-byte JSON header does not parse: %s' % (hlen, exc))
    if not isinstance(header, dict):
        raise SafetensorsError('the JSON header is not an object')

    data_base = 8 + hlen
    tensors = {}
    for name, meta in header.items():
        if name == '__metadata__':
            continue
        if not isinstance(meta, dict):
            raise SafetensorsError(
                'tensor %r entry is not an object' % name)
        dtype = meta.get('dtype')
        if dtype not in DTYPE_BYTES:
            raise SafetensorsError(
                'tensor %r has dtype %r; only %s are supported'
                % (name, dtype, sorted(DTYPE_BYTES)))
        shape = meta.get('shape')
        if not (isinstance(shape, list) and shape
                and all(isinstance(d, int) and d > 0 for d in shape)):
            raise SafetensorsError(
                'tensor %r has an invalid shape %r (needs positive ints)'
                % (name, shape))
        if len(shape) > 2:
            raise SafetensorsError(
                'tensor %r has %d dimensions; only 1-D and 2-D tensors can '
                'be folded into the %d-wide state' % (name, len(shape), MAX_DIM))
        offs = meta.get('data_offsets')
        if not (isinstance(offs, list) and len(offs) == 2
                and all(isinstance(o, int) for o in offs)):
            raise SafetensorsError(
                'tensor %r has invalid data_offsets %r (needs [start, end])'
                % (name, offs))
        start, end = offs
        if start < 0 or end < start:
            raise SafetensorsError(
                'tensor %r data_offsets [%d, %d] are out of order or '
                'negative' % (name, start, end))
        if data_base + end > size:
            raise SafetensorsError(
                'tensor %r runs past the end of the file (data base %d + '
                'offset end %d > file size %d)'
                % (name, data_base, end, size))
        # The declared range must also match the element count for the dtype;
        # a mismatch means the header and the bytes disagree.
        nbytes = _prod(shape) * DTYPE_BYTES[dtype]
        if end - start != nbytes:
            raise SafetensorsError(
                'tensor %r declares %d elements x %d bytes = %d bytes, but '
                'data_offsets span %d'
                % (name, _prod(shape), DTYPE_BYTES[dtype], nbytes, end - start))
        tensors[name] = {
            'dtype': dtype, 'shape': list(shape), 'start': start, 'end': end,
        }
    if not tensors:
        raise SafetensorsError('the file declares no tensors')
    return blob, data_base, tensors


# --------------------------------------------------------------- decoding


def _decode_f32(raw, n):
    if len(raw) != 4 * n:
        raise SafetensorsError('F32 payload is %d bytes, expected %d' % (len(raw), 4 * n))
    out = list(struct.unpack_from('<%df' % n, raw))
    return out


def _decode_f16(raw, n):
    if len(raw) != 2 * n:
        raise SafetensorsError('F16 payload is %d bytes, expected %d' % (len(raw), 2 * n))
    # struct '<e' is IEEE binary16, exact for subnormals, +/-inf and NaN.
    return [t[0] for t in struct.iter_unpack('<e', raw)]


def _decode_bf16(raw, n):
    if len(raw) != 2 * n:
        raise SafetensorsError('BF16 payload is %d bytes, expected %d' % (len(raw), 2 * n))
    # BF16 is the top 16 bits of an F32: widen by shifting into the high
    # half of a 32-bit word, then read it as F32.
    out = [0.0] * n
    for i, (h,) in enumerate(struct.iter_unpack('<H', raw)):
        out[i] = struct.unpack('<f', struct.pack('<I', h << 16))[0]
    return out


_DECODERS = {'F32': _decode_f32, 'F16': _decode_f16, 'BF16': _decode_bf16}


def decode_tensor(blob, data_base, meta):
    """Decode one tensor to a flat list of floats (row-major)."""
    raw = blob[data_base + meta['start']:data_base + meta['end']]
    n = _prod(meta['shape'])
    return _DECODERS[meta['dtype']](raw, n)


# ---------------------------------------------------------- quantisation


def absmean(values):
    """gamma = (1/n) sum |W_i| over the tensor's elements."""
    if not values:
        return 0.0
    return sum(abs(v) for v in values) / float(len(values))


def ternarise_column_sums(values, rows, cols, thr):
    """Per-column ternary sums, i.e. count(W >= thr) - count(W <= -thr).

    This is the column sum of W_q = clip(round(W/(gamma+eps))) without ever
    building W_q: each +1 contributes +1, each -1 contributes -1, each 0
    contributes 0, and the threshold form above is exactly where round()
    crosses +/-0.5. One pass over the elements.
    """
    pos = [0] * cols
    neg = [0] * cols
    ge = thr
    le = -thr
    for i, v in enumerate(values):
        j = i % cols
        if v >= ge:
            pos[j] += 1
        elif v <= le:
            neg[j] += 1
    return [p - m for p, m in zip(pos, neg)]


def fold_to(vec, dim):
    """Fold a vector of any length down to `dim` entries by summing
    contiguous blocks (a 4096-wide tensor becomes 64 blocks of 64). A
    shorter vector is zero-padded. Returns (folded, padded_count)."""
    n = len(vec)
    if n == dim:
        return list(vec), 0
    if n == 0:
        return [0] * dim, dim
    if n < dim:
        return list(vec) + [0] * (dim - n), dim - n
    out = [0] * dim
    for j in range(dim):
        lo = (n * j) // dim
        hi = (n * (j + 1)) // dim
        if hi <= lo:
            hi = lo + 1
        out[j] = sum(vec[lo:hi])
    return out, 0


def plane_from_tensor(blob, data_base, meta, dim, label):
    """One weight plane: decode, ternarise by absmean, sum columns, fold."""
    values = decode_tensor(blob, data_base, meta)
    rows, cols = (meta['shape'] + [1])[:2]
    gamma = absmean(values)
    if gamma == 0.0:
        # All-zero matrix: no scale to divide by, and the threshold is 0,
        # so every element ternarises to 0. Reporting zeros is the only
        # answer that is not a division by zero.
        csum = [0] * cols
        density = 0.0
    else:
        s = gamma + EPS
        csum = ternarise_column_sums(values, rows, cols, s * 0.5)
        nz = sum(1 for v in csum if v != 0)
        density = nz / float(cols) if cols else 0.0
    folded, padded = fold_to(csum, dim)
    report = {
        'plane': label,
        'dtype': meta['dtype'],
        'shape': meta['shape'],
        'gamma': round(gamma, 9),
        'density': round(density, 6),
        'zeroPaddedDims': padded,
    }
    return report, [int(v) for v in folded]


# ---------------------------------------------------------------- writing


def _code(v):
    return CODE_POS if v > 0 else (CODE_NEG if v < 0 else CODE_ZERO)


def write_hydra_v2(path, vocab, dim, layers, a_vec, b_vec):
    """Write a version-2 .hydra whose aggregation equals a_vec / b_vec
    exactly.

    A[i] is an integer in [-layers, +layers]. Column i stores +1 in the
    first min(A[i], layers) layers and -1 in the next -A[i] layers (when
    negative), zero elsewhere, so sum_l w1[l][i] == a_vec[i] by
    construction rather than by rounding. The 2-bit codes are packed four
    per byte, two planes of dim/4 bytes per layer (docs/FORMAT.md,
    "Version 2").
    """
    if dim % 4 != 0:
        raise SafetensorsError(
            'version 2 needs dim %% 4 == 0, got dim=%d' % dim)
    if layers < 1:
        raise SafetensorsError('need at least 1 layer, got %d' % layers)
    plane_bytes = dim // 4
    body = bytearray(layers * plane_bytes * 2)
    for l in range(layers):
        w1 = l * plane_bytes * 2
        w2 = w1 + plane_bytes
        for byte_idx in range(plane_bytes):
            i0 = byte_idx * 4
            body[w1 + byte_idx] = (
                _code(1 if l < a_vec[i0] else (-1 if l < -a_vec[i0] else 0))
                | _code(1 if l < a_vec[i0 + 1] else (-1 if l < -a_vec[i0 + 1] else 0)) << 2
                | _code(1 if l < a_vec[i0 + 2] else (-1 if l < -a_vec[i0 + 2] else 0)) << 4
                | _code(1 if l < a_vec[i0 + 3] else (-1 if l < -a_vec[i0 + 3] else 0)) << 6
            )
            body[w2 + byte_idx] = (
                _code(1 if l < b_vec[i0] else (-1 if l < -b_vec[i0] else 0))
                | _code(1 if l < b_vec[i0 + 1] else (-1 if l < -b_vec[i0 + 1] else 0)) << 2
                | _code(1 if l < b_vec[i0 + 2] else (-1 if l < -b_vec[i0 + 2] else 0)) << 4
                | _code(1 if l < b_vec[i0 + 3] else (-1 if l < -b_vec[i0 + 3] else 0)) << 6
            )
    header = struct.pack('<IHHIIII', MAGIC, VERSION, vocab, dim, layers,
                         HEADER_SIZE, len(body))
    tmp = path + '.tmp'
    with open(tmp, 'wb') as fh:
        fh.write(header)
        fh.write(body)
    os.replace(tmp, path)
    return HEADER_SIZE + len(body)


# ------------------------------------------------------------------ pick


def pick_tensors(tensors, a_name, b_name):
    """Choose the A and B source tensors, by name or by file order.

    A defaults to the first tensor, B to the second, so the two recurrent
    planes come from two different real matrices. With only one tensor, B
    is A's folded plane rolled by one dimension - arbitrary, deterministic,
    and named as such in the report instead of being passed off as a second
    real matrix.
    """
    names = list(tensors.keys())
    if a_name:
        if a_name not in tensors:
            raise SafetensorsError('no tensor named %r in this file' % a_name)
        a_name = a_name
    else:
        a_name = names[0]
    if b_name:
        if b_name not in tensors:
            raise SafetensorsError('no tensor named %r in this file' % b_name)
    elif len(names) >= 2:
        idx = names.index(a_name)
        for cand in names[idx + 1:] + names[:idx]:
            if cand != a_name:
                b_name = cand
                break
    else:
        b_name = None
    return a_name, b_name


def pick_vocab(header_blob, data_base, override):
    """A vocabulary the console chat can use, and a legal header value.

    Safetensors carries no tokenizer; the header field is a uint16 capped
    at MAX_VOCAB. Default DEFAULT_VOCAB, overridable with --vocab.
    """
    if override is not None:
        requested = override or 0
        kept = max(2, min(requested, MAX_VOCAB))
        return kept, max(0, requested - kept)
    return DEFAULT_VOCAB, 0


# ----------------------------------------------------------------- run


def convert(args):
    blob, data_base, tensors = load_safetensors(args.input)
    a_name, b_name = pick_tensors(tensors, args.a_tensor, args.b_tensor)
    dim = min(args.dim, MAX_DIM)

    report_a, a_raw = plane_from_tensor(blob, data_base, tensors[a_name], dim, 'A')
    if b_name is not None:
        report_b, b_raw = plane_from_tensor(blob, data_base, tensors[b_name], dim, 'B')
    else:
        # One tensor in the file: the recurrent plane is A's folded plane
        # rolled by one dimension. Arbitrary, deterministic, and named as
        # such in the report.
        b_raw = a_raw[1:] + a_raw[:1]
        report_b = dict(report_a)
        report_b['plane'] = 'B'
        report_b['dtype'] = '%s (rolled by 1 - no second tensor)' % a_name
        report_b['density'] = report_a['density']

    a_vec = [int(v) for v in a_raw]
    b_vec = [int(v) for v in b_raw]
    peak = max(max(abs(v) for v in a_vec), max(abs(v) for v in b_vec))
    layers = max(1, peak)
    if layers > MAX_LAYERS:
        raise SafetensorsError(
            'the folded column sums reach %d, but the format can hold at '
            'most %d layers per plane' % (peak, MAX_LAYERS))

    vocab, dropped = pick_vocab(blob, data_base, args.vocab)
    written = write_hydra_v2(args.out, vocab, dim, layers, a_vec, b_vec)

    report = {
        'ok': True,
        'source': os.path.basename(args.input),
        'out': args.out,
        'bytes': written,
        'header': {
            'magic': '0x%08X' % MAGIC,
            'version': VERSION,
            'vocab': vocab,
            'dim': dim,
            'layers': layers,
            'weights_offset': HEADER_SIZE,
            'weights_len': layers * dim // 2,
        },
        'planes': [report_a, report_b],
        'aggregation': {
            'A': a_vec,
            'B': b_vec,
            'peakAbs': peak,
            'exact': True,
        },
        'whatThisIs': (
            'a projection of the real float weights into the aggregated '
            'recurrent model the .hydra format can express (version 2, '
            '4 ternary weights per byte). It is not the source network: '
            'hydra_engine_step() computes A*token + B*state. Train it '
            'further with the engine.'
        ),
    }
    return report


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Convert a safetensors model into a Hydra-Stone '
                    'version-2 .hydra model (4 ternary weights per byte).')
    ap.add_argument('input', help='path to the .safetensors file')
    ap.add_argument('--out', required=True, help='path of the .hydra to write')
    ap.add_argument('--dim', type=int, default=MAX_DIM,
                    help='state width, 1..%d, %% 4 == 0 (default %d)'
                         % (MAX_DIM, MAX_DIM))
    ap.add_argument('--a-tensor', help='tensor for the A plane (default: first)')
    ap.add_argument('--b-tensor', help='tensor for the B plane (default: second)')
    ap.add_argument('--vocab', type=int, help='force the header vocab size')
    ap.add_argument('--json', action='store_true', help='print the report as JSON')
    args = ap.parse_args(argv)

    if not 1 <= args.dim <= MAX_DIM:
        ap.error('--dim must be between 1 and %d' % MAX_DIM)
    if args.dim % 4 != 0:
        ap.error('--dim must be a multiple of 4 for version 2')

    try:
        report = convert(args)
    except (SafetensorsError, OSError) as exc:
        if args.json:
            json.dump({'ok': False, 'error': str(exc)}, sys.stdout)
            sys.stdout.write('\n')
        else:
            sys.stderr.write('[safetensors_to_hydra] %s\n' % exc)
        return 1

    if args.json:
        json.dump(report, sys.stdout, ensure_ascii=False)
        sys.stdout.write('\n')
    else:
        h = report['header']
        print('[safetensors_to_hydra] %s -> %s' % (report['source'], report['out']))
        for p in report['planes']:
            print('  plane %s      : %s %s, gamma %.6g, non-zero density %.3f'
                  % (p['plane'], p['shape'], p['dtype'], p['gamma'], p['density']))
        print('  header       : vocab %d, dim %d, layers %d, %d bytes'
              % (h['vocab'], h['dim'], h['layers'], report['bytes']))
        print('  aggregation  : exact (peak |A| = %d)' % report['aggregation']['peakAbs'])
        print('  note         : %s' % report['whatThisIs'])
    return 0


if __name__ == '__main__':
    sys.exit(main())

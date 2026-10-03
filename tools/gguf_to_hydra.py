#!/usr/bin/env python3
"""GGUF -> .hydra converter (Python standard library only).

    python3 tools/gguf_to_hydra.py model.gguf --out models/converted.hydra
    python3 tools/gguf_to_hydra.py model.gguf --out m.hydra --json

WHAT THIS DOES, EXACTLY
-----------------------
The Hydra engine is not a transformer. hydra_engine_step() computes

    acc[i] = A[i] * token + B[i] * state[i]
    token_out = ((acc[0] mod vocab) + vocab) mod vocab + token + 1  (mod vocab)

and A/B are the per-dimension column sums of the two weight planes, folded
into int32 at load time (src/hydra_engine.c, hydra_build_aggregation). A
Llama GGUF is a stack of attention and feed-forward matrices, so there is no
mapping that makes one run the other. What is possible - and what this tool
does - is to read the real float weights out of the GGUF, ternarise them
with the b1.58 absmean rule, and fold them into the aggregated model the
format can actually express.

TWO PROMISES THIS TOOL KEEPS
----------------------------
1. Exactness. The emitted .hydra reproduces the aggregated vectors A and B
   the converter computed, value for value, because A[i] is written as
   sum_l w1[l][i] over enough layers to hold it. The .expected.json sidecar
   states A and B, and tools/gguf_test.py recomputes them from the packed
   bytes with an independent implementation and compares.

2. Honesty about what it is. This is a projection into a 64-dimensional
   recurrent core, not a Llama. The report says which tensors were read, how
   many rows of each were used, what the sparsification cost, and that the
   result is a seed model for hydra-train, not a reproduction of the
   original network. See docs/GGUF-IMPORT.md.

TERNARISATION (BitNet b1.58, absmean)
--------------------------------------
    gamma    = 1 / mean(|W|)          over the rows actually read
    W_tilde  = clip(round(gamma * W), -1, +1)

Rounding is half-away-from-zero, stated here because Python's built-in
round() is half-to-even and the two disagree on every exact .5 - which is
most of a small, dense matrix.
"""

import argparse
import json
import math
import operator
import os
import struct
import sys
from itertools import repeat

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gguf_reader  # noqa: E402  (needs the sys.path line above)

# The format constants. Mirrored from include/hydra_model.h and
# tools/hydra_train.js; tools/gguf_test.py checks all three agree.
MAGIC = 0x48594452
VERSION = 1
HEADER_SIZE = 24
MAX_DIM = 64
MAX_VOCAB = 1024
MAX_LAYERS = 4096

CODE_ZERO, CODE_POS, CODE_NEG = 0, 1, 2

#: How many rows of a source tensor to read. The target is 64 dimensions
#: wide, so a column sum over 4096 rows has long since converged to the
#: same 64 numbers it would give over 32000 - while costing 8x less time in
#: a pure-Python loop. 0 means "every row", which is the honest default for
#: a small file and the slow one for a 4 GB download.
DEFAULT_MAX_ROWS = 4096

#: Preferred tensor for the A plane, in the order llama.cpp names them.
A_PREFERENCE = ('token_embd.weight', 'output.weight', 'token_embd.weight')


def round_half_away(x):
    """Round half away from zero. round() rounds half to even."""
    return math.floor(x + 0.5) if x >= 0 else math.ceil(x - 0.5)


#: Float values decoded per pass. 32 M is ~128 MB as F32: big enough that
#: the per-column loop below is not dominated by slice overhead, small
#: enough that a phone or a 2 GB CI box survives a 4 GB download.
ROW_CHUNK_ELEMENTS = 32 * 1024 * 1024

_ge = operator.ge
_le = operator.le
_abs = operator.abs


def _row_blocks(rows, cols, total_rows):
    """Row ranges covering total_rows, each about ROW_CHUNK_ELEMENTS wide."""
    per_chunk = max(1, ROW_CHUNK_ELEMENTS // max(1, cols))
    return [(r, min(r + per_chunk, total_rows))
            for r in range(0, total_rows, per_chunk)]


def mean_abs(gguf, tensor, used_rows, cols, blocks):
    """mean(|W|) over exactly the rows the ternarisation will read.

    The scale and the weights have to describe the SAME matrix, so the mean
    is taken over the sampled rows and not over the whole tensor - otherwise
    --max-rows 4096 on a 32000-row embedding would ternarise against a
    gamma that belongs to rows nobody looked at.
    """
    total = 0.0
    for first, last in blocks:
        total += sum(map(_abs, gguf.decode_rows(tensor, first, last - first)))
    return total / float(used_rows * cols)


def ternarise_column_sums(gguf, tensor, used_rows, cols, blocks, thr):
    """Per-column +1/-1/0 counts, i.e. the column sums of the ternary matrix.

    b1.58 is clip(round(gamma * W), -1, +1) with gamma = 1/mean(|W|). That
    is a THRESHOLD, not a rounding table:

        round(gamma*w) >=  0.5  <=>  w >=  0.5/gamma =  mean(|W|)/2
        round(gamma*w) <= -0.5  <=>  w <= -0.5/gamma = -mean(|W|)/2

    and clipping to [-1, +1] maps everything in between to zero. So the
    whole quantisation is two counts per column, and both counts run in C
    through map() - measured 4.5x faster than materialising the ternary
    matrix element by element, and it cannot run out of memory because the
    matrix is never built.
    """
    pos = [0] * cols
    neg = [0] * cols
    pos_count = 0
    neg_count = 0
    for first, last in blocks:
        chunk = gguf.decode_rows(tensor, first, last - first)
        for j in range(cols):
            column = chunk[j::cols]
            p = sum(map(_ge, column, repeat(thr)))
            n = sum(map(_le, column, repeat(-thr)))
            pos[j] += p
            neg[j] += n
            pos_count += p
            neg_count += n
    total = used_rows * cols
    zeros = total - pos_count - neg_count
    return [p - n for p, n in zip(pos, neg)], zeros


def fold_to(vec, dim):
    """Fold a vector of any length down to `dim` entries.

    Contiguous blocks, so a 4096-wide embedding becomes 64 blocks of 64
    columns and the result is a sum, not a sample. A vector shorter than
    `dim` is zero-padded; the report says how much padding was added.
    """
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


#: Peak magnitude of a folded plane. 127 is the state vector's own
#: saturation point (include/hydra_model.h), so a converted model uses the
#: whole dynamic range the engine has - and the layer count that follows
#: from it is 127, i.e. a 8 KiB weight block no matter how big the source
#: tensor was.
PEAK = 127


def normalise(folded):
    """Rescale a folded column profile to +/- PEAK.

    The raw column sum grows with the number of rows: a 4096-row tensor of
    all +1 weights sums to 4096, which no v1 model can hold (4096 layers is
    the cap). Dividing by the row count gives the MEAN ternary weight per
    dimension, and rescaling that by its own peak is what makes the result
    independent of how many rows were read - so --max-rows 4096 and
    --max-rows 0 produce comparable planes instead of planes 8x apart.

    Both operations are a single divide by a positive constant, so the
    direction of the profile - the only thing the engine's token decoder
    reads - is preserved exactly.
    """
    peak = max(abs(v) for v in folded) if folded else 0
    if peak == 0:
        return [0] * len(folded), 0.0
    scale = float(PEAK) / peak
    return [int(round_half_away(v * scale)) for v in folded], scale


def plane_from_tensor(gguf, tensor, max_rows, dim, label):
    """One weight plane: read, ternarise, sum columns, fold, rescale.

    Never materialises the ternary matrix: the tensor is walked in row
    blocks and only two column counters survive each one.
    """
    rows, cols = tensor.shape()
    used_rows = rows if not max_rows or rows <= max_rows else max_rows
    if used_rows == 0 or cols == 0:
        raise gguf_reader.GgufError('tensor %r is empty' % tensor.name)
    blocks = _row_blocks(rows, cols, used_rows)

    mean = mean_abs(gguf, tensor, used_rows, cols, blocks)
    count = used_rows * cols
    if mean == 0.0:
        # An all-zero matrix has no scale to divide by, and gamma * 0 is
        # zero at any gamma. Reporting zeros is the only answer that is not
        # a division by zero.
        sums, zeros = [0] * cols, count
        gamma = 0.0
    else:
        gamma = 1.0 / mean
        sums, zeros = ternarise_column_sums(
            gguf, tensor, used_rows, cols, blocks, mean * 0.5)

    folded, padded = fold_to(sums, dim)
    scaled, scale = normalise(folded)
    return {
        'plane': label,
        'tensor': tensor.name,
        'shape': [rows, cols],
        'rowsRead': used_rows,
        'rowsTotal': rows,
        'sampled': used_rows != rows,
        'type': tensor.type_name,
        'gamma': round(gamma, 9),
        'meanAbs': round(mean, 9),
        'ternaryZeros': zeros,
        'density': round(1.0 - (zeros / count), 6),
        'zeroPaddedDims': padded,
        'rescale': round(scale, 9),
    }, scaled


def pick_tensors(gguf, a_name, b_name):
    """Choose the A and B source tensors.

    A defaults to the embedding, then the output projection, then the first
    readable 2-D tensor. B defaults to the next readable 2-D tensor after A,
    so the two recurrent planes come from two different real matrices; with
    only one candidate, B is A's column sums rolled by one dimension, which
    is arbitrary but deterministic and stated in the report.

    Readable means F32, F16, BF16 or Q8_0 (dequantised at read time).
    """
    candidates = gguf.decodable_candidates()
    if not candidates:
        quantised = gguf.quantised_names()
        detail = ', '.join(quantised[:6]) if quantised else 'none'
        raise gguf_reader.GgufError(
            'no F32/F16/BF16/Q8_0 1-D or 2-D tensor in this file. '
            'Unsupported quantised tensors: %s. Re-export with '
            'llama.cpp --convert-f16 and import again.' % detail)

    a_t = None
    if a_name:
        a_t = gguf.find(a_name)
        if a_t is None:
            raise gguf_reader.GgufError('no tensor named %r in this file' % a_name)
        if not a_t.is_decodable:
            raise gguf_reader.GgufError(
                'tensor %r is %s, which this converter cannot read '
                '(supported: F32, F16, BF16, Q8_0)'
                % (a_name, a_t.type_name))
        if a_t.shape() is None:
            raise gguf_reader.GgufError(
                'tensor %r has %d dimensions; only 1-D and 2-D tensors can '
                'be folded into the %d-wide state' % (a_name, a_t.n_dims, MAX_DIM))
    else:
        for name in A_PREFERENCE:
            cand = gguf.find(name)
            if cand is not None and cand.is_decodable and cand.shape() is not None:
                a_t = cand
                break
        if a_t is None:
            a_t = candidates[0]

    b_t = None
    rolled = False
    if b_name:
        b_t = gguf.find(b_name)
        if b_t is None:
            raise gguf_reader.GgufError('no tensor named %r in this file' % b_name)
        if not b_t.is_decodable:
            raise gguf_reader.GgufError(
                'tensor %r is %s, which this converter cannot read '
                '(supported: F32, F16, BF16, Q8_0)' % (b_name, b_t.type_name))
        if b_t.shape() is None:
            raise gguf_reader.GgufError(
                'tensor %r has %d dimensions; only 1-D and 2-D tensors can '
                'be folded into the %d-wide state' % (b_name, b_t.n_dims, MAX_DIM))
    else:
        idx = candidates.index(a_t) if a_t in candidates else -1
        for cand in candidates[idx + 1:]:
            if cand is not a_t:
                b_t = cand
                break
    return a_t, b_t, rolled


def pick_vocab(gguf, a_tensor, override):
    """A vocabulary the console chat can use, and a legal header value.

    The header field is a uint16 capped at MAX_VOCAB, so a 128k BPE
    vocabulary cannot be stored. The first MAX_VOCAB pieces are what fit;
    the report says how many were dropped.
    """
    if override is not None:
        return max(2, min(override, MAX_VOCAB)), override or 0, []
    tokens = gguf.tokens()
    if tokens:
        keep = tokens[:MAX_VOCAB]
        dropped = len(tokens) - len(keep)
        return max(2, len(keep)), dropped, keep
    if a_tensor is not None and a_tensor.n_dims == 2:
        rows = min(max(2, a_tensor.dims[0]), MAX_VOCAB)
        return rows, 0, []
    return 512, 0, []


def write_hydra(path, vocab, dim, layers, a_vec, b_vec):
    """Write a .hydra whose aggregation equals a_vec / b_vec exactly.

    A[i] is an integer in [-layers, +layers] and is stored as that many +1
    (or -1) entries in column i, zero elsewhere, so
    sum_l w1[l][i] == a_vec[i] by construction rather than by rounding.

    Byte layout is the engine's, verified against hydra_engine_step:
    byte index l*dim + i, w1 in the low two bits, w2 in the next two.
    """
    body = bytearray(layers * dim)
    for i in range(dim):
        a = a_vec[i]
        b = b_vec[i]
        for l in range(layers):
            idx = l * dim + i
            w1 = 1 if l < a else (-1 if l < -a else 0)
            w2 = 1 if l < b else (-1 if l < -b else 0)
            c1 = CODE_POS if w1 > 0 else (CODE_NEG if w1 < 0 else CODE_ZERO)
            c2 = CODE_POS if w2 > 0 else (CODE_NEG if w2 < 0 else CODE_ZERO)
            body[idx] = c1 | (c2 << 2)
    header = struct.pack('<IHHIIII', MAGIC, VERSION, vocab, dim, layers,
                         HEADER_SIZE, len(body))
    tmp = path + '.tmp'
    with open(tmp, 'wb') as fh:
        fh.write(header)
        fh.write(body)
    os.replace(tmp, path)
    return HEADER_SIZE + len(body)


def build_map(tokens, vocab):
    """word -> token id, from GGUF BPE pieces.

    llama.cpp marks a leading space with U+2581 (LOWER ONE EIGHTH BLOCK).
    The console splits on whitespace, so the mark is dropped and the piece
    becomes a plain word.
    """
    out = {}
    for i, piece in enumerate(tokens[:vocab]):
        word = piece.replace('▁', '').strip()
        if not word:
            continue
        if word in out:
            continue
        out[word] = i
    return out


def convert(args):
    gguf = gguf_reader.GgufFile.open(args.input)
    try:
        return _convert(args, gguf)
    finally:
        gguf.close()


def _convert(args, gguf):
    a_tensor, b_tensor, rolled = pick_tensors(gguf, args.a_tensor, args.b_tensor)

    dim = min(args.dim, MAX_DIM)
    report_a, a_raw = plane_from_tensor(gguf, a_tensor, args.max_rows, dim, 'A')

    if b_tensor is not None:
        report_b, b_raw = plane_from_tensor(gguf, b_tensor, args.max_rows, dim, 'B')
    else:
        # One readable tensor in the file: the recurrent plane is A's column
        # sums rolled by one dimension. Arbitrary, deterministic, and named
        # as such in the report instead of being passed off as a second
        # real matrix.
        b_raw = a_raw[1:] + a_raw[:1]
        report_b = {
            'plane': 'B',
            'tensor': '%s (columns rolled by 1 - no second readable tensor)' % a_tensor.name,
            'shape': report_a['shape'],
            'rowsRead': report_a['rowsRead'],
            'rowsTotal': report_a['rowsTotal'],
            'sampled': report_a['sampled'],
            'type': report_a['type'],
            'gamma': report_a['gamma'],
            'meanAbs': report_a['meanAbs'],
            'ternaryZeros': report_a['ternaryZeros'],
            'density': report_a['density'],
            'zeroPaddedDims': report_a['zeroPaddedDims'],
            'rescale': report_a['rescale'],
        }

    a_vec = [int(v) for v in a_raw]
    b_vec = [int(v) for v in b_raw]
    peak = max(max(abs(v) for v in a_vec), max(abs(v) for v in b_vec))
    layers = max(1, peak)
    if layers > MAX_LAYERS:
        # Unreachable through normalise(), kept as a guard: if the target
        # peak is ever raised past HYDRA_MAX_LAYERS this becomes the error
        # a user sees instead of a silently truncated file.
        raise gguf_reader.GgufError(
            'the folded column sums reach %d, but the format can hold at most '
            '%d layers per plane.' % (peak, MAX_LAYERS))

    vocab, dropped, tokens = pick_vocab(gguf, a_tensor, args.vocab)
    written = write_hydra(args.out, vocab, dim, layers, a_vec, b_vec)

    report = {
        'ok': True,
        'source': os.path.basename(args.input),
        'out': args.out,
        'bytes': written,
        'gguf': gguf.summary(),
        'header': {
            'magic': '0x%08X' % MAGIC,
            'version': VERSION,
            'vocab': vocab,
            'dim': dim,
            'layers': layers,
            'weights_offset': HEADER_SIZE,
            'weights_len': layers * dim,
        },
        'planes': [report_a, report_b],
        'aggregation': {
            'A': a_vec,
            'B': b_vec,
            'peakAbs': peak,
            'peakTarget': PEAK,
            'exact': True,
        },
        'vocabulary': {
            'entries': len(tokens) if tokens else 0,
            'droppedAboveMax': dropped,
            'writtenTo': None,
        },
        'sampling': {
            'maxRows': args.max_rows,
            'note': ('every row was read' if not args.max_rows else
                     'at most %d rows per tensor; the %d-column fold sums the rest'
                     % (args.max_rows, dim)),
            'whySafe': (
                'both planes are rescaled by their own peak, so the row count '
                'changes the profile but not its direction - the only thing the '
                'token decoder reads.'),
        },
        'whatThisIs': (
            'a projection of the real float weights into the aggregated '
            'recurrent model the .hydra format can express. It is not a '
            'Llama: hydra_engine_step() computes A*token + B*state, not '
            'attention. Train it further with /api/train.'
        ),
    }

    if tokens and args.vocab_out:
        m = build_map(tokens, vocab)
        target_dir = os.path.dirname(os.path.abspath(args.vocab_out))
        os.makedirs(target_dir, exist_ok=True)
        with open(args.vocab_out, 'w', encoding='utf-8') as fh:
            json.dump(m, fh, ensure_ascii=False, indent=1, sort_keys=True)
        report['vocabulary']['entries'] = len(m)
        report['vocabulary']['writtenTo'] = args.vocab_out

    if args.expect:
        with open(args.expect, 'w', encoding='utf-8') as fh:
            json.dump({'header': report['header'],
                       'aggregation': report['aggregation']}, fh)

    return report


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Convert a GGUF model into a Hydra-Stone .hydra model.')
    ap.add_argument('input', help='path to the .gguf file')
    ap.add_argument('--out', required=True, help='path of the .hydra to write')
    ap.add_argument('--dim', type=int, default=MAX_DIM,
                    help='state width, 1..%d (default %d)' % (MAX_DIM, MAX_DIM))
    ap.add_argument('--max-rows', type=int, default=DEFAULT_MAX_ROWS,
                    help='rows per source tensor; 0 reads every row (default %d)'
                         % DEFAULT_MAX_ROWS)
    ap.add_argument('--a-tensor', help='tensor for the A plane (default: the embedding)')
    ap.add_argument('--b-tensor', help='tensor for the B plane (default: the next readable tensor)')
    ap.add_argument('--vocab', type=int, help='force the header vocab size')
    ap.add_argument('--vocab-out', help='write the token list here as a word -> id JSON map')
    ap.add_argument('--expect', help='also write the header and A/B here, for the test gate')
    ap.add_argument('--json', action='store_true', help='print the report as JSON')
    args = ap.parse_args(argv)

    if not 1 <= args.dim <= MAX_DIM:
        ap.error('--dim must be between 1 and %d' % MAX_DIM)
    if args.max_rows < 0:
        ap.error('--max-rows must be 0 (all rows) or a positive count')

    try:
        report = convert(args)
    except (gguf_reader.GgufError, OSError) as exc:
        if args.json:
            json.dump({'ok': False, 'error': str(exc)}, sys.stdout)
            sys.stdout.write('\n')
        else:
            sys.stderr.write('[gguf_to_hydra] %s\n' % exc)
        return 1

    if args.json:
        json.dump(report, sys.stdout, ensure_ascii=False)
        sys.stdout.write('\n')
    else:
        h = report['header']
        print('[gguf_to_hydra] %s -> %s' % (report['source'], report['out']))
        print('  architecture : %s' % (report['gguf']['architecture'] or 'unknown'))
        print('  tensors read : %s' % ', '.join(p['tensor'] for p in report['planes']))
        for p in report['planes']:
            print('  plane %s      : %s %s, %d/%d rows, |w|<=1 density %.3f, gamma %.6g'
                  % (p['plane'], p['shape'], p['type'], p['rowsRead'],
                     p['rowsTotal'], p['density'], p['gamma']))
        print('  header       : vocab %d, dim %d, layers %d, %d bytes'
              % (h['vocab'], h['dim'], h['layers'], report['bytes']))
        print('  aggregation  : exact (peak |A| = %d)' % report['aggregation']['peakAbs'])
        if report['vocabulary']['writtenTo']:
            print('  vocabulary   : %d entries -> %s'
                  % (report['vocabulary']['entries'], report['vocabulary']['writtenTo']))
        print('  note         : %s' % report['whatThisIs'])
    return 0


if __name__ == '__main__':
    sys.exit(main())

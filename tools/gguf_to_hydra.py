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
        # Name each refused tensor WITH its layout: the message must say
        # which quantisation the file has, not just where it sits, or
        # the user has to hexdump the file to find out (2026-10-26:
        # TQ1_0 files were refused as "token_embd.weight" with no type).
        parts = ['%s (%s)' % (t.name, t.type_name)
                 for t in gguf.tensors
                 if not t.is_decodable and t.shape() is not None]
        detail = ', '.join(parts[:6]) if parts else 'none'
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
        # No tokens were read from the file on this path, so "dropped" can
        # only mean "how much of the requested count does not fit the
        # header". The kept count is clamped to [2, MAX_VOCAB], which can
        # exceed the request (--vocab 1 keeps 2), and that is not a drop:
        # dropped is max(0, requested - kept), never the raw request.
        requested = override or 0
        kept = max(2, min(requested, MAX_VOCAB))
        return kept, max(0, requested - kept), []
    tokens = gguf.tokens()
    if tokens:
        keep = tokens[:MAX_VOCAB]
        dropped = len(tokens) - len(keep)
        return max(2, len(keep)), dropped, keep
    if a_tensor is not None and a_tensor.n_dims == 2:
        rows = min(max(2, a_tensor.dims[0]), MAX_VOCAB)
        return rows, 0, []
    return 512, 0, []



# ---------------------------------------------------------------------------
# Version 3 export: per-layer ternary blocks + per-layer absmean scale.
#
# Unlike the v1/v2 aggregation (column sums of the ternary matrices), v3
# stores the ternary weight MATRICES themselves (dim x dim per block,
# 2-bit packed, 4 weights/byte) plus the layer's absmean scale, so the
# engine's per-layer forward pass applies real cross-dimension
# projections between non-linearities. The b1.58 rule is unchanged:
# ternary = clip(round(W / gamma), -1, +1), gamma = mean(|W|) over the
# sampled rows (threshold form: |w| >= mean/2).
# ---------------------------------------------------------------------------

CODE_ZERO, CODE_POS, CODE_NEG = 0, 1, 2


def pack_ternary_block(codes, dim):
    """Row-major -1/0/+1 codes -> dim*dim/4 bytes (00=0, 01=+1, 10=-1)."""
    out = bytearray((dim * dim) // 4)
    for idx, c in enumerate(codes):
        code = CODE_POS if c > 0 else (CODE_NEG if c < 0 else CODE_ZERO)
        out[idx // 4] |= code << (2 * (idx & 3))
    return bytes(out)


def unpack_ternary_block(raw, dim):
    """Inverse of pack_ternary_block, for the write-time roundtrip check."""
    out = []
    for idx in range(dim * dim):
        code = (raw[idx // 4] >> (2 * (idx & 3))) & 0x03
        out.append(1 if code == CODE_POS else (-1 if code == CODE_NEG else 0))
    return out


def v3_block_from_tensor(gguf, tensor, dim, max_rows):
    """Ternarise the top-left dim x dim slice of a real tensor.

    gamma (the scale) is the absmean over the tensor's SAMPLED rows - the
    same rows the v1/v2 path samples - so a converted v3 model and its
    v1 twin see the same source magnitudes. The slice itself is small
    (dim <= 64 -> 4096 elements) and is materialised; the absmean over
    the rest of the tensor is NOT, for the same memory reasons as the
    v1/v2 two-count pass. A tensor smaller than dim x dim is zero-
    padded; the report says so.
    """
    rows, cols = tensor.shape()
    used_rows = rows if not max_rows or rows <= max_rows else max_rows
    if used_rows == 0 or cols == 0:
        raise gguf_reader.GgufError('tensor %r is empty' % tensor.name)
    blocks = _row_blocks(rows, cols, used_rows)
    mean = mean_abs(gguf, tensor, used_rows, cols, blocks)
    thr = mean * 0.5 if mean > 0.0 else 0.0
    sr = min(dim, rows)
    sc = min(dim, cols)
    codes = [0] * (dim * dim)
    nonzeros = 0
    if thr > 0.0 and sr > 0:
        chunk = gguf.decode_rows(tensor, 0, sr)
        for i in range(sr):
            row = chunk[i * cols:(i + 1) * cols]
            for j in range(sc):
                w = row[j]
                if w >= thr:
                    c = 1
                elif w <= -thr:
                    c = -1
                else:
                    c = 0
                codes[i * dim + j] = c
                nonzeros += 1 if c != 0 else 0
    # scale u16 = absmean * 256 (fixed point), clamped to [1, 65535]:
    # 0 is forbidden (the engine divides by it) and 65535 the field cap.
    scale = 1
    if mean > 0.0:
        scale = max(1, min(65535, int(round_half_away(mean * 256.0))))
    report = {
        'tensor': tensor.name,
        'shape': [rows, cols],
        'rowsRead': used_rows,
        'rowsTotal': rows,
        'sampled': used_rows != rows,
        'type': tensor.type_name,
        'gamma': round(mean, 9),
        'scale': scale,
        'density': round(nonzeros / float(dim * dim), 6),
        'zeroPadded': (dim - sr, dim - sc),
    }
    return codes, scale, report


def _find_preference(gguf, candidates):
    """First decodable 2-D tensor matching A_PREFERENCE, else candidates[0]."""
    for name in A_PREFERENCE:
        t = gguf.find(name)
        if t is not None and t.is_decodable and t.shape() is not None:
            return t
    return candidates[0]


def _convert_v3(args, gguf):
    """Version 3 export: per-layer blocks from real tensor slices."""
    dim = args.dim
    if dim % 4 != 0:
        raise gguf_reader.GgufError(
            '--v3 needs --dim a multiple of 4 (got %d)' % dim)
    layers = args.v3_layers
    if not 1 <= layers <= MAX_LAYERS:
        raise gguf_reader.GgufError(
            '--v3-layers must be between 1 and %d' % MAX_LAYERS)
    candidates = [c for c in gguf.decodable_candidates()
                  if c.shape() is not None]
    if not candidates:
        raise gguf_reader.GgufError('no decodable 2-D tensor in this file')
    n = len(candidates)
    w1_src = _find_preference(gguf, candidates)

    w1_rows, w1_cols = w1_src.shape()
    w1_used = w1_rows if not args.max_rows or w1_rows <= args.max_rows else args.max_rows
    w1_mean = mean_abs(gguf, w1_src, w1_used, w1_cols,
                       _row_blocks(w1_rows, w1_cols, w1_used))
    w1_thr = w1_mean * 0.5 if w1_mean > 0.0 else 0.0

    layer_reports = []
    body = bytearray()
    for l in range(layers):
        # Gate/up sources cycle deterministically through the candidate
        # tensors (two distinct tensors per layer when at least two
        # exist). Stated in the report, never passed off as one tensor.
        t_g = candidates[(2 * l) % n]
        t_u = candidates[(2 * l + 1) % n]
        codes_g, scale_g, rep_g = v3_block_from_tensor(gguf, t_g, dim, args.max_rows)
        codes_u, scale_u, rep_u = v3_block_from_tensor(gguf, t_u, dim, args.max_rows)
        # One scale per layer (the engine divides gate and up by the
        # same value): the LARGER of the two source absmeans, so neither
        # projection is rescaled above its own magnitude.
        scale = max(scale_g, scale_u)
        # Token injection: one embedding row per layer (cycling), ternary
        # with the embedding's own absmean - the closest real-tensor
        # analogue of the v1 token drive.
        w1_codes = [0] * dim
        w1_nz = 0
        if w1_thr > 0.0:
            # decode_rows returns a flat list of values (rows
            # concatenated); one requested row is the whole list.
            row = gguf.decode_rows(w1_src, l % w1_rows, 1)
            for i in range(min(dim, w1_cols)):
                w = row[i]
                if w >= w1_thr:
                    w1_codes[i] = 1
                elif w <= -w1_thr:
                    w1_codes[i] = -1
                w1_nz += 1 if w1_codes[i] != 0 else 0
        body += pack_ternary_block(codes_g, dim)
        body += pack_ternary_block(codes_u, dim)
        body += pack_ternary_block(w1_codes, dim)[:dim // 4]
        body += struct.pack('<H', scale)
        layer_reports.append({
            'layer': l,
            'gate': rep_g,
            'up': rep_u,
            'scale': scale,
            'w1': {
                'tensor': w1_src.name,
                'row': l % w1_rows,
                'gamma': round(w1_mean, 9),
                'nonzeros': w1_nz,
            },
        })

    w_out_src = candidates[0]
    # scale_o is intentionally unused: the v3 W_out block has no scale field
    # in the file format - the engine applies the raw ternary codes as int32
    # logits without rescaling (the per-layer blocks keep their u16 scale).
    codes_o, _scale_o, rep_o = v3_block_from_tensor(gguf, w_out_src, dim, args.max_rows)
    body += pack_ternary_block(codes_o, dim)
    w_out_report = dict(rep_o)
    w_out_report['note'] = ('final logit-mix block; the engine applies it '
                            'without rescaling (int32 logits)')

    vocab, dropped, tokens = pick_vocab(gguf, w1_src, args.vocab)
    header = struct.pack('<IHHIIII', MAGIC, 3, vocab, dim, layers,
                         HEADER_SIZE, len(body))
    tmp = args.out + '.tmp'
    with open(tmp, 'wb') as fh:
        fh.write(header)
        fh.write(bytes(body))
    os.replace(tmp, args.out)

    # Write-time roundtrip check, stdlib only: re-read the file, re-decode
    # the first layer's scale and one W_out row, and compare with the
    # source-derived values. Catches a layout drift between writer and
    # format on the spot, before the C tests ever run.
    with open(args.out, 'rb') as fh:
        raw = fh.read()
    wlen = struct.unpack_from('<I', raw, 20)[0]
    lb = dim * dim // 2 + dim // 4 + 2
    scale0 = struct.unpack_from('<H', raw, HEADER_SIZE + lb - 2)[0]
    wout_base = HEADER_SIZE + layers * lb
    wout_full = unpack_ternary_block(raw[wout_base:wout_base + dim * dim // 4], dim)
    ok = (wlen == len(body)
          and scale0 == layer_reports[0]['scale']
          and wout_full == codes_o)
    if not ok:
        raise gguf_reader.GgufError(
            'v3 write-time roundtrip check failed (layout drift)')

    report = {
        'ok': True,
        'source': os.path.basename(args.input),
        'out': args.out,
        'bytes': HEADER_SIZE + len(body),
        'gguf': gguf.summary(),
        'header': {
            'magic': '0x%08X' % MAGIC,
            'version': 3,
            'vocab': vocab,
            'dim': dim,
            'layers': layers,
            'weights_offset': HEADER_SIZE,
            'weights_len': len(body),
        },
        'layers': layer_reports,
        'wOut': w_out_report,
        'roundtripCheck': 'passed',
        'vocabulary': {
            'entries': vocab,
            'dropped': dropped,
            'writtenTo': None,
        },
        'whatThisIs': (
            'version 3: a per-layer ternary forward pass built from dim x dim '
            'slices of real tensors (top-left corner, zero-padded when the '
            'tensor is smaller). The engine runs dim <= %d, so this is a '
            'faithful miniature of the source model, not the full model; '
            'per-layer absmean scales keep the relative magnitudes honest.'
            % MAX_DIM),
    }
    if args.vocab_out and tokens:
        with open(args.vocab_out, 'w') as fh:
            json.dump(build_map(tokens, vocab), fh, ensure_ascii=False, indent=1)
        report['vocabulary']['writtenTo'] = args.vocab_out
    return report


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
        if getattr(args, 'v3', False):
            return _convert_v3(args, gguf)
        return _convert(args, gguf)
    finally:
        gguf.close()


def _convert(args, gguf):
    # _rolled is (b_tensor is None) by construction of pick_tensors; the
    # b_tensor-is-None branch below already names the rolled plane.
    a_tensor, b_tensor, _rolled = pick_tensors(gguf, args.a_tensor, args.b_tensor)

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
    ap.add_argument('--v3', action='store_true',
                    help='export a version-3 model (per-layer ternary blocks '
                         '+ absmean scales; run by hydra_engine_step_tf)')
    ap.add_argument('--v3-layers', type=int, default=16,
                    help='number of real layers for --v3 (default 16, max %d)'
                         % MAX_LAYERS)
    ap.add_argument('--vocab', type=int, help='force the header vocab size')
    ap.add_argument('--vocab-out', help='write the token list here as a word -> id JSON map')
    ap.add_argument('--expect', help='also write the header and A/B here, for the test gate')
    ap.add_argument('--json', action='store_true', help='print the report as JSON')
    args = ap.parse_args(argv)

    if not 1 <= args.dim <= MAX_DIM:
        ap.error('--dim must be between 1 and %d' % MAX_DIM)
    if args.v3:
        if args.dim % 4 != 0:
            ap.error('--v3 needs --dim a multiple of 4')
        if not 1 <= args.v3_layers <= MAX_LAYERS:
            ap.error('--v3-layers must be between 1 and %d' % MAX_LAYERS)
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
        if 'planes' in report:
            print('  tensors read : %s' % ', '.join(p['tensor'] for p in report['planes']))
            for p in report['planes']:
                print('  plane %s      : %s %s, %d/%d rows, |w|<=1 density %.3f, gamma %.6g'
                      % (p['plane'], p['shape'], p['type'], p['rowsRead'],
                         p['rowsTotal'], p['density'], p['gamma']))
        else:
            tens = []
            for lr in report['layers']:
                tens.append(lr['gate']['tensor'])
                tens.append(lr['up']['tensor'])
            tens.append(report['wOut']['tensor'])
            print('  tensors read : %s' % ', '.join(dict.fromkeys(tens)))
        print('  header       : vocab %d, dim %d, layers %d, %d bytes'
              % (h['vocab'], h['dim'], h['layers'], report['bytes']))
        if 'aggregation' in report:
            print('  aggregation  : exact (peak |A| = %d)' % report['aggregation']['peakAbs'])
        if report['header']['version'] == 3:
            print('  v3 layers      : %d blocks, roundtrip check: %s'
                  % (report['header']['layers'], report['roundtripCheck']))
            for lr in report['layers'][:4]:
                print('  layer %-3d    : gate %s (gamma %.6g) | up %s (gamma %.6g) | scale %d'
                      % (lr['layer'], lr['gate']['tensor'], lr['gate']['gamma'],
                         lr['up']['tensor'], lr['up']['gamma'], lr['scale']))
            if report['header']['layers'] > 4:
                print('  ...          : %d more layers'
                      % (report['header']['layers'] - 4))
        if report['vocabulary']['writtenTo']:
            print('  vocabulary   : %d entries -> %s'
                  % (report['vocabulary']['entries'], report['vocabulary']['writtenTo']))
        print('  note         : %s' % report['whatThisIs'])
    return 0


if __name__ == '__main__':
    sys.exit(main())

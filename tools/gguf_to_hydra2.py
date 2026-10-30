#!/usr/bin/env python3
"""GGUF -> .hydra2 converter (round 12, M2.2 / M3.1 / M4).

    python3 tools/gguf_to_hydra2.py in.gguf out.hydra2 [--max-experts 16]

Reads a decoder-only **llama** GGUF (F32/F16/BF16/Q8_0 weights) and writes
a complete-contract `.hydra2` file (FORMAT-V2.md, architecture
`moe-transformer-v1`) that the MoE engine (`src/hydra_moe.c`) runs
directly: per-layer GQA attention, expert FFN, RMSNorm, real logits,
plus the embedded vocabulary and BPE merges (role 8, section 3.2.3).

MAPPING (llama -> moe-transformer-v1)
-------------------------------------
  dim / n_heads / n_kv_heads / head_dim / inter / layers
      from llama.embedding_length / attention.head_count /
      attention.head_count_kv / feed_forward_length / block_count
  tok_emb [dim, vocab]            -> embed [vocab, dim]   (transposed)
  blk.L.attn_q/k/v/o [in, out]    -> attn_q/k/v/o.L [out, in] (transposed)
  blk.L.attn_norm / ffn_norm      -> attn_norm.L / ffn_norm.L
  blk.L.ffn_gate / ffn_up         -> ffn_gate/up.L [inter, dim] (transposed)
  blk.L.ffn_down [inter, dim]     -> ffn_down.L [dim, inter] (transposed)
  output norm                     -> final_norm
  output [dim, vocab] (optional)  -> lm_head [vocab, dim] (transposed);
                                     absent => tied embedding
  tokenizer.ggml.tokens           -> 'vocab' tensor (VOCAB_UTF8, role 8)
  tokenizer.ggml.merges           -> 'merges' tensor (U16_PAIRS, role 8)
  llama.rope.freq_base            -> v2.rope_theta (default 10000.0)

DENSE FFN -> EXPERTS (the masked decomposition, EXACT by construction)
-----------------------------------------------------------------------
The engine's expert geometry is FIXED to the model's `inter` (every
expert is a full-width [inter x dim] gate/up and [dim x inter] down),
so a dense SwiGLU layer `down @ (up@x * sigmoid(gate@x))` is converted
to E full-width experts that PARTITION the intermediate rows: expert e
keeps only the rows [e*c .. (e+1)*c) of gate/up (c = inter/E), scaled
by E, and zeroes the rest; its down matrix is the full dense down
unmodified. Because every intermediate row r contributes only through
its own gate/up rows, the partition is exact: with a UNIFORM router
(all-zero router rows -> every logit 0 -> softmax weight 1/E) and
top_k = E the engine computes
  sum_e (1/E) * E * (rows G_e of the dense intermediate) = the dense
  result,
up to F32 rounding of the per-expert summation order (a few ulp) - the
only approximation in the weight path. E = 1 (the default: --max-
experts 1) is a verbatim copy of the dense FFN (one expert, no
masking). Note the storage cost: E experts of FULL width, i.e. E times
the dense FFN bytes per layer; --max-experts N keeps E = the largest
divisor of `inter` in [1, N]. The dense matrices are additionally kept
verbatim in the trunk tensors `ffn_gate/up/down.L` (source of record).

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
  - a tensor stored in a layout this reader cannot decode (Q4_0, ...)
  - a required llama tensor is missing
  - head_count does not divide embedding_length
"""

import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gguf_reader  # noqa: E402
import make_model   # noqa: E402  (the shared _assemble_v2 byte writer)


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


class Trunk:
    """16-byte-aligned trunk writer with the tensor-directory record
    format _assemble_v2 expects: (name, role, tid, ndims, elems, off,
    plen)."""

    def __init__(self):
        self.data = bytearray()
        self.tensors = []

    def add(self, name, role, payload, tid=0x12, ndims=2, elems=None):
        off = len(self.data)
        if off % 16:
            pad = 16 - off % 16
            self.data.extend(b'\x00' * pad)
            off += pad
        if elems is None:
            elems = len(payload) // 4 if tid == 0x12 else len(payload)
        self.data.extend(payload)
        self.tensors.append((name, role, tid, ndims, elems, off,
                             len(payload)))
        return off


def pick_experts(inter, max_experts):
    """The largest divisor of `inter` in [1, max_experts] (1 when none)."""
    for e in range(min(max_experts, inter), 0, -1):
        if inter % e == 0:
            return e
    return 1


def rows_to_f32(gguf, tensor, rows, cols):
    """Decode a tensor (decode_rows returns a FLAT list, row-major) and
    return it as one flat byte string, [rows][cols] F32."""
    out = bytearray(rows * cols * 4)
    for r0 in range(0, rows, 256):
        nr = min(256, rows - r0)
        block = gguf.decode_rows(tensor, r0, nr)
        struct.pack_into('<%df' % len(block), out, r0 * cols * 4, *block)
    return bytes(out)


def transpose_f32(gguf, tensor, rows, cols):
    """GGUF [rows][cols] -> flat F32 [cols][rows] (the tok_emb/output
    transpose: [dim][vocab] -> [vocab][dim])."""
    out = bytearray(rows * cols * 4)
    for r0 in range(0, rows, 256):
        nr = min(256, rows - r0)
        block = gguf.decode_rows(tensor, r0, nr)
        for i in range(nr):
            d = r0 + i  # source row index == destination column
            base = i * cols
            for c in range(cols):
                struct.pack_into('<f', out, c * rows * 4 + d * 4,
                                 block[base + c])
    return bytes(out)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Convert a llama GGUF to a .hydra2 MoE model')
    ap.add_argument('in_gguf')
    ap.add_argument('out_hydra2')
    ap.add_argument('--max-experts', type=int, default=1,
                    help='expert count bound for the dense->expert '
                         'decomposition (default 1 = verbatim dense FFN '
                         'as a single expert; E is the largest divisor '
                         'of the FFN width within the bound; costs E '
                         'times the FFN storage per layer)')
    args = ap.parse_args(argv)

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

        trunk = Trunk()
        fe = 0
        expert_dir = []
        expert_payloads = []

        # ---- embed (transposed) ---------------------------------------
        trunk.add('embed', 2, transpose_f32(gguf, tok_emb, dim, vocab),
                  elems=vocab * dim)

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
            # GGUF llama stores every linear as [in, out]; the engine
            # wants [out, in] - all of them are transposed.
            for t, exp in ((q, [dim, dim]), (k, [dim, kvdim]),
                           (v, [dim, kvdim]), (o, [dim, dim]),
                           (an, [dim]), (fn, [dim]),
                           (gate, [dim, inter]), (up, [dim, inter]),
                           (down, [inter, dim])):
                if t.n_dims != len(exp) or list(t.dims) != exp:
                    die('tensor %s is %s, expected %s'
                        % (t.name, t.dims, exp))
            trunk.add('attn_q.%d' % l, 7,
                      transpose_f32(gguf, q, dim, dim), elems=dim * dim)
            trunk.add('attn_k.%d' % l, 7,
                      transpose_f32(gguf, k, dim, kvdim), elems=kvdim * dim)
            trunk.add('attn_v.%d' % l, 7,
                      transpose_f32(gguf, v, dim, kvdim), elems=kvdim * dim)
            trunk.add('attn_o.%d' % l, 7,
                      transpose_f32(gguf, o, dim, dim), elems=dim * dim)
            trunk.add('attn_norm.%d' % l, 3,
                      rows_to_f32(gguf, an, 1, dim), elems=dim, ndims=1)
            trunk.add('ffn_norm.%d' % l, 3,
                      rows_to_f32(gguf, fn, 1, dim), elems=dim, ndims=1)

            # uniform router: every logit is 0 -> softmax 1/E (exact)
            trunk.add('router.%d' % l, 1,
                      b'\x00' * (E * dim * 4), elems=E * dim)

            # dense FFN kept verbatim in the trunk (source of record;
            # the engine routes through the experts)
            gate_f = transpose_f32(gguf, gate, dim, inter)
            up_f = transpose_f32(gguf, up, dim, inter)
            down_f = transpose_f32(gguf, down, inter, dim)
            trunk.add('ffn_gate.%d' % l, 0, gate_f, elems=inter * dim)
            trunk.add('ffn_up.%d' % l, 0, up_f, elems=inter * dim)
            trunk.add('ffn_down.%d' % l, 0, down_f, elems=dim * inter)

            # the masked decomposition: E full-width experts that
            # partition the intermediate rows (chunk = inter/E rows
            # each; E == 1 -> the dense FFN verbatim)
            def masked_scaled(buf, i0):
                """Full [inter][dim] matrix: rows i0..i0+chunk kept and
                multiplied by E, every other row zeroed."""
                out = bytearray(inter * dim * 4)
                for i in range(chunk * dim):
                    x = struct.unpack_from('<f', buf,
                                           (i0 + i) * 4)[0]
                    struct.pack_into('<f', out, (i0 + i) * 4, x * E)
                return bytes(out)

            for e in range(E):
                i0 = e * chunk * dim
                payload = masked_scaled(gate_f, i0) + masked_scaled(
                    up_f, i0) + down_f
                off = sum(len(p) for p in expert_payloads)
                expert_payloads.append(payload)
                expert_dir.append((l, e, off, len(payload), fe,
                                   0x12, crc32(payload)))
                fe += 1

        # ---- final norm + lm_head -------------------------------------
        trunk.add('final_norm', 3,
                  rows_to_f32(gguf, norm_t, 1, dim),
                  elems=dim, ndims=1)
        if output is not None:
            trunk.add('lm_head', 9,
                      transpose_f32(gguf, output, dim, vocab),
                      elems=vocab * dim)

        # ---- vocabulary + merges (role 8, FORMAT-V2.md 3.2.3) ----------
        vocab_blob = b''.join(t.encode('utf-8') + b'\x00' for t in tokens)
        trunk.add('vocab', 8, vocab_blob, tid=0x0050, ndims=1,
                  elems=vocab)
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
                li = id_of.get(left)
                ri = id_of.get(right)
                ti = id_of.get(left + right)
                if li is None or ri is None or ti is None:
                    skipped += 1  # not expressible as a token triple
                    continue
                rules.append((li, ri))
        if rules:
            trunk.add('merges', 8,
                      b''.join(struct.pack('<HH', a, b)
                               for a, b in rules),
                      tid=0x0051, ndims=1, elems=len(rules))

        # ---- metadata --------------------------------------------------
        meta_pairs = [
            ('v2.architecture', b'moe-transformer-v1'),
            ('v2.layers', u32(layers)),
            ('v2.moe_layers', u32(layers)),
            ('v2.top_k', u32(E)),
            ('v2.experts_per_layer', u32(E)),
            ('v2.router_dtype', u32(0x12)),
            ('v2.expert_bytes_typ',
             struct.pack('<Q', len(expert_payloads[0]))),
            ('v2.router_runs_ahead', u32(1)),
            ('v2.dim', u32(dim)),
            ('v2.vocab_size', u32(vocab)),
            ('v2.n_heads', u32(n_heads)),
            ('v2.n_kv_heads', u32(n_kv)),
            ('v2.head_dim', u32(head_dim)),
            ('v2.inter', u32(inter)),
            ('v2.expert_layout', u32(0)),
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

        data = make_model._assemble_v2(
            bytes(meta_b), trunk.tensors, expert_dir, bytes(trunk.data),
            b''.join(expert_payloads))
        parent = os.path.dirname(os.path.abspath(args.out_hydra2))
        os.makedirs(parent, exist_ok=True)
        tmp = args.out_hydra2 + '.tmp'
        with open(tmp, 'wb') as fh:
            fh.write(data)
        os.replace(tmp, args.out_hydra2)
        print('[gguf_to_hydra2] %s -> %s: %d bytes (llama %d layers, '
              'dim %d, heads %d/%d, inter %d, vocab %d, %d experts/layer '
              'x top_k %d, %d merge rules written, %d skipped)'
              % (args.in_gguf, args.out_hydra2, len(data), layers, dim,
                 n_heads, n_kv, inter, vocab, E, E, len(rules), skipped))


if __name__ == '__main__':
    main()

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

        # ---- embed (file order: already [vocab][dim]) -----------------
        trunk.add('embed', 2, flat_f32(gguf, tok_emb), elems=vocab * dim)

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
            for t, exp in ((q, [dim, dim]), (k, [dim, kvdim]),
                           (v, [dim, kvdim]), (o, [dim, dim]),
                           (an, [dim]), (fn, [dim]),
                           (gate, [dim, inter]), (up, [dim, inter]),
                           (down, [inter, dim])):
                if t.n_dims != len(exp) or list(t.dims) != exp:
                    die('tensor %s is %s, expected %s'
                        % (t.name, t.dims, exp))
            trunk.add('attn_q.%d' % l, 7,
                      flat_f32(gguf, q), elems=dim * dim)
            trunk.add('attn_k.%d' % l, 7,
                      flat_f32(gguf, k), elems=kvdim * dim)
            trunk.add('attn_v.%d' % l, 7,
                      flat_f32(gguf, v), elems=kvdim * dim)
            trunk.add('attn_o.%d' % l, 7,
                      flat_f32(gguf, o), elems=dim * dim)
            trunk.add('attn_norm.%d' % l, 3,
                      flat_f32(gguf, an), elems=dim, ndims=1)
            trunk.add('ffn_norm.%d' % l, 3,
                      flat_f32(gguf, fn), elems=dim, ndims=1)

            # uniform router: every logit is 0 -> softmax 1/E (exact)
            trunk.add('router.%d' % l, 1,
                      b'\x00' * (E * dim * 4), elems=E * dim)

            # dense FFN kept verbatim in the trunk (source of record;
            # the engine routes through the experts)
            gate_f = flat_f32(gguf, gate)
            up_f = flat_f32(gguf, up)
            down_f = flat_f32(gguf, down)
            trunk.add('ffn_gate.%d' % l, 0, gate_f, elems=inter * dim)
            trunk.add('ffn_up.%d' % l, 0, up_f, elems=inter * dim)
            trunk.add('ffn_down.%d' % l, 0, down_f, elems=dim * inter)

            # the masked decomposition: E full-width experts that
            # partition the intermediate rows (chunk = inter/E rows
            # each; E == 1 -> the dense FFN verbatim). SiLU is nonlinear,
            # so only the UP rows carry the E factor (see the module
            # docstring): G_e = block, U_e = E * block.
            def masked_block(buf, i0, scale):
                """Full [inter][dim] matrix: rows i0..i0+chunk kept and
                multiplied by `scale`, every other row zeroed."""
                out = bytearray(inter * dim * 4)
                for i in range(chunk * dim):
                    x = struct.unpack_from('<f', buf,
                                           (i0 + i) * 4)[0]
                    struct.pack_into('<f', out, (i0 + i) * 4, x * scale)
                return bytes(out)

            for e in range(E):
                i0 = e * chunk * dim
                payload = masked_block(gate_f, i0, 1.0) + masked_block(
                    up_f, i0, float(E)) + down_f
                off = sum(len(p) for p in expert_payloads)
                expert_payloads.append(payload)
                expert_dir.append((l, e, off, len(payload), fe,
                                   0x12, crc32(payload)))
                fe += 1

        # ---- final norm + lm_head -------------------------------------
        # the engine reads final_norm as a 1-D vector of length dim - a
        # tensor that merely EXISTS with other shape would open and
        # compute garbage, so validate it (round-13, Qodo #24).
        if (norm_t.n_dims != 1 or list(norm_t.dims) != [dim]):
            die('output norm %s is %s, expected 1-D [dim=%d]'
                % (norm_t.name, list(norm_t.dims), dim))
        trunk.add('final_norm', 3,
                  flat_f32(gguf, norm_t),
                  elems=dim, ndims=1)
        if output is not None:
            trunk.add('lm_head', 9,
                      flat_f32(gguf, output),
                      elems=vocab * dim)

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

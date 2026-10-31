#!/usr/bin/env python3
"""End-to-end tests for tools/gguf_to_hydra2.py (round 12, M2.2/M3.1/M4;
round 13, Qodo #1/#2/#4/#9/#15/#24).

    python3 tools/gguf_to_hydra2_test.py [path/to/hydra-run]

Builds llama GGUF fixtures (via tools/gguf_minimal_llama.py, the same
generator the v1 gate pins), converts them with the new converter, and
verifies:

  1. the converted file opens in the real engine and two runs produce
     the identical token stream (determinism);
  2. a single flipped byte in the expert section changes the outcome
     (refusal or different stream) - the weights actually flow;
  3. embed is exactly the tok_emb bytes in GGUF FILE order (independent
     re-read of both files, F32-exact) - the round-13 layout fix: the
     engine layout IS the file order, so any rearrangement (the old
     transpose) scrambles every non-square tensor;
  4. every expert payload is exactly the masked dense FFN with the
     correct scaling - gate x1, up xE (independent re-computation from
     the dense trunk matrices; the round-13 fix: SiLU is nonlinear, so
     the gate rows must stay unscaled);
  5. the embedded vocabulary is byte-exact (NUL-terminated tokens);
  6. the v2 metadata agrees with the GGUF architecture;
  7. the router rows are all-zero (the documented uniform router);
  8. merges: a GGUF WITH merge rules produces exactly the expected
     u16 pair rules; unresolvable rules are skipped;
  8b. E=1 vs E=2 equivalence: the same source GGUF converted with
      --max-experts 1 and 2 yields IDENTICAL greedy streams (the masked
      decomposition is behaviourally the dense FFN);
  8c. token normalisation: SPM U+2581-glyph tokens are stored
      byte-equivalent (' b' not '\u2581b') and merges resolve through
      the normalised ids (Qodo #15 - real llama vocabs have no
      single-byte space token);
  9. loud refusals: non-GGUF input, Q4_0 tensors, non-llama
     architecture, missing tokens, mismatched dimensions, empty/NUL/
     duplicate tokens, a wrongly shaped output norm, and a merge
     operand above the 16-bit pair format.

Every check exits non-zero on failure (the CI gate contract).
"""

import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import gguf_minimal_llama  # noqa: E402
import gguf_reader         # noqa: E402
import gguf_to_hydra2      # noqa: E402

MAGIC = b'HYDRA2\x00\x00'


def check(name, cond, detail=''):
    if not cond:
        print('FAIL: %s %s' % (name, detail))
        sys.exit(1)
    print('ok: %s' % name)


def parse_v2(path):
    """Minimal .hydra2 reader for the test: returns (meta, tensors,
    expert_dir, blobs) where tensors is a dict name -> (role, tid,
    ndims, elems, off, plen) and blobs are the raw trunk/expert bytes."""
    with open(path, 'rb') as fh:
        raw = fh.read()
    assert raw[:8] == MAGIC, 'magic'
    u64 = lambda i: struct.unpack_from('<Q', raw, i)[0]
    meta_off = u64(24)
    td_off, n_tensors = u64(40), u64(48)
    ed_off, n_experts = u64(64), u64(72)
    trunk_off, trunk_len = u64(88), u64(96)
    expdata_off, experts_len = u64(104), u64(112)
    meta = {}
    p = meta_off
    n_meta = struct.unpack_from('<I', raw, p)[0]
    p += 4
    for _ in range(n_meta):
        klen, vlen = struct.unpack_from('<II', raw, p)
        p += 8
        key = raw[p:p + klen].decode('ascii')
        p += klen
        meta[key] = raw[p:p + vlen]
        p += vlen
    tensors = {}
    base = td_off
    for _ in range(n_tensors):
        name = raw[base:base + 32].split(b'\x00', 1)[0].decode('ascii')
        role, tid, ndims = struct.unpack_from('<HHH', raw, base + 32)
        off, elems, plen = u64(base + 40), u64(base + 48), u64(base + 56)
        tensors[name] = (role, tid, ndims, elems, off, plen)
        base += 64
    edir = []
    base = ed_off
    for _ in range(n_experts):
        l, e = struct.unpack_from('<II', raw, base)
        off, length, fe = u64(base + 8), u64(base + 16), u64(base + 24)
        tid, crc = struct.unpack_from('<II', raw, base + 32)
        edir.append((l, e, off, length, fe, tid, crc))
        base += 48
    trunk_blob = raw[trunk_off:trunk_off + trunk_len]
    expert_blob = raw[expdata_off:expdata_off + experts_len]
    return meta, tensors, edir, trunk_blob, expert_blob, raw


def f32_at(blob, i):
    """F32 element i of a fresh byte buffer (element index, not offset)."""
    return struct.unpack_from('<f', blob, i * 4)[0]


def f32_at_off(blob, byte_off):
    return struct.unpack_from('<f', blob, byte_off)[0]


def run_engine(binary, model, seed, steps, timeout=120):
    p = subprocess.run([binary, model, str(seed), str(steps)],
                       capture_output=True, text=True, timeout=timeout)
    toks = None
    for line in p.stdout.splitlines():
        if line.startswith('[Tokens]'):
            toks = line.split(':', 1)[1].split()
    return p.returncode, toks


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        REPO, 'hydra-run')
    if not os.path.exists(binary):
        print('FAIL: engine binary %s not found (run make first)' % binary)
        sys.exit(1)

    tmp = tempfile.mkdtemp(prefix='hydra2-gguf-conv-')
    try:
        gguf = os.path.join(tmp, 'minilama.gguf')
        with open(gguf, 'wb') as fh:
            fh.write(gguf_minimal_llama.build(7))

        # ============================ 1. convert + run =================
        model = os.path.join(tmp, 'minilama.hydra2')
        rc = subprocess.run([sys.executable,
                             os.path.join(HERE, 'gguf_to_hydra2.py'),
                             gguf, model],
                            capture_output=True, text=True)
        check('converter exits 0', rc.returncode == 0, rc.stderr)
        rc1, t1 = run_engine(binary, model, 7, 24)
        check('engine runs the converted model', rc1 == 0 and t1,
              str((rc1, t1)))
        rc2, t2 = run_engine(binary, model, 7, 24)
        check('deterministic stream (two runs identical)',
              rc2 == 0 and t1 == t2, '%r vs %r' % (t1, t2))

        meta, tensors, edir, trunk, experts, raw = parse_v2(model)

        # ============================ 2. tamper ========================
        # a single flipped byte in the expert section must be caught by
        # the per-payload CRC32 the engine verifies at read time
        tampered = os.path.join(tmp, 'tampered.hydra2')
        shutil.copy(model, tampered)
        exp_off = struct.unpack_from('<Q', raw, 104)[0]
        with open(tampered, 'r+b') as fh:
            fh.seek(exp_off + 4)
            b = bytearray(fh.read(4))
            b[0] ^= 0xFF
            fh.seek(exp_off + 4)
            fh.write(b)
        rc3, t3 = run_engine(binary, tampered, 7, 24)
        check('flipped expert byte is refused (CRC32 mismatch)',
              rc3 != 0, 'engine accepted a tampered expert payload')

        # ============================ 3. embed file order ==============
        # The embedding's GGUF dims are ggml [dim, vocab] (ne[0] = the
        # contiguous dim); its FILE bytes are already the engine's
        # [vocab][dim] embed layout (token t's vector is the contiguous
        # run at offset t*dim). The converter must copy in file order -
        # any rearrangement (the pre-round-13 transpose) scrambles every
        # non-square tensor, and this non-square fixture (64 x 99) makes
        # the difference visible. Independent re-read, F32-exact.
        g = gguf_reader.GgufFile.open(gguf)
        dim = struct.unpack('<I', meta['v2.dim'])[0]
        vocab = struct.unpack('<I', meta['v2.vocab_size'])[0]
        tok_emb = g.find('token_embd.weight') or g.find('tok_emb')
        eshape = tok_emb.shape()
        ok = True
        detail = ''
        for r0 in range(0, eshape[0], 64):
            n = min(64, eshape[0] - r0)
            rows = g.decode_rows(tok_emb, r0, n)
            for i in range(n):
                d = r0 + i
                row = rows[i * eshape[1]:(i + 1) * eshape[1]]
                for v, val in enumerate(row):
                    if (f32_at_off(trunk, tensors['embed'][4]
                                   + (d * eshape[1] + v) * 4) != val):
                        ok = False
                        detail = 'i=%d' % (d * eshape[1] + v)
                        break
                if not ok:
                    break
            if not ok:
                break
        check('embed == tok_emb in GGUF file order (F32-exact)', ok, detail)

        # ============================ 4. expert == masked dense ========
        # The scaling must respect SiLU's nonlinearity: the engine
        # computes SiLU(G_e b) * (U_e b), so the GATE block stays
        # unscaled and only the UP block carries the factor E (with the
        # uniform router's 1/E weight the sum over the experts
        # reproduces the dense FFN). The pre-round-13 converter scaled
        # BOTH by E, which made every E > 1 model compute a different
        # function than the source FFN - this F32-exact re-computation
        # pins the corrected convention.
        inter = struct.unpack('<I', meta['v2.inter'])[0]
        E = struct.unpack('<I', meta['v2.experts_per_layer'])[0]
        topk = struct.unpack('<I', meta['v2.top_k'])[0]
        check('top_k == experts_per_layer == E (uniform router)',
              topk == E, 'top_k=%d E=%d' % (topk, E))
        layers = struct.unpack('<I', meta['v2.layers'])[0]
        chunk = inter // E
        ok = True
        detail = ''
        for l in range(layers):
            def dense(name):
                t = tensors[name]
                return [f32_at_off(trunk, t[4] + i * 4)
                        for i in range(t[3])]
            gate = dense('ffn_gate.%d' % l)
            up = dense('ffn_up.%d' % l)
            down = dense('ffn_down.%d' % l)
            for e in range(E):
                ent = edir[l * E + e]
                p = experts[ent[2]:ent[2] + ent[3]]
                ng = inter * dim
                ok_g = all(
                    f32_at(p, i) == (gate[e * chunk * dim + i]
                                     if (i // dim) >= e * chunk and
                                     (i // dim) < (e + 1) * chunk else 0.0)
                    for i in range(ng))
                ok_u = all(
                    f32_at(p, ng + i) == (E * up[e * chunk * dim + i]
                                          if (i // dim) >= e * chunk and
                                          (i // dim) < (e + 1) * chunk
                                          else 0.0)
                    for i in range(ng))
                ok_d = all(f32_at(p, 2 * ng + i) == down[i]
                           for i in range(dim * inter))
                if not (ok_g and ok_u and ok_d):
                    ok = False
                    detail = 'layer %d expert %d (g=%s u=%s d=%s)' % (
                        l, e, ok_g, ok_u, ok_d)
                    break
            if not ok:
                break
        check('experts == masked dense FFN, gate x1 / up xE (F32-exact)',
              ok, detail)

        # ============================ 5. vocab bytes ===================
        tv = tensors['vocab']
        check('vocab tensor is role 8 VOCAB_UTF8',
              tv[0] == 8 and tv[1] == 0x0050, str(tv))
        vblob = trunk[tv[4]:tv[4] + tv[5]]
        toks = vblob.split(b'\x00')
        check('vocab blob is NUL-terminated and ends with NUL',
              len(toks) == vocab + 1 and toks[-1] == b'')
        gtoks = g.tokens()
        check('embedded vocab == GGUF tokens (byte-exact)',
              list(toks[:-1]) == [t.encode('utf-8') for t in gtoks])

        # ============================ 6. metadata ======================
        check('metadata dim/vocab/layers/heads',
              dim == 64 and vocab == 99 and layers == 2
              and struct.unpack('<I', meta['v2.n_heads'])[0] == 4
              and struct.unpack('<I', meta['v2.n_kv_heads'])[0] == 4
              and struct.unpack('<I', meta['v2.head_dim'])[0] == 16
              and inter == 192)
        check('expert_bytes_typ == 3*inter*dim F32 bytes',
              struct.unpack('<Q', meta['v2.expert_bytes_typ'])[0]
              == 3 * inter * dim * 4)
        check('tied_embedding 0 (output present), bos from GGUF',
              struct.unpack('<I', meta['v2.tied_embedding'])[0] == 0
              and struct.unpack('<I', meta['v2.bos_token'])[0] == 96)

        # ============================ 7. router zeros ==================
        ok = True
        for l in range(layers):
            t = tensors['router.%d' % l]
            ok = ok and all(b == 0 for b in trunk[t[4]:t[4] + t[5]])
        check('router rows all zero (uniform routing)', ok)

        # ============================ 8. merges ========================
        build_with_merges(os.path.join(tmp, 'merges.gguf'))
        merges_model = os.path.join(tmp, 'merges.hydra2')
        rc = subprocess.run([sys.executable,
                             os.path.join(HERE, 'gguf_to_hydra2.py'),
                             os.path.join(tmp, 'merges.gguf'), merges_model],
                            capture_output=True, text=True)
        check('converter handles a merges-carrying GGUF',
              rc.returncode == 0, rc.stderr)
        m2, t2m, _, trunk2, _, _ = parse_v2(merges_model)
        check('merges tensor present, role 8 U16_PAIRS',
              t2m.get('merges', (None,))[0] == 8
              and t2m['merges'][1] == 0x0051, str(t2m.get('merges')))
        tm = t2m['merges']
        mblob = trunk2[tm[4]:tm[4] + tm[5]]
        rules = list(struct.unpack('<%dH' % (len(mblob) // 2), mblob))
        g3 = gguf_reader.GgufFile.open(os.path.join(tmp, "merges.gguf"))
        id_of = {t: i for i, t in enumerate(g3.tokens())}
        expected = []
        for m in g3.metadata['tokenizer.ggml.merges']:
            left, _, right = m.partition(' ')
            if (left in id_of and right in id_of and left + right in id_of):
                expected += [id_of[left], id_of[right]]
        check('merge rules == expected (left, right) pairs',
              rules == expected, '%r vs %r' % (rules, expected))
        rc4, t4 = run_engine(binary, merges_model, 3, 12)
        check('merges model runs in the engine', rc4 == 0 and t4)

        # ==================== 8b. E=1 vs E=2 equivalence ===============
        # Qodo #4 end-to-end proof: the same source GGUF converted with
        # --max-experts 1 (dense FFN verbatim) and with --max-experts 2
        # (two experts, up rows scaled by 2) must produce the IDENTICAL
        # greedy token stream in the engine. The pre-round-13 scaling
        # (both gate and up x E) broke this for every E > 1 because
        # SiLU(E*g) * u != E * (SiLU(g) * u).
        model_e2 = os.path.join(tmp, 'minilama-e2.hydra2')
        rce = subprocess.run([sys.executable,
                              os.path.join(HERE, 'gguf_to_hydra2.py'),
                              gguf, model_e2, '--max-experts', '2'],
                             capture_output=True, text=True)
        check('converter --max-experts 2 exits 0', rce.returncode == 0,
              rce.stderr)
        rc_e2, t_e2 = run_engine(binary, model_e2, 7, 24)
        check('E=2 greedy stream == E=1 dense stream (equivalence)',
              rc_e2 == 0 and t_e2 == t1, '%r vs %r' % (t_e2, t1))
        m_e2, _, _, _, _, _ = parse_v2(model_e2)
        check('E=2 model reports 2 experts/layer and top_k 2',
              struct.unpack('<I', m_e2['v2.experts_per_layer'])[0] == 2
              and struct.unpack('<I', m_e2['v2.top_k'])[0] == 2)

        # ==================== 8c. token normalisation ==================
        # Qodo #15: SPM/llama vocabularies store the leading space as
        # the U+2581 glyph, which has no byte-identity - the engine's
        # byte->token map cannot map 0x20 and --text-prompt refused on
        # EVERY real llama model. The converter now stores the
        # byte-equivalent string (' b' instead of '\u2581b') and merges
        # resolve against the normalised ids.
        build_with_glyphs(os.path.join(tmp, 'glyphs.gguf'))
        glyph_model = os.path.join(tmp, 'glyphs.hydra2')
        rcg = subprocess.run([sys.executable,
                              os.path.join(HERE, 'gguf_to_hydra2.py'),
                              os.path.join(tmp, 'glyphs.gguf'), glyph_model],
                             capture_output=True, text=True)
        check('converter normalises glyph tokens and exits 0',
              rcg.returncode == 0, rcg.stderr)
        m4, t4m, _, trunk4, _, _ = parse_v2(glyph_model)
        tv4 = t4m['vocab']
        vb4 = trunk4[tv4[4]:tv4[4] + tv4[5]]
        toks4 = vb4.split(b'\x00')[:-1]
        check('SPM "\u2581x" tokens stored byte-equivalent (" x")',
              toks4 == [b'a', b' b', b'a b'], repr(toks4))
        tm4 = t4m.get('merges')
        rules4 = (list(struct.unpack('<%dH' % (len(trunk4[
            tm4[4]:tm4[4] + tm4[5]]) // 2),
            trunk4[tm4[4]:tm4[4] + tm4[5]])) if tm4 else None)
        check('glyph merge rule resolved to normalised ids [0, 1]',
              rules4 == [0, 1], repr(rules4))
        rc5, t5 = run_engine(binary, glyph_model, 1, 12)
        check('normalised-glyph model runs in the engine',
              rc5 == 0 and t5)

        # ============================ 9. refusals ======================
        def must_fail(name, in_path, expect_substr):
            bad = os.path.join(tmp, 'bad-%s.hydra2' % name)
            rc = subprocess.run([sys.executable,
                                 os.path.join(HERE, 'gguf_to_hydra2.py'),
                                 in_path, bad],
                                capture_output=True, text=True)
            check('refusal: %s' % name,
                  rc.returncode == 2 and expect_substr in rc.stderr,
                  'rc=%d err=%r' % (rc.returncode, rc.stderr[:200]))

        txt = os.path.join(tmp, 'not-gguf.txt')
        with open(txt, 'wb') as fh:
            fh.write(b'this is not a gguf file')
        must_fail('non-gguf', txt, 'not a GGUF')

        # a GGUF whose embedding is stored as IQ2_XXS (ggml_type 16):
        # metadata and tokens are complete, so the ONLY problem is the
        # undecodable layout - that is what the converter must refuse
        # loudly. (Q4_0/Q4_1 itself is decodable since round 15 and is
        # covered by the parity section below.)
        q4 = os.path.join(tmp, 'q40.gguf')
        q4_w = gguf_minimal_llama.Writer()
        q4_w.s('general.architecture', 'llama')
        q4_w.s('tokenizer.ggml.model', 'gpt2')
        q4_w.arr('tokenizer.ggml.tokens', gguf_minimal_llama.KV_STRING,
                 None, ['a', 'b', 'c', 'd'])
        q4_w.u32('llama.embedding_length', 8)
        q4_w.u32('llama.attention.head_count', 2)
        q4_w.u32('llama.attention.head_count_kv', 2)
        q4_w.u32('llama.block_count', 1)
        q4_w.u32('llama.feed_forward_length', 16)
        # tok_emb as one 34-byte Q4_0 block (ggml_type 0x10)
        q4_w.tensors.append(
            ('tok_emb', [8, 4], 0x10, b'\x01\x00' * 17))
        with open(q4, 'wb') as fh:
            fh.write(q4_w.serialize())
        must_fail('undecodable-weights', q4, 'does not implement')

        badarch = os.path.join(tmp, 'badarch.gguf')
        with open(gguf, 'rb') as fh:
            data = fh.read()
        # rewrite the architecture VALUE (5 bytes after the u64 length)
        # from 'llama' to 'gpt2x' - same length, valid GGUF
        ki = data.find(b'general.architecture')
        vi = data.find(b'llama', ki)
        data = data[:vi] + b'gpt2x' + data[vi + 5:]
        with open(badarch, 'wb') as fh:
            fh.write(data)
        must_fail('non-llama-arch', badarch, 'only decoder-only')

        notok = os.path.join(tmp, 'notok.gguf')
        with open(gguf, 'rb') as fh:
            data = fh.read()
        # point the header at a metadata list without the tokens key by
        # truncating the metadata count to 1 (drops the tokenizer array)
        # - the loader still sees a valid GGUF, just without tokens.
        # rebuild a structurally complete GGUF WITHOUT the tokens array
        nt = gguf_minimal_llama.Writer()
        nt.s('general.architecture', 'llama')
        nt.s('tokenizer.ggml.model', 'gpt2')
        nt.u32('llama.embedding_length', 8)
        nt.u32('llama.attention.head_count', 2)
        nt.u32('llama.attention.head_count_kv', 2)
        nt.u32('llama.block_count', 1)
        nt.u32('llama.feed_forward_length', 16)
        nt.tensor('tok_emb', [8, 4], [0.0] * 32, 'f32')
        for l in range(1):
            nt.tensor('blk.%d.attn_q' % l, [8, 8], [0.0] * 64, 'f32')
            nt.tensor('blk.%d.attn_k' % l, [8, 8], [0.0] * 64, 'f32')
            nt.tensor('blk.%d.attn_v' % l, [8, 8], [0.0] * 64, 'f32')
            nt.tensor('blk.%d.attn_o' % l, [8, 8], [0.0] * 64, 'f32')
            nt.tensor('blk.%d.attn_norm' % l, [8], [1.0] * 8, 'f32')
            nt.tensor('blk.%d.ffn_gate' % l, [8, 16], [0.0] * 128, 'f32')
            nt.tensor('blk.%d.ffn_up' % l, [8, 16], [0.0] * 128, 'f32')
            nt.tensor('blk.%d.ffn_down' % l, [16, 8], [0.0] * 128, 'f32')
            nt.tensor('blk.%d.ffn_norm' % l, [8], [1.0] * 8, 'f32')
        nt.tensor('norm', [8], [1.0] * 8, 'f32')
        with open(notok, 'wb') as fh:
            fh.write(nt.serialize())
        must_fail('missing-tokens', notok, 'tokens')

        baddim = os.path.join(tmp, 'baddim.gguf')
        with open(gguf, 'rb') as fh:
            data = fh.read()
        # flip embedding_length 64 -> 63 in the metadata: after the 22-byte
        # key text comes a u32 value_type (4) and then the u32 value.
        key = b'llama.embedding_length'
        i = data.find(key)
        assert i != -1
        vpos = i + len(key) + 4  # u32 value right after the type marker
        assert struct.unpack_from('<I', data, i + len(key))[0] == 4
        data = data[:vpos] + struct.pack('<I', 63) + data[vpos + 4:]
        with open(baddim, 'wb') as fh:
            fh.write(data)
        must_fail('dim-mismatch', baddim, 'not divisible')

        # ---- token-level refusals (Qodo #2) ---------------------------
        # The engine's vocab loader REQUIRES no empty token, no embedded
        # NUL, no duplicates. The converter must refuse a GGUF that
        # violates any of these at conversion time (loudly, exit 2)
        # instead of emitting a .hydra2 the engine would refuse to open.
        etok = os.path.join(tmp, 'emptytok.gguf')
        build_tiny_with_tokens(etok, ['a', '', 'b'])
        must_fail('empty-token', etok, 'is empty')

        ntok = os.path.join(tmp, 'nultok.gguf')
        build_tiny_with_tokens(ntok, ['a', 'x\x00y', 'b'])
        must_fail('nul-token', ntok, 'embedded NUL')

        dtok = os.path.join(tmp, 'duptok.gguf')
        build_tiny_with_tokens(dtok, ['a', 'b', 'a'])
        must_fail('duplicate-token', dtok, 'duplicates')

        # ---- norm shape refusal (Qodo #24) ----------------------------
        # a norm tensor that exists but is the wrong shape would open
        # and compute garbage in the engine - refuse at conversion time
        bnorm = os.path.join(tmp, 'badnorm.gguf')
        build_tiny_with_tokens(bnorm, ['a', 'b', 'c'], norm_len=7)
        must_fail('norm-shape', bnorm, 'expected 1-D [dim')

        # ---- merge U16 overflow refusal (Qodo #9) ---------------------
        # a merge operand above 65535 cannot fit the 16-bit pair format;
        # struct.pack('<HH') would raise mid-conversion (uncaught
        # traceback after the expensive decode) - refuse before that
        bigtok = os.path.join(tmp, 'bigtok.gguf')
        # '%06d' so that id 65536 reads 't065536' (the merge operand
        # that must overflow the 16-bit pair format)
        build_tiny_with_tokens(bigtok, ['t%06d' % i for i in range(70000)]
                               + ['t000000t065536'],
                               merges=['t000000 t065536'])
        must_fail('merge-u16-overflow', bigtok, '16-bit')

        # ---- Q4_0 / Q4_1 (round 15: the 8B-class quants) -------------
        # 4-bit block-scaled GGUF (the format 8B downloads actually use)
        # converts and runs; the stored F32 must be BIT-EXACT against an
        # independent from-scratch decode of the ggml block format.
        def f32(x):
            return struct.unpack('<f', struct.pack('<f', x))[0]

        def indep_q4_decode(g, t, kind):
            bb = 18 if kind == 'q40' else 20
            n = t.n_elements
            if n % 32:
                raise AssertionError('element count not a multiple of 32')
            out = []
            for b0 in range(0, n, 32):
                off = t.data_start + (b0 // 32) * bb
                d = f32(struct.unpack_from('<e', g.data, off)[0])
                if kind == 'q41':
                    m = f32(struct.unpack_from('<e', g.data, off + 2)[0])
                    nbs = g.data[off + 4:off + 20]
                else:
                    m = 0.0
                    nbs = g.data[off + 2:off + 18]
                for k in range(16):
                    lo = nbs[k] & 0x0F
                    hi = nbs[k] >> 4
                    out.append(f32(f32((lo - 8) * d) + m))
                    out.append(f32(f32((hi - 8) * d) + m))
            return out

        for kind in ('q40', 'q41'):
            src = os.path.join(tmp, 'minilama-%s.gguf' % kind)
            with open(src, 'wb') as fh:
                fh.write(gguf_minimal_llama.build(7, dtype=kind))
            dst = os.path.join(tmp, 'minilama-%s.hydra2' % kind)
            rcc = subprocess.run([sys.executable,
                                  os.path.join(HERE, 'gguf_to_hydra2.py'),
                                  src, dst],
                                 capture_output=True, text=True)
            check('converter exits 0 on the %s fixture' % kind,
                  rcc.returncode == 0, rcc.stderr)
            ra, sa = run_engine(binary, dst, 7, 24)
            check('engine runs the %s-converted model' % kind,
                  ra == 0 and sa, str((ra, sa)))
            rb, sb = run_engine(binary, dst, 7, 24)
            check('%s stream deterministic (two runs identical)' % kind,
                  rb == 0 and sa == sb, '%r vs %r' % (sa, sb))
            if kind == 'q40':
                check('%s stream differs from the F32 stream '
                      '(quantisation is visible)' % kind, sa != t1,
                      'the 4-bit weights must change the tokens')
            gq = gguf_reader.GgufFile.open(src)
            tq = gq.find('token_embd.weight')
            ref = indep_q4_decode(gq, tq, kind)
            gq.close()
            _, ttensors, _, ttrunk, _, _ = parse_v2(dst)
            eoff = ttensors['embed'][4]
            ok = True
            detail = ''
            for i, val in enumerate(ref):
                if f32_at_off(ttrunk, eoff + i * 4) != val:
                    ok = False
                    detail = 'i=%d' % i
                    break
            check('%s embed == independent block decode (F32-exact)' % kind,
                  ok, detail)

        # ---- --t4 (round 16): Q4_0 resident, bit-exact raw copy ------
        # The Q4_0 source fixture from above is converted in --t4 mode:
        # the large tensors are stored as 18-byte Q4_0 blocks (the
        # source blocks are copied bit-exact, no re-quantization), the
        # experts are layout 2, and the engine dequantizes in-kernel.
        src = os.path.join(tmp, 'minilama-q40.gguf')
        dst4 = os.path.join(tmp, 'minilama-q40-t4.hydra2')
        rc4 = subprocess.run([sys.executable,
                              os.path.join(HERE, 'gguf_to_hydra2.py'),
                              '--t4', src, dst4],
                             capture_output=True, text=True)
        check('converter exits 0 with --t4 on the Q4_0 fixture',
              rc4.returncode == 0, rc4.stderr)
        ra, sa = run_engine(binary, dst4, 7, 24)
        check('engine runs the --t4 model', ra == 0 and sa,
              str((ra, sa)))
        rb, sb = run_engine(binary, dst4, 7, 24)
        check('--t4 stream deterministic (two runs identical)',
              rb == 0 and sa == sb, '%r vs %r' % (sa, sb))
        check('--t4 stream differs from the F32 stream '
              '(quantisation is visible)', sa != t1,
              'the 4-bit resident weights must change the tokens')
        # bit-exact raw copy: the stored embed payload must be the
        # source blocks themselves (the default path decodes Q4_0 to
        # F32; --t4 must not re-quantize).
        gq = gguf_reader.GgufFile.open(src)
        tq = gq.find('token_embd.weight')
        raw_src = gq.raw_bytes(tq)
        gq.close()
        _, ttensors, _, ttrunk, _, _ = parse_v2(dst4)
        eoff = ttensors['embed'][4]
        check('--t4 embed is a bit-exact copy of the source blocks',
              ttrunk[eoff:eoff + len(raw_src)] == raw_src,
              '%d bytes' % len(raw_src))

        # ---- quantizer parity (numpy optional, round 16) --------------
        # The vectorized fast path must produce byte-identical payloads
        # to the pure-Python reference. numpy is a CI-optional
        # dependency, so the check is skipped (loudly) without it.
        rng = random.Random(20261031)
        vals = []
        for _ in range(30016):  # 938 whole 32-value Q4_0 blocks
            r = rng.random()
            if r < 0.1:
                vals.append(0.0)
            else:
                vals.append((rng.random() * 2 - 1)
                            * 10 ** rng.randint(-8, 3))
        qbuf = struct.pack('<%df' % len(vals), *vals)
        if not gguf_to_hydra2._numpy_available():
            print('  skip: quantizer numpy parity (numpy not installed)')
        else:
            check('quantize_q40: numpy fast path is bit-identical to '
                  'the pure-Python reference',
                  gguf_to_hydra2._quantize_q40_python(qbuf)
                  == gguf_to_hydra2._quantize_q40_numpy(qbuf))

        # ---- --t4 loud refusal: dim not a multiple of 32 --------------
        bad = os.path.join(tmp, 'minilama-dim48.gguf')
        with open(bad, 'wb') as fh:
            # F32 source is fine: the refusal must fire on the dim,
            # before any payload is read.
            fh.write(gguf_minimal_llama.build(7, dtype='f32', dim=48))
        rc4b = subprocess.run([sys.executable,
                               os.path.join(HERE, 'gguf_to_hydra2.py'),
                               '--t4', bad, os.path.join(tmp, 'x.hydra2')],
                              capture_output=True, text=True)
        check('--t4 refuses dim % 32 != 0 (loud)',
              rc4b.returncode != 0 and 'divisible by 32' in rc4b.stderr,
              rc4b.stderr)

        # ---- 3B-geometry end-to-end (opt-in: HYDRA2_3B_E2E=1) --------
        # Real Llama-3.2-3B shapes (28 layers, dim 3072, heads 8/4,
        # inter 8192, vocab 128256, Q4_0 source ~1.7 GiB) through the
        # full pipeline: synthetic GGUF -> --t4 conversion (streaming)
        # -> engine inference. The real Llama weights are not
        # downloadable from the sandbox egress allowlist, so the
        # weights are synthetic (deterministic MT19937); the geometry
        # and the Q4_0 payload path are the real thing. Takes a few
        # minutes, so it is opt-in (the default suite stays fast).
        if os.environ.get('HYDRA2_3B_E2E') and not \
                gguf_to_hydra2._numpy_available():
            print('  skip: 3B E2E (numpy not installed)')
        elif os.environ.get('HYDRA2_3B_E2E'):
                src3 = os.path.join(tmp, 'llama3b-synthetic.gguf')
                out3 = os.path.join(tmp, 'llama3b.hydra2')
                r3 = subprocess.run(
                    [sys.executable, os.path.join(HERE, 'build_3b_synth_gguf.py'),
                     src3], capture_output=True, text=True, timeout=1800)
                check('3B: synthetic Q4_0 source builds (1.7 GiB)',
                      r3.returncode == 0 and os.path.exists(src3)
                      and os.path.getsize(src3) > 1 << 30,
                      r3.stderr[-200:])
                if r3.returncode == 0:
                    r3c = subprocess.run(
                        [sys.executable, os.path.join(HERE,
                                                       'gguf_to_hydra2.py'),
                         '--t4', src3, out3],
                        capture_output=True, text=True, timeout=1800)
                    check('3B: --t4 conversion succeeds (streaming)',
                          r3c.returncode == 0
                          and os.path.getsize(out3) > 2 << 30,
                          r3c.stderr[-200:])
                    if r3c.returncode == 0:
                        # run_engine returns (returncode, token list)
                        rc3a, t3a = run_engine(binary, out3, 128000, 6,
                                               timeout=600)
                        rc3b, t3b = run_engine(binary, out3, 128000, 6,
                                               timeout=600)
                        check('3B: engine runs and is deterministic',
                              rc3a == 0 and rc3b == 0
                              and t3a is not None and t3a == t3b,
                              ('%r %r vs %r %r' % (rc3a, t3a, rc3b, t3b))[:200])
                        check('3B: token stream has the requested count',
                              t3a is not None and len(t3a) == 6,
                              repr(t3a)[:100])

        print('\nAll gguf_to_hydra2 checks passed.')
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def build_with_merges(path):
    """A tiny llama GGUF WITH three tokens and one merge rule."""
    w = gguf_minimal_llama.Writer()
    w.s('general.architecture', 'llama')
    w.s('tokenizer.ggml.model', 'gpt2')
    w.u32('llama.embedding_length', 8)
    w.u32('llama.attention.head_count', 2)
    w.u32('llama.attention.head_count_kv', 2)
    w.u32('llama.block_count', 1)
    w.u32('llama.feed_forward_length', 16)
    w.f32('llama.rope.freq_base', 1234.0)
    w.u32('tokenizer.ggml.bos_token_id', 0)
    w.arr('tokenizer.ggml.tokens', gguf_minimal_llama.KV_STRING, None,
          ['a', 'b', 'ab'])
    w.arr('tokenizer.ggml.merges', gguf_minimal_llama.KV_STRING, None,
          ['a b', 'zz missing'])
    dim, ffn = 8, 16
    vals = lambda n: [((i * 7919) % 1000) / 1000.0 - 0.5 for i in range(n)]
    w.tensor('tok_emb', [dim, 3], vals(dim * 3), 'f32')
    w.tensor('blk.0.attn_q', [dim, dim], vals(dim * dim), 'f32')
    w.tensor('blk.0.attn_k', [dim, dim], vals(dim * dim), 'f32')
    w.tensor('blk.0.attn_v', [dim, dim], vals(dim * dim), 'f32')
    w.tensor('blk.0.attn_o', [dim, dim], vals(dim * dim), 'f32')
    w.tensor('blk.0.attn_norm', [dim], [1.0] * dim, 'f32')
    # ggml value order: a linear with dims [in, out] stores [out] rows
    # of [in] values (see tools/gguf_minimal_llama.py)
    w.tensor('blk.0.ffn_gate', [dim, ffn], vals(ffn * dim), 'f32')
    w.tensor('blk.0.ffn_up', [dim, ffn], vals(ffn * dim), 'f32')
    w.tensor('blk.0.ffn_down', [ffn, dim], vals(dim * ffn), 'f32')
    w.tensor('blk.0.ffn_norm', [dim], [1.0] * dim, 'f32')
    w.tensor('norm', [dim], [1.0] * dim, 'f32')
    w.tensor('output', [dim, 3], vals(dim * 3), 'f32')
    with open(path, 'wb') as fh:
        fh.write(w.serialize())


def build_with_glyphs(path):
    """A tiny llama GGUF whose vocabulary uses the SPM leading-space
    glyph U+2581 (like real llama.cpp conversions) plus one merge that
    only resolves through the normalised ids."""
    w = gguf_minimal_llama.Writer()
    w.s('general.architecture', 'llama')
    w.s('tokenizer.ggml.model', 'gpt2')
    w.u32('llama.embedding_length', 8)
    w.u32('llama.attention.head_count', 2)
    w.u32('llama.attention.head_count_kv', 2)
    w.u32('llama.block_count', 1)
    w.u32('llama.feed_forward_length', 16)
    w.u32('tokenizer.ggml.bos_token_id', 0)
    w.arr('tokenizer.ggml.tokens', gguf_minimal_llama.KV_STRING, None,
          ['a', '\u2581b', 'a\u2581b'])
    w.arr('tokenizer.ggml.merges', gguf_minimal_llama.KV_STRING, None,
          ['a \u2581b'])
    dim, ffn = 8, 16
    vals = lambda n: [((i * 7919) % 1000) / 1000.0 - 0.5 for i in range(n)]
    w.tensor('tok_emb', [dim, 3], vals(dim * 3), 'f32')
    w.tensor('blk.0.attn_q', [dim, dim], vals(dim * dim), 'f32')
    w.tensor('blk.0.attn_k', [dim, dim], vals(dim * dim), 'f32')
    w.tensor('blk.0.attn_v', [dim, dim], vals(dim * dim), 'f32')
    w.tensor('blk.0.attn_o', [dim, dim], vals(dim * dim), 'f32')
    w.tensor('blk.0.attn_norm', [dim], [1.0] * dim, 'f32')
    w.tensor('blk.0.ffn_gate', [dim, ffn], vals(ffn * dim), 'f32')
    w.tensor('blk.0.ffn_up', [dim, ffn], vals(ffn * dim), 'f32')
    w.tensor('blk.0.ffn_down', [ffn, dim], vals(dim * ffn), 'f32')
    w.tensor('blk.0.ffn_norm', [dim], [1.0] * dim, 'f32')
    w.tensor('norm', [dim], [1.0] * dim, 'f32')
    w.tensor('output', [dim, 3], vals(dim * 3), 'f32')
    with open(path, 'wb') as fh:
        fh.write(w.serialize())


def build_tiny_with_tokens(path, tokens, merges=None, norm_len=None):
    """A structurally complete 1-layer llama GGUF (all tensors F32,
    dim 8, ffn 16) with a caller-chosen token list - used by the
    refusal tests. norm_len replaces the norm shape (the Qodo #24
    refusal)."""
    w = gguf_minimal_llama.Writer()
    w.s('general.architecture', 'llama')
    w.s('tokenizer.ggml.model', 'gpt2')
    w.u32('llama.embedding_length', 8)
    w.u32('llama.attention.head_count', 2)
    w.u32('llama.attention.head_count_kv', 2)
    w.u32('llama.block_count', 1)
    w.u32('llama.feed_forward_length', 16)
    w.u32('tokenizer.ggml.bos_token_id', 0)
    w.arr('tokenizer.ggml.tokens', gguf_minimal_llama.KV_STRING, None,
          tokens)
    if merges:
        w.arr('tokenizer.ggml.merges', gguf_minimal_llama.KV_STRING, None,
              merges)
    dim, ffn = 8, 16
    vals = lambda n: [((i * 7919) % 1000) / 1000.0 - 0.5 for i in range(n)]
    w.tensor('tok_emb', [dim, len(tokens)], vals(dim * len(tokens)), 'f32')
    w.tensor('blk.0.attn_q', [dim, dim], vals(dim * dim), 'f32')
    w.tensor('blk.0.attn_k', [dim, dim], vals(dim * dim), 'f32')
    w.tensor('blk.0.attn_v', [dim, dim], vals(dim * dim), 'f32')
    w.tensor('blk.0.attn_o', [dim, dim], vals(dim * dim), 'f32')
    w.tensor('blk.0.attn_norm', [dim], [1.0] * dim, 'f32')
    w.tensor('blk.0.ffn_gate', [dim, ffn], vals(ffn * dim), 'f32')
    w.tensor('blk.0.ffn_up', [dim, ffn], vals(ffn * dim), 'f32')
    w.tensor('blk.0.ffn_down', [ffn, dim], vals(dim * ffn), 'f32')
    w.tensor('blk.0.ffn_norm', [dim], [1.0] * dim, 'f32')
    nlen = norm_len if norm_len is not None else dim
    w.tensor('norm', [nlen], vals(nlen), 'f32')
    w.tensor('output', [dim, len(tokens)], vals(dim * len(tokens)), 'f32')
    with open(path, 'wb') as fh:
        fh.write(w.serialize())


if __name__ == '__main__':
    sys.exit(main())

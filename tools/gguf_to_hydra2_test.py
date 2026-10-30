#!/usr/bin/env python3
"""End-to-end tests for tools/gguf_to_hydra2.py (round 12, M2.2/M3.1/M4).

    python3 tools/gguf_to_hydra2_test.py [path/to/hydra-run]

Builds llama GGUF fixtures (via tools/gguf_minimal_llama.py, the same
generator the v1 gate pins), converts them with the new converter, and
verifies:

  1. the converted file opens in the real engine and two runs produce
     the identical token stream (determinism);
  2. a single flipped byte in the expert section changes the outcome
     (refusal or different stream) - the weights actually flow;
  3. embed is exactly the transposed tok_emb (independent re-read of
     both files, F32-exact);
  4. every expert payload is exactly the masked/scaled dense FFN
     (independent re-computation from the dense trunk matrices);
  5. the embedded vocabulary is byte-exact (NUL-terminated tokens);
  6. the v2 metadata agrees with the GGUF architecture;
  7. the router rows are all-zero (the documented uniform router);
  8. merges: a GGUF WITH merge rules produces exactly the expected
     u16 pair rules; unresolvable rules are skipped;
  9. loud refusals: non-GGUF input, Q4_0 tensors, non-llama
     architecture, missing tokens, mismatched dimensions.

Every check exits non-zero on failure (the CI gate contract).
"""

import os
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

        # ============================ 3. embed transpose ===============
        g = gguf_reader.GgufFile.open(gguf)
        dim = struct.unpack('<I', meta['v2.dim'])[0]
        vocab = struct.unpack('<I', meta['v2.vocab_size'])[0]
        tok_emb = g.find('token_embd.weight') or g.find('tok_emb')
        ok = True
        detail = ''
        for r0 in range(0, dim, 64):
            rows = g.decode_rows(tok_emb, r0, min(64, dim - r0))
            n_cols = len(rows) // min(64, dim - r0)
            for i in range(min(64, dim - r0)):
                d = r0 + i
                row = rows[i * n_cols:(i + 1) * n_cols]
                for v, val in enumerate(row):
                    if (f32_at_off(trunk, tensors['embed'][4]
                                   + (v * dim + d) * 4) != val):
                        ok = False
                        detail = 'd=%d v=%d' % (d, v)
                        break
                if not ok:
                    break
            if not ok:
                break
        check('embed == transposed tok_emb (F32-exact)', ok, detail)

        # ============================ 4. expert == masked dense ========
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
                    f32_at(p, i) == (E * gate[e * chunk * dim + i]
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
        check('experts == E x masked dense FFN (F32-exact)', ok, detail)

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

        # a GGUF whose embedding is stored as Q4_0: metadata and tokens
        # are complete, so the ONLY problem is the undecodable layout -
        # that is what the converter must refuse loudly.
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
        must_fail('q4-0-weights', q4, 'does not implement')

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
    w.tensor('blk.0.ffn_gate', [dim, ffn], vals(dim * ffn), 'f32')
    w.tensor('blk.0.ffn_up', [dim, ffn], vals(dim * ffn), 'f32')
    w.tensor('blk.0.ffn_down', [ffn, dim], vals(ffn * dim), 'f32')
    w.tensor('blk.0.ffn_norm', [dim], [1.0] * dim, 'f32')
    w.tensor('norm', [dim], [1.0] * dim, 'f32')
    w.tensor('output', [dim, 3], vals(dim * 3), 'f32')
    with open(path, 'wb') as fh:
        fh.write(w.serialize())


if __name__ == '__main__':
    sys.exit(main())

/* hydra_moe.c - F32 sparse-MoE inference on the .hydra2 container.
 *
 * Round 12 design (session.md R12, FORMAT-V2.md): additive module,
 * v1-v4 engine paths untouched, container stays structural (this
 * module enforces the v2.* inference metadata). See include/hydra_moe.h
 * for the model definition and error codes.
 *
 * Arithmetic discipline: all mat-vecs are plain f32 accumulators in
 * source order (no reassociation, no SIMD - the stream pins of this
 * module are defined by THIS summation order, the same discipline the
 * v3/v4 float paths keep so the pinned legacy streams stay valid).
 */
#include "hydra_moe.h"
#include "hydra_v2_container.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* internals                                                          */
/* ------------------------------------------------------------------ */

/* The handle struct lives in include/hydra_moe.h (public, the
 * repository pattern); this file defines the methods. */
typedef hydra_moe_tensor_t moe_tensor_t;
typedef hydra_moe_layer_t moe_layer_t;

#define MOE_NAN_CHECK(v) (!(isfinite(v)))

/* Loud refusal helper: the 5.2 discipline (name the file, the field,
 * the value, and the section). Returns the error code to propagate. */
static int moe_refuse(const char *path, const char *what,
                      const char *detail, int code)
{
    fprintf(stderr, "[MoE] %s: %s: %s\n", path ? path : "(null)", what, detail);
    fprintf(stderr, "[MoE] see docs/FORMAT-V2.md section 5.2 (loud refusals)\n");
    return code;
}

/* ------------------------------------------------------------------ */
/* metadata access (the v2.* inference keys, enforced HERE)           */
/* ------------------------------------------------------------------ */

/* Find a metadata key; returns NULL when absent. Key/value bytes live
 * in the container's flat meta blob at the kv offsets (one
 * allocation, no sub-pointers). */
static const hydra2_meta_kv_t *moe_find_meta(const hydra_moe_t *m,
                                             const char *key)
{
    for (uint64_t i = 0; i < m->container.meta_count; i++) {
        const hydra2_meta_kv_t *kv = &m->container.meta[i];
        if (kv->key_len == strlen(key) &&
            memcmp(m->container.meta_blob + kv->key_off, key, kv->key_len) == 0)
            return kv;
    }
    return NULL;
}

/* Read a u32 metadata value with a default; refuses on absent/non-u32.
 * require = 1: absent is a refusal; 0: absent yields *out = def. */
static int moe_meta_u32(const char *path, const hydra_moe_t *m,
                        const char *key, uint32_t def, int require,
                        uint32_t *out)
{
    const hydra2_meta_kv_t *kv = moe_find_meta(m, key);
    if (!kv) {
        if (require) {
            char d[160];
            snprintf(d, sizeof d, "required metadata key '%s' is missing", key);
            return moe_refuse(path, "metadata", d, -4);
        }
        *out = def;
        return 0;
    }
    if (kv->val_len != 4) {
        char d[160];
        snprintf(d, sizeof d, "metadata key '%s' must be a 4-byte u32, got %u bytes",
                 key, (unsigned)kv->val_len);
        return moe_refuse(path, "metadata", d, -4);
    }
    const uint8_t *p = m->container.meta_blob + kv->val_off;
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    *out = v;
    return 0;
}

/* Read a u32-valued metadata key that stores a float (rope_theta is
 * a u32 bit pattern of an f32, the container's only 4-byte float
 * convention). */
static int moe_meta_f32(const char *path, const hydra_moe_t *m,
                        const char *key, uint32_t def_bits, int require,
                        double *out)
{
    uint32_t bits;
    int rc = moe_meta_u32(path, m, key, def_bits, require, &bits);
    if (rc) return rc;
    float f;
    memcpy(&f, &bits, sizeof f);
    if (!isfinite(f) || f <= 0.0f) {
        char d[160];
        snprintf(d, sizeof d, "metadata key '%s' must be a positive finite float", key);
        return moe_refuse(path, "metadata", d, -4);
    }
    *out = (double)f;
    return 0;
}

/* ------------------------------------------------------------------ */
/* tensor resolution                                                  */
/* ------------------------------------------------------------------ */

/* Resolve ONE required trunk tensor by name into t.
 * want_type -1 = any known value type (F32 only is accepted in
 * layout 0, so this is HYDRA2_F32 for everything in this round). */
/* Flat tensor dequant (paired '<name>.scale' convention); the full
 * implementation is with the other dequant helpers below. */
int hydra_moe_dequant_flat(const uint8_t *payload, const float *scales,
                           uint64_t n, uint32_t group, float *out);

static int moe_get_tensor(const char *path, hydra_moe_t *m, const char *name,
                          uint64_t want_nelem, moe_tensor_t *t)
{
    const hydra2_tensor_entry_t *e;
    int rc = hydra2_find_tensor(&m->container, name, &e);
    if (rc != HYDRA2_OK) {
        char d[190];
        snprintf(d, sizeof d, "required tensor '%s' is missing", name);
        return moe_refuse(path, "tensor", d, -5);
    }
    if (e->type_id != HYDRA2_F32 &&
        e->type_id != HYDRA2_TERNARY2_S64 &&
        e->type_id != HYDRA2_TERNARY2_S128 &&
        e->type_id != HYDRA2_Q4_0) {
        char d[230];
        snprintf(d, sizeof d,
                 "tensor '%s' must be F32, a block-scaled ternary "
                 "(0x0040/0x0041) or Q4_0 (0x0030), got type 0x%04X "
                 "('%s')",
                 name, e->type_id, hydra2_type_name(e->type_id));
        return moe_refuse(path, "tensor", d, -6);
    }
    if (e->n_elements != want_nelem) {
        char d[190];
        snprintf(d, sizeof d, "tensor '%s' has %llu elements, need %llu",
                 name, (unsigned long long)e->n_elements,
                 (unsigned long long)want_nelem);
        return moe_refuse(path, "tensor", d, -6);
    }
    t->name = name;
    t->n_elements = e->n_elements;
    t->type_id = e->type_id;
    /* The ternary payload is n/2 bytes on disk but materializes to n
     * F32 values: the buffer is sized for the DESTINATION width.
     * Q4_0 (round 16) is the opposite: the buffer holds the RAW 18-byte
     * blocks (n_bytes) and is never materialized - the mat-vec path
     * dequantizes one row at a time, keeping the resident memory 4-bit. */
    const uint64_t alloc = (e->type_id == HYDRA2_F32 ||
                            e->type_id == HYDRA2_Q4_0) ? e->n_bytes
        : e->n_elements * 4u;
    t->data = (float *)malloc(alloc ? alloc : 1);
    if (!t->data) return moe_refuse(path, "memory", "out of memory", -9);
    if (hydra2_read_tensor(&m->container, e,
                           (uint8_t *)t->data, e->n_bytes) != HYDRA2_OK) {
        free(t->data);
        t->data = NULL;
        char d[190];
        snprintf(d, sizeof d, "tensor '%s' could not be read from the trunk", name);
        return moe_refuse(path, "trunk read", d, -2);
    }
    if (e->type_id == HYDRA2_TERNARY2_S64 ||
        e->type_id == HYDRA2_TERNARY2_S128) {
        /* Block-scaled ternary: materialize through the PAIRED
         * '<name>.scale' tensor (FORMAT-V2.md 3.2.1). Q4_0 (round 16)
         * deliberately does NOT materialize - the raw blocks stay
         * resident and the mat-vec dequantizes per row. */
        const uint32_t group = (e->type_id == HYDRA2_TERNARY2_S64)
            ? 64u : 128u;
        const uint64_t nscale = (e->n_elements + group - 1u) / group;
        char sname[40];
        snprintf(sname, sizeof sname, "%s.scale", name);
        const hydra2_tensor_entry_t *se;
        if (hydra2_find_tensor(&m->container, sname, &se) != HYDRA2_OK) {
            char d[190];
            snprintf(d, sizeof d, "tensor '%s' is block-scaled ternary but "
                     "its paired F32 tensor '%s' is missing", name, sname);
            free(t->data); t->data = NULL;
            return moe_refuse(path, "tensor", d, -6);
        }
        if (se->type_id != HYDRA2_F32 || se->n_elements != nscale) {
            char d[200];
            snprintf(d, sizeof d, "paired tensor '%s' must be F32 with "
                     "%llu elements, got type 0x%04X / %llu",
                     sname, (unsigned long long)nscale, se->type_id,
                     (unsigned long long)se->n_elements);
            free(t->data); t->data = NULL;
            return moe_refuse(path, "tensor", d, -6);
        }
        float *scales = (float *)malloc((nscale ? nscale : 1) * sizeof(float));
        if (!scales) {
            free(t->data); t->data = NULL;
            return moe_refuse(path, "memory", "out of memory", -9);
        }
        if (hydra2_read_tensor(&m->container, se,
                               (uint8_t *)scales, se->n_bytes) != HYDRA2_OK) {
            free(scales); free(t->data); t->data = NULL;
            char d[190];
            snprintf(d, sizeof d, "tensor '%s' could not be read from the "
                     "trunk", sname);
            return moe_refuse(path, "trunk read", d, -2);
        }
        /* Dequantize through a SEPARATE packed buffer. The payload
         * (2 elements/byte) is read at byte k/2 while the F32 result
         * is written at byte 4k: with src == dst the write at 4k
         * corrupts the packed bytes still to be read, so in-place
         * dequant silently scrambles the tensor (round-13, Qodo #3;
         * the unit test replays the in-place pattern and shows the
         * first corrupted element). The temporary is exactly the disk
         * payload. */
        uint8_t *packed = (uint8_t *)malloc(e->n_bytes ? e->n_bytes : 1u);
        if (!packed) {
            free(scales); free(t->data); t->data = NULL;
            return moe_refuse(path, "memory", "out of memory", -9);
        }
        memcpy(packed, t->data, e->n_bytes);
        if (hydra_moe_dequant_flat(packed, scales, e->n_elements, group,
                                   t->data) != 0) {
            free(packed); free(scales); free(t->data); t->data = NULL;
            return moe_refuse(path, "tensor",
                              "ternary dequantization failed", -6);
        }
        free(packed);
        free(scales);
    }
    return 0;
}

static void moe_free_tensor(moe_tensor_t *t)
{
    if (t->data) { free(t->data); t->data = NULL; }
    t->n_elements = 0;
}

/* ------------------------------------------------------------------ */
/* open / close / config                                              */
/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/* vocabulary + BPE (role 8, FORMAT-V2.md 3.2.3)                      */
/* ------------------------------------------------------------------ */

/* UTF-8 validation (RFC 3629): 0 when the whole buffer is valid,
 * -1 when not (and *err_off set to the offset of the first invalid
 * byte; pass NULL when the offset is not needed). No overlongs, no
 * surrogates, no > U+10FFFF. Round-13 (Qodo #20): the old single
 * return value used 0 for BOTH "valid" and "invalid at offset 0",
 * so an invalid leading byte slipped past every caller. */
static int moe_utf8_first_error(const uint8_t *p, uint32_t len,
                                uint32_t *err_off)
{
    uint32_t i = 0;
    while (i < len) {
        const uint8_t b = p[i];
        uint32_t need;
        uint32_t cp;
        if (b < 0x80u) { i += 1; continue; }
        if (b < 0xC2u) goto invalid;               /* 0x80-0xC1 overlong */
        if (b < 0xE0u) { need = 1; cp = b & 0x1Fu; }
        else if (b < 0xF0u) { need = 2; cp = b & 0x0Fu; }
        else if (b < 0xF5u) { need = 3; cp = b & 0x07u; }
        else goto invalid;                         /* > U+10FFFF */
        if (i + need > len) goto invalid;
        for (uint32_t k = 1; k <= need; k++) {
            if (p[i + k] < 0x80u || p[i + k] >= 0xC0u) goto invalid;
            cp = (cp << 6) | (p[i + k] & 0x3Fu);
        }
        if (need == 1 && cp < 0x20u) goto invalid; /* overlong */
        if (need == 2 && cp < 0x800u) goto invalid;/* overlong */
        if (need == 3 && (cp < 0x10000u ||
                          (cp >= 0xD800u && cp <= 0xDFFFu) ||
                          cp > 0x10FFFFu)) goto invalid;
        i += 1 + need;
    }
    return 0;
invalid:
    if (err_off) *err_off = i;
    return -1;
}

/* FNV-1a over the token strings; open-addressing map (empty slot =
 * keylen 0). Used at open time only: duplicate-string detection and
 * merge-target lookup. */
typedef struct moe_vstr {
    const uint8_t *key;
    uint32_t keylen;
    uint32_t id;
} moe_vstr_t;

static uint32_t moe_fnv1a(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* Returns 0, or -1 on a duplicate / full table. */
static int moe_vstr_put(moe_vstr_t *tab, uint32_t cap,
                        const uint8_t *key, uint32_t keylen, uint32_t id)
{
    uint32_t slot = moe_fnv1a(key, keylen) & (cap - 1u);
    for (uint32_t probes = 0; probes < cap; probes++, slot = (slot + 1u) & (cap - 1u)) {
        if (tab[slot].keylen == 0u) {
            tab[slot].key = key;
            tab[slot].keylen = keylen;
            tab[slot].id = id;
            return 0;
        }
        if (tab[slot].keylen == keylen &&
            memcmp(tab[slot].key, key, keylen) == 0)
            return -1; /* duplicate */
    }
    return -1; /* full */
}

static const moe_vstr_t *moe_vstr_get(const moe_vstr_t *tab, uint32_t cap,
                                      const uint8_t *key, uint32_t keylen)
{
    uint32_t slot = moe_fnv1a(key, keylen) & (cap - 1u);
    for (uint32_t probes = 0; probes < cap; probes++, slot = (slot + 1u) & (cap - 1u)) {
        if (tab[slot].keylen == 0u) return NULL;
        if (tab[slot].keylen == keylen &&
            memcmp(tab[slot].key, key, keylen) == 0)
            return &tab[slot];
    }
    return NULL;
}

static uint32_t moe_pow2(uint32_t x)
{
    uint32_t p = 16;
    while (p < x) p <<= 1;
    return p;
}

static int moe_load_vocab(const char *path, hydra_moe_t *m,
                          const hydra2_tensor_entry_t *tv)
{
    if (tv->n_elements != m->cfg.vocab_size) {
        char d[190];
        snprintf(d, sizeof d, "tensor 'vocab' has %llu token strings but "
                 "v2.vocab_size is %u",
                 (unsigned long long)tv->n_elements, m->cfg.vocab_size);
        return moe_refuse(path, "tensor", d, -6);
    }
    m->vocab_blob = (uint8_t *)malloc(tv->n_bytes);
    if (!m->vocab_blob)
        return moe_refuse(path, "memory", "out of memory", -9);
    if (hydra2_read_tensor(&m->container, tv, m->vocab_blob,
                           tv->n_bytes) != HYDRA2_OK) {
        free(m->vocab_blob); m->vocab_blob = NULL;
        return moe_refuse(path, "trunk read",
                          "tensor 'vocab' could not be read", -2);
    }
    /* NUL walk: the last byte is NUL, no two NULs are adjacent (an
     * empty token), and there are exactly n_elements NULs. */
    const uint8_t *b = m->vocab_blob;
    const uint64_t nb = tv->n_bytes;
    if (b[nb - 1u] != 0u)
        return moe_refuse(path, "tensor",
                          "vocab payload does not end with NUL", -6);
    uint64_t nnul = 0;
    for (uint64_t i = 0; i < nb; i++) {
        if (b[i] == 0u) {
            if (i + 1u < nb && b[i + 1u] == 0u)
                return moe_refuse(path, "tensor",
                                  "vocab payload has an empty token "
                                  "string", -6);
            nnul++;
        }
    }
    if (nnul != tv->n_elements)
        return moe_refuse(path, "tensor",
                          "vocab payload NUL count does not match "
                          "n_elements", -6);

    m->vocab_off = (uint32_t *)malloc(tv->n_elements * 4u);
    m->vocab_len = (uint32_t *)malloc(tv->n_elements * 4u);
    if (!m->vocab_off || !m->vocab_len)
        return moe_refuse(path, "memory", "out of memory", -9);
    const uint32_t cap = moe_pow2((uint32_t)tv->n_elements * 4u);
    moe_vstr_t *tab = (moe_vstr_t *)calloc(cap, sizeof(moe_vstr_t));
    if (!tab)
        return moe_refuse(path, "memory", "out of memory", -9);
    for (int i = 0; i < 256; i++) m->byte_map[i] = -1;

    uint64_t off = 0;
    for (uint32_t i = 0; i < tv->n_elements; i++) {
        uint32_t len = 0;
        while (b[off + len] != 0u) len++;
        /* UTF-8 per string (the whole-blob check is implied) */
        if (moe_utf8_first_error(b + off, len, NULL) != 0) {
            char d[150];
            snprintf(d, sizeof d, "vocab token %u is not valid UTF-8", i);
            free(tab);
            return moe_refuse(path, "tensor", d, -6);
        }
        m->vocab_off[i] = (uint32_t)off;
        m->vocab_len[i] = len;
        if (moe_vstr_put(tab, cap, b + off, len, i) != 0) {
            char d[150];
            snprintf(d, sizeof d, "vocab token %u duplicates an earlier "
                     "token string - the byte map and merge targets "
                     "would be ambiguous", i);
            free(tab);
            return moe_refuse(path, "tensor", d, -6);
        }
        if (len == 1u)
            m->byte_map[b[off]] = (int32_t)i;
        off += (uint64_t)len + 1u;
    }
    free(tab);
    m->has_vocab = 1;
    return 0;
}

static int moe_load_merges(const char *path, hydra_moe_t *m,
                           const hydra2_tensor_entry_t *tm)
{
    m->merges = (hydra_moe_merge_t *)malloc(
        tm->n_elements * sizeof(hydra_moe_merge_t));
    uint8_t *payload = (uint8_t *)malloc(tm->n_bytes);
    if (!m->merges || !payload)
        return moe_refuse(path, "memory", "out of memory", -9);
    if (hydra2_read_tensor(&m->container, tm, payload, tm->n_bytes)
        != HYDRA2_OK) {
        free(payload); free(m->merges);
        m->merges = NULL;
        return moe_refuse(path, "trunk read",
                          "tensor 'merges' could not be read", -2);
    }
    /* target lookup needs the string map again (open-time only) */
    const uint32_t cap = moe_pow2((uint32_t)m->cfg.vocab_size * 4u);
    moe_vstr_t *tab = (moe_vstr_t *)calloc(cap, sizeof(moe_vstr_t));
    if (!tab) {
        free(payload); free(m->merges);
        m->merges = NULL;
        return moe_refuse(path, "memory", "out of memory", -9);
    }
    for (uint32_t i = 0; i < m->cfg.vocab_size; i++)
        (void)moe_vstr_put(tab, cap, m->vocab_blob + m->vocab_off[i],
                           m->vocab_len[i], i);
    for (uint32_t i = 0; i < tm->n_elements; i++) {
        const uint16_t *pr = (const uint16_t *)(payload + (uint64_t)i * 4u);
        const uint32_t left = pr[0], right = pr[1];
        if (left >= m->cfg.vocab_size || right >= m->cfg.vocab_size) {
            char d[190];
            snprintf(d, sizeof d, "merge rule %u references token id %u/%u "
                     "outside the vocabulary (vocab_size %u)",
                     i, left, right, m->cfg.vocab_size);
            free(tab); free(payload); free(m->merges);
            m->merges = NULL;
            return moe_refuse(path, "tensor", d, -6);
        }
        uint8_t cat[2 * 256]; /* tokens are UTF-8, max len bounded by blob */
        const uint32_t ll = m->vocab_len[left], rl = m->vocab_len[right];
        if ((uint64_t)ll + rl > sizeof cat) {
            free(tab); free(payload); free(m->merges);
            m->merges = NULL;
            return moe_refuse(path, "tensor",
                              "merge target string overflows the lookup "
                              "buffer", -6);
        }
        memcpy(cat, m->vocab_blob + m->vocab_off[left], ll);
        memcpy(cat + ll, m->vocab_blob + m->vocab_off[right], rl);
        const moe_vstr_t *hit = moe_vstr_get(tab, cap, cat, ll + rl);
        if (!hit) {
            char d[190];
            snprintf(d, sizeof d, "merge rule %u (token %u + token %u) "
                     "has no target token in the vocabulary",
                     i, left, right);
            free(tab); free(payload); free(m->merges);
            m->merges = NULL;
            return moe_refuse(path, "tensor", d, -6);
        }
        m->merges[i].left = left;
        m->merges[i].right = right;
        m->merges[i].target = hit->id;
    }
    m->merge_count = tm->n_elements;
    free(tab);
    free(payload);
    return 0;
}

/* Quiet tensor lookup (no stderr on absence): for OPTIONAL tensors
 * whose absence is legal (vocab/merges, FORMAT-V2.md 3.2.3). */
static const hydra2_tensor_entry_t *moe_find_quiet(const hydra_moe_t *m,
                                                   const char *name)
{
    const uint64_t n = m->container.tensor_dir_count;
    const hydra2_tensor_entry_t *t = m->container.tensors;
    for (uint64_t i = 0; i < n; i++)
        if (strcmp(t[i].name, name) == 0)
            return &t[i];
    return NULL;
}

int hydra_moe_open(const char *path, hydra_moe_t *m)
{
    if (!path || !m) return -1;
    memset(m, 0, sizeof *m);
    if (hydra2_open(&m->container, path) != HYDRA2_OK) {
        moe_refuse(path, "container", "the .hydra2 reader refused the file "
                     "(see its message above)", -2);
        return -2;
    }
    m->container_open = 1;

    if (strcmp(m->container.architecture, "moe-transformer-v1") != 0) {
        char d[190];
        snprintf(d, sizeof d, "architecture '%s' is not 'moe-transformer-v1'",
                 m->container.architecture);
        int rc = moe_refuse(path, "architecture", d, -3);
        hydra2_close(&m->container);
        m->container_open = 0;
        return rc;
    }

    /* --- inference metadata (enforced here, 4.4 / R12 design) ----- */
    uint32_t layers = 0, epl = 0, topk = 0;
    int rc = moe_meta_u32(path, m, "v2.dim", 0, 1, &m->cfg.dim);
    if (!rc) rc = moe_meta_u32(path, m, "v2.vocab_size", 0, 1, &m->cfg.vocab_size);
    if (!rc) rc = moe_meta_u32(path, m, "v2.n_heads", 0, 1, &m->cfg.n_heads);
    if (!rc) rc = moe_meta_u32(path, m, "v2.n_kv_heads", 0, 1, &m->cfg.n_kv_heads);
    if (!rc) rc = moe_meta_u32(path, m, "v2.head_dim", 0, 1, &m->cfg.head_dim);
    if (!rc) rc = moe_meta_u32(path, m, "v2.inter", 0, 1, &m->cfg.inter);
    if (!rc) rc = moe_meta_u32(path, m, "v2.layers", 0, 1, &layers);
    if (!rc) rc = moe_meta_u32(path, m, "v2.expert_layout", 0, 1, &m->cfg.expert_layout);
    if (!rc) rc = moe_meta_f32(path, m, "v2.rope_theta",
                               (uint32_t)0x41200000u /* 10000.0f bits */, 0,
                               &m->cfg.rope_theta);
    if (!rc) rc = moe_meta_u32(path, m, "v2.kv_sink", 4, 0, &m->cfg.kv_sink);
    if (!rc) rc = moe_meta_u32(path, m, "v2.kv_ring", 128, 0, &m->cfg.kv_ring);
    if (!rc) rc = moe_meta_u32(path, m, "v2.tied_embedding", 1, 0,
                               (uint32_t *)&m->cfg.tied_embedding);
    if (!rc) rc = moe_meta_u32(path, m, "v2.bos_token", 0, 0, &m->cfg.bos_token);
    if (!rc) rc = moe_meta_u32(path, m, "v2.experts_per_layer", 0, 0, &epl);
    if (!rc) rc = moe_meta_u32(path, m, "v2.top_k", 0, 0, &topk);
    if (rc) goto fail;

    /* The container's own routing metadata must agree with v2.* -
     * the container is structural, but a disagreeing file is a bug in
     * the writer, and it must be refused loudly. */
    if (layers != m->container.layers) {
        char d[190];
        snprintf(d, sizeof d, "v2.layers %u != container v2.layers %u",
                 layers, (unsigned)m->container.layers);
        rc = moe_refuse(path, "metadata", d, -4);
        goto fail;
    }
    if (epl != m->container.experts_per_layer) {
        char d[190];
        snprintf(d, sizeof d, "v2.experts_per_layer %u != container %u",
                 epl, (unsigned)m->container.experts_per_layer);
        rc = moe_refuse(path, "metadata", d, -4);
        goto fail;
    }
    if (topk != m->container.top_k) {
        char d[190];
        snprintf(d, sizeof d, "v2.top_k %u != container %u",
                 topk, (unsigned)m->container.top_k);
        rc = moe_refuse(path, "metadata", d, -4);
        goto fail;
    }
    m->cfg.layers = layers;
    m->cfg.experts_per_layer = epl;
    m->cfg.top_k = topk;

    /* --- shape relations (7) --------------------------------------
     * Round 13 (Qodo #17/#26): every product is 64-bit - in 32-bit
     * arithmetic n_heads * head_dim could WRAP AROUND and accidentally
     * 'equal' dim (e.g. 65536 * 65536 % 2^32 == 0) - and GQA requires
     * n_heads % n_kv_heads == 0 (the block mapping head i -> kv head
     * i / (n_heads / n_kv_heads) is only defined then; llama.cpp
     * convention). */
    if (m->cfg.dim == 0 || m->cfg.vocab_size == 0 || m->cfg.n_heads == 0 ||
        m->cfg.head_dim == 0 || m->cfg.inter == 0 || m->cfg.layers == 0 ||
        m->cfg.experts_per_layer == 0 || m->cfg.top_k == 0 ||
        m->cfg.top_k > m->cfg.experts_per_layer ||
        m->cfg.n_kv_heads < 1 || m->cfg.n_kv_heads > m->cfg.n_heads ||
        m->cfg.n_heads % m->cfg.n_kv_heads != 0 ||
        (uint64_t)m->cfg.n_heads * (uint64_t)m->cfg.head_dim !=
            m->cfg.dim ||
        m->cfg.expert_layout > 2 ||
        m->cfg.kv_sink > 0x10000u || m->cfg.kv_ring > 0x10000u) {
        char d[240];
        snprintf(d, sizeof d,
                 "shape relation violated: dim=%u vocab=%u n_heads=%u "
                 "n_kv_heads=%u head_dim=%u inter=%u layers=%u epl=%u "
                 "top_k=%u layout=%u",
                 m->cfg.dim, m->cfg.vocab_size, m->cfg.n_heads,
                 m->cfg.n_kv_heads, m->cfg.head_dim, m->cfg.inter,
                 m->cfg.layers, m->cfg.experts_per_layer, m->cfg.top_k,
                 m->cfg.expert_layout);
        rc = moe_refuse(path, "shape", d, -7);
        goto fail;
    }

    /* --- trunk tensors -------------------------------------------- */
    rc = moe_get_tensor(path, m, "embed",
                        (uint64_t)m->cfg.vocab_size * m->cfg.dim, &m->embed);
    if (rc) goto fail;
    rc = moe_get_tensor(path, m, "final_norm", m->cfg.dim, &m->final_norm);
    if (rc) goto fail;
    m->has_lm_head = 0;
    if (m->cfg.tied_embedding) {
        /* optional lm_head: resolve if present, else tied. The presence
         * probe scans the directory directly (hydra2_find_tensor would
         * print the 5.2 refusal for a tensor that is OPTIONAL). */
        int present = 0;
        for (uint64_t i = 0; i < m->container.tensor_dir_count; i++) {
            if (strncmp(m->container.tensors[i].name, "lm_head", 32) == 0) {
                present = 1;
                break;
            }
        }
        if (present) {
            rc = moe_get_tensor(path, m, "lm_head",
                                (uint64_t)m->cfg.vocab_size * m->cfg.dim,
                                &m->lm_head);
            if (rc) goto fail;
            m->has_lm_head = 1;
        }
    } else {
        rc = moe_get_tensor(path, m, "lm_head",
                            (uint64_t)m->cfg.vocab_size * m->cfg.dim,
                            &m->lm_head);
        if (rc) goto fail;
        m->has_lm_head = 1;
    }

    m->layers = (moe_layer_t *)calloc(m->cfg.layers, sizeof(moe_layer_t));
    if (!m->layers) { rc = moe_refuse(path, "memory", "out of memory", -9); goto fail; }

    char name[96];
    for (uint32_t l = 0; l < m->cfg.layers; l++) {
        moe_layer_t *L = &m->layers[l];
        snprintf(name, sizeof name, "attn_q.%u", l);
        rc = moe_get_tensor(path, m, name, (uint64_t)m->cfg.dim * m->cfg.dim, &L->attn_q);
        if (rc) goto fail;
        snprintf(name, sizeof name, "attn_k.%u", l);
        rc = moe_get_tensor(path, m, name,
                            (uint64_t)m->cfg.n_kv_heads * m->cfg.head_dim * m->cfg.dim,
                            &L->attn_k);
        if (rc) goto fail;
        snprintf(name, sizeof name, "attn_v.%u", l);
        rc = moe_get_tensor(path, m, name,
                            (uint64_t)m->cfg.n_kv_heads * m->cfg.head_dim * m->cfg.dim,
                            &L->attn_v);
        if (rc) goto fail;
        snprintf(name, sizeof name, "attn_o.%u", l);
        rc = moe_get_tensor(path, m, name, (uint64_t)m->cfg.dim * m->cfg.dim, &L->attn_o);
        if (rc) goto fail;
        snprintf(name, sizeof name, "attn_norm.%u", l);
        rc = moe_get_tensor(path, m, name, m->cfg.dim, &L->attn_norm);
        if (rc) goto fail;
        snprintf(name, sizeof name, "ffn_norm.%u", l);
        rc = moe_get_tensor(path, m, name, m->cfg.dim, &L->ffn_norm);
        if (rc) goto fail;
        snprintf(name, sizeof name, "router.%u", l);
        rc = moe_get_tensor(path, m, name,
                            (uint64_t)m->cfg.experts_per_layer * m->cfg.dim, &L->router);
        if (rc) goto fail;
        snprintf(name, sizeof name, "ffn_gate.%u", l);
        rc = moe_get_tensor(path, m, name,
                            (uint64_t)m->cfg.inter * m->cfg.dim, &L->ffn_gate);
        if (rc) goto fail;
        snprintf(name, sizeof name, "ffn_up.%u", l);
        rc = moe_get_tensor(path, m, name,
                            (uint64_t)m->cfg.inter * m->cfg.dim, &L->ffn_up);
        if (rc) goto fail;
        snprintf(name, sizeof name, "ffn_down.%u", l);
        rc = moe_get_tensor(path, m, name,
                            (uint64_t)m->cfg.dim * m->cfg.inter, &L->ffn_down);
        if (rc) goto fail;
    }

    /* --- expert geometry (8) -------------------------------------- */
    if (m->cfg.inter > (UINT64_MAX / 3u) / m->cfg.dim) {
        rc = moe_refuse(path, "expert", "expert element count overflows",
                        -8);
        goto fail;
    }
    m->expert_f32 = (uint64_t)3 * m->cfg.inter * m->cfg.dim;
    if (m->cfg.expert_layout == 0) {
        m->expert_bytes = (size_t)m->expert_f32 * sizeof(float);
        if (m->expert_bytes != (size_t)m->expert_f32 * sizeof(float)) {
            rc = moe_refuse(path, "expert", "expert size overflows", -8);
            goto fail;
        }
    } else if (m->cfg.expert_layout == 2) {
        /* Q4_0 experts (round 16): each sub-matrix is a flat Q4_0
         * payload (18 bytes per 32 values), so rows*cols must be a
         * multiple of 32; the blob is [gate][up][down] of equal length
         * 3 * inter * dim elements -> 3 * (inter*dim*18/32) bytes. */
        const uint64_t nsub = (uint64_t)m->cfg.inter * m->cfg.dim;
        if (nsub % 32u) {
            char d[160];
            snprintf(d, sizeof d,
                     "Q4_0 expert sub-matrix has rows*cols %llu, not a "
                     "multiple of 32 (the Q4_0 block holds 32 values)",
                     (unsigned long long)nsub);
            rc = moe_refuse(path, "expert", d, -8);
            goto fail;
        }
        const uint64_t sub = nsub * 18u / 32u;
        if (sub > SIZE_MAX / 3u) {
            rc = moe_refuse(path, "expert", "Q4_0 expert size overflows",
                            -8);
            goto fail;
        }
        m->expert_bytes = (size_t)(3u * sub);
    } else {
        /* Ternary + scales (layout 1): the group comes from the
         * expert directory type (0x0040 -> 64, 0x0041 -> 128); every
         * entry must carry that same type. Per sub-matrix the blob
         * holds n/2 payload bytes + rows*ceil(cols/group) F32 scales
         * (the TERNARY2 payload needs an even element count). */
        const uint32_t first_type = (m->container.expert_dir_count > 0)
            ? m->container.experts[0].type_id : 0;
        const uint32_t group = (first_type == HYDRA2_TERNARY2_S64) ? 64u
            : (first_type == HYDRA2_TERNARY2_S128) ? 128u : 0u;
        if (!group) {
            char d[190];
            snprintf(d, sizeof d,
                     "ternary expert layout (v2.expert_layout 1) needs "
                     "directory type TERNARY2_S64 (0x0040) or "
                     "TERNARY2_S128 (0x0041), found 0x%04X", first_type);
            rc = moe_refuse(path, "expert", d, -8);
            goto fail;
        }
        m->cfg.ternary_group = group;
        size_t total = 0;
        const uint32_t sub_rows[3] = { m->cfg.inter, m->cfg.inter,
                                       m->cfg.dim };
        const uint32_t sub_cols[3] = { m->cfg.dim, m->cfg.dim,
                                       m->cfg.inter };
        for (int s = 0; s < 3; s++) {
            const uint64_t rows = sub_rows[s], cols = sub_cols[s];
            const uint64_t n = rows * cols; /* < 2^64: both < 2^32 */
            const uint64_t ngroups = (cols + group - 1) / group;
            if (n % 2u) {
                char d[130];
                snprintf(d, sizeof d,
                         "ternary expert sub-matrix %d has odd rows*cols "
                         "(%llu) - the TERNARY2 payload needs an even "
                         "element count",
                         s, (unsigned long long)n);
                rc = moe_refuse(path, "expert", d, -8);
                goto fail;
            }
            if (n > UINT64_MAX / 4u ||
                n / 2u > UINT64_MAX - ngroups * rows * 4u) {
                rc = moe_refuse(path, "expert",
                                "ternary expert size overflows", -8);
                goto fail;
            }
            const size_t term = (size_t)(n / 2u + ngroups * rows * 4u);
            if (total > SIZE_MAX - term) {
                rc = moe_refuse(path, "expert",
                                "ternary expert size overflows", -8);
                goto fail;
            }
            total += term;
        }
        m->expert_bytes = total;
    }
    /* every expert directory entry must carry exactly this length
     * and type */
    for (uint64_t i = 0; i < m->container.expert_dir_count; i++) {
        const hydra2_expert_entry_t *e = &m->container.experts[i];
        const uint32_t want_type = (m->cfg.expert_layout == 0)
            ? HYDRA2_F32
            : (m->cfg.expert_layout == 2) ? HYDRA2_Q4_0
            : m->container.experts[0].type_id;
        if (e->length != m->expert_bytes || e->type_id != want_type) {
            char d[250];
            snprintf(d, sizeof d,
                     "expert %llu (layer %u, id %u) has length %llu / "
                     "type 0x%04X, need %zu bytes type 0x%04X",
                     (unsigned long long)i, (unsigned)e->layer,
                     (unsigned)e->expert_id, (unsigned long long)e->length,
                     e->type_id, m->expert_bytes, want_type);
            rc = moe_refuse(path, "expert", d, -8);
            goto fail;
        }
    }
    if (m->container.expert_dir_count !=
        (uint64_t)m->cfg.layers * m->cfg.experts_per_layer) {
        char d[190];
        snprintf(d, sizeof d, "expert directory has %llu entries, need %llu",
                 (unsigned long long)m->container.expert_dir_count,
                 (unsigned long long)((uint64_t)m->cfg.layers * m->cfg.experts_per_layer));
        rc = moe_refuse(path, "expert", d, -8);
        goto fail;
    }
    m->expert_scratch = (uint8_t *)malloc(m->expert_bytes);
    if (!m->expert_scratch) {
        rc = moe_refuse(path, "memory", "out of memory", -9);
        goto fail;
    }
    if (m->cfg.ternary_group) {
        if (m->expert_f32 > SIZE_MAX / sizeof(float)) {
            rc = moe_refuse(path, "expert",
                            "dequant buffer size overflows", -8);
            goto fail;
        }
        m->expert_f32buf = (float *)malloc(m->expert_f32 * sizeof(float));
        if (!m->expert_f32buf) {
            rc = moe_refuse(path, "memory", "out of memory", -9);
            goto fail;
        }
    }
    /* Q4_0 (round 16): the row dequant scratch is sized for the WIDEST
     * mat-vec column count (dim for q/k/v/o/gate/up/embed/lm_head,
     * inter for ffn_down). It is needed when any trunk tensor is Q4_0
     * or the experts use layout 2. Shape rule (loud, at open): a Q4_0
     * row must hold a whole number of 32-value blocks, so dim AND
     * inter must be multiples of 32 (over-strict on purpose - a mixed
     * file with one odd row is a confusing file). */
    {
        int any_q4 = (m->cfg.expert_layout == 2);
        if (m->embed.type_id == HYDRA2_Q4_0 ||
            (m->has_lm_head && m->lm_head.type_id == HYDRA2_Q4_0))
            any_q4 = 1;
        for (uint32_t l = 0; l < m->cfg.layers && !any_q4; l++) {
            const moe_layer_t *L = &m->layers[l];
            if (L->attn_q.type_id == HYDRA2_Q4_0 ||
                L->attn_k.type_id == HYDRA2_Q4_0 ||
                L->attn_v.type_id == HYDRA2_Q4_0 ||
                L->attn_o.type_id == HYDRA2_Q4_0 ||
                L->ffn_gate.type_id == HYDRA2_Q4_0 ||
                L->ffn_up.type_id == HYDRA2_Q4_0 ||
                L->ffn_down.type_id == HYDRA2_Q4_0)
                any_q4 = 1;
        }
        if (any_q4) {
            if (m->cfg.dim % 32u || m->cfg.inter % 32u) {
                char d[160];
                snprintf(d, sizeof d,
                         "file uses Q4_0 tensors but dim %u / inter %u "
                         "are not multiples of 32 (a Q4_0 row must hold "
                         "whole 32-value blocks)",
                         m->cfg.dim, m->cfg.inter);
                rc = moe_refuse(path, "shape", d, -7);
                goto fail;
            }
            const uint32_t widest = (m->cfg.inter > m->cfg.dim)
                ? m->cfg.inter : m->cfg.dim;
            /* widest <= UINT32_MAX; * sizeof(float) cannot overflow
             * size_t (4 * 2^32 < 2^64). */
            m->q4_scratch = (float *)malloc((size_t)widest *
                                            sizeof(float));
            if (!m->q4_scratch) {
                rc = moe_refuse(path, "memory", "out of memory", -9);
                goto fail;
            }
        }
    }
    /* --- vocabulary + merges (role 8, FORMAT-V2.md 3.2.3) -------- */
    {
        const hydra2_tensor_entry_t *tv = NULL;
        const hydra2_tensor_entry_t *tm = NULL;
        /* quiet lookup: an ABSENT vocab/merges tensor is legal (the
         * MoE engine runs inference-only files), so the container's
         * loud find-refusal must not fire for the optional pair */
        tv = moe_find_quiet(m, "vocab");
        tm = moe_find_quiet(m, "merges");
        if (tv) {
            if (tv->type_id != HYDRA2_VOCAB_UTF8) {
                char d[160];
                snprintf(d, sizeof d,
                         "tensor 'vocab' must be VOCAB_UTF8 (0x0050), "
                         "got type 0x%04X", tv->type_id);
                rc = moe_refuse(path, "tensor", d, -6);
                goto fail;
            }
            rc = moe_load_vocab(path, m, tv);
            if (rc) goto fail;
        }
        if (tm) {
            if (!m->has_vocab) {
                rc = moe_refuse(path, "tensor",
                                "tensor 'merges' without a 'vocab' "
                                "tensor", -6);
                goto fail;
            }
            if (tm->type_id != HYDRA2_U16_PAIRS) {
                char d[160];
                snprintf(d, sizeof d,
                         "tensor 'merges' must be U16_PAIRS (0x0051), "
                         "got type 0x%04X", tm->type_id);
                rc = moe_refuse(path, "tensor", d, -6);
                goto fail;
            }
            rc = moe_load_merges(path, m, tm);
            if (rc) goto fail;
        }
    }
    return 0;

fail:
    hydra_moe_close(m);
    return rc;
}

void hydra_moe_close(hydra_moe_t *m)
{
    if (!m) return;
    moe_free_tensor(&m->embed);
    moe_free_tensor(&m->final_norm);
    moe_free_tensor(&m->lm_head);
    if (m->layers) {
        for (uint32_t l = 0; l < m->cfg.layers; l++) {
            moe_layer_t *L = &m->layers[l];
            moe_free_tensor(&L->attn_q);
            moe_free_tensor(&L->attn_k);
            moe_free_tensor(&L->attn_v);
            moe_free_tensor(&L->attn_o);
            moe_free_tensor(&L->attn_norm);
            moe_free_tensor(&L->ffn_norm);
            moe_free_tensor(&L->router);
            moe_free_tensor(&L->ffn_gate);
            moe_free_tensor(&L->ffn_up);
            moe_free_tensor(&L->ffn_down);
        }
        free(m->layers);
        m->layers = NULL;
    }
    free(m->expert_scratch);
    m->expert_scratch = NULL;
    free(m->expert_f32buf);
    m->expert_f32buf = NULL;
    free(m->q4_scratch);
    m->q4_scratch = NULL;
    free(m->vocab_blob);
    m->vocab_blob = NULL;
    free(m->vocab_off);
    m->vocab_off = NULL;
    free(m->vocab_len);
    m->vocab_len = NULL;
    free(m->merges);
    m->merges = NULL;
    if (m->container_open) {
        hydra2_close(&m->container);
        m->container_open = 0;
    }
    memset(m, 0, sizeof *m);
}

int hydra_moe_config(const hydra_moe_t *m, hydra_moe_config_t *out)
{
    if (!m || !out || !m->container_open) return -1;
    *out = m->cfg;
    return 0;
}

/* Flat tensor dequant for the PAIRED scale convention (FORMAT-V2.md
 * 3.2.1): `n` TERNARY2 elements with `n / group` F32 scales in
 * element-group order. Materialized value = code * scale (exact F32);
 * the engine then runs the identical F32 path (the same materialize-
 * and-reuse decision as the layout-1 experts). */
/* Public (test-visible): the flat tensor dequant that moe_get_tensor
 * runs for TERNARY2 trunk tensors. payload must NOT alias out - the
 * caller materializes the packed bytes in a separate buffer first
 * (round-13, Qodo #3; see moe_get_tensor). */
int hydra_moe_dequant_flat(const uint8_t *payload, const float *scales,
                           uint64_t n, uint32_t group, float *out)
{
    if (!payload || !scales || !out || !group) return -1;
    if (n % 2u) return -1;
    const uint64_t nscale = (n + group - 1u) / group; /* partial last group */
    for (uint64_t g = 0; g < nscale; g++) {
        const float scale = scales[g];
        const uint64_t start = g * group;
        const uint64_t end = start + group < n ? start + group : n;
        for (uint64_t k = start; k < end; k++) {
            const uint8_t code = (k & 1u)
                ? (uint8_t)((payload[k / 2u] >> 2) & 0x3u)
                : (uint8_t)(payload[k / 2u] & 0x3u);
            const int t = (code == HYDRA_TERNARY_POS) ? 1
                : (code == HYDRA_TERNARY_NEG) ? -1 : 0;
            out[k] = (t == 0) ? 0.0f : (t < 0) ? -scale : scale;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* ternary expert dequantization (layout 1)                           */
/* ------------------------------------------------------------------ */

/* One (rows x cols) TERNARY2 + scale block. The payload is the exact
 * TERNARY2 packing (n/2 bytes; element 2j in the LOW 2 bits of byte
 * j, 2j+1 in bits 2-3; codes 00=0, 01=+1, 10=-1, 11=0); the scales
 * follow in row-major [row][group] order, one per group of `group`
 * consecutive columns (the last group may be partial). Dequantized
 * value = code * scale, exact in F32 (multiply by +/-1 or 0). */
static int moe_dequant_block(const uint8_t **pp, float *out,
                             uint32_t rows, uint32_t cols, uint32_t group)
{
    const uint8_t *p = *pp;
    const uint64_t n = (uint64_t)rows * cols;
    if (n % 2u) return -1;
    const uint64_t ngroups = ((uint64_t)cols + group - 1) / group;
    const uint8_t *codes = p;
    /* The scales follow the packed code bytes directly, so when n/2 is
     * odd the scale section starts at a 2-byte, not a 4-byte, aligned
     * offset (rows*cols % 4 == 2). Reading it through a float pointer is
     * misaligned access - legal on x86, a trap hazard on ARM. The scale
     * value is identical through memcpy, which is the portable read.
     * (Found 2026-10-30 by the ASan+UBSan gate; the dequant unit test's
     * inter=2 x dim=3 blob is exactly the n % 4 == 2 case.) */
    const uint8_t *scales = p + n / 2u;
    *pp = p + n / 2u + ngroups * rows * 4u;
    for (uint64_t r = 0; r < rows; r++) {
        for (uint64_t g = 0; g < ngroups; g++) {
            float scale;
            memcpy(&scale, scales + (r * ngroups + g) * 4u, sizeof scale);
            const uint64_t c0 = g * group;
            const uint64_t c1 = (c0 + group < cols) ? c0 + group : cols;
            for (uint64_t i = c0; i < c1; i++) {
                const uint64_t idx = r * cols + i;
                const uint8_t code = (idx & 1u)
                    ? (uint8_t)((codes[idx / 2u] >> 2) & 0x3u)
                    : (uint8_t)(codes[idx / 2u] & 0x3u);
                const int t = (code == HYDRA_TERNARY_POS) ? 1
                    : (code == HYDRA_TERNARY_NEG) ? -1 : 0;
                out[idx] = (t == 0) ? 0.0f : (t < 0) ? -scale : scale;
            }
        }
    }
    return 0;
}

int hydra_moe_dequant_expert(const uint8_t *blob, float *out,
                             uint32_t inter, uint32_t dim, uint32_t group)
{
    if (!blob || !out || !group || !inter || !dim) return -1;
    const uint32_t rows[3] = { inter, inter, dim };
    const uint32_t cols[3] = { dim, dim, inter };
    const uint8_t *p = blob;
    for (int s = 0; s < 3; s++) {
        if (moe_dequant_block(&p, out, rows[s], cols[s], group) != 0)
            return -1;
        out += (size_t)rows[s] * cols[s];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* forward pass                                                       */
/* ------------------------------------------------------------------ */

/* Plain f32 mat-vec: y[i] = sum_j W[i*cols + j] * x[j], source order. */
static void moe_matvec(const float *W, const float *x, float *y,
                       uint32_t rows, uint32_t cols)
{
    for (uint32_t i = 0; i < rows; i++) {
        float acc = 0.0f;
        const float *row = W + (size_t)i * cols;
        for (uint32_t j = 0; j < cols; j++)
            acc += row[j] * x[j];
        y[i] = acc;
    }
}

/* ------------------------------------------------------------------ */
/* Q4_0 (round 16, FORMAT-V2.md 3.2): 18-byte blocks of 32 values,    */
/* F16 scale d at block offset 0, then 16 bytes carrying two 4-bit    */
/* codes each (value 2j in the LOW nibble of byte j, 2j+1 in the      */
/* HIGH nibble), w[i] = d * (q[i] - 8) - the exact ggml block_q4_0    */
/* layout (the GGUF reader's Q4_0 is gated bit-exact against an       */
/* independent decode in tools/gguf_test.py; this C decode must stay  */
/* value-identical to tools/gguf_reader.py's _decode_q4_0).           */
/* ------------------------------------------------------------------ */

/* IEEE binary16 -> binary32, exact (F16 is a subset of F32). */
static float f16_to_f32(uint16_t h)
{
    uint32_t sign = ((uint32_t)(h >> 15) & 1u) << 31;
    int e = (int)((h >> 10) & 0x1Fu);
    uint32_t mant = (uint32_t)(h & 0x3FFu);
    uint32_t bits;
    if (e == 0) {
        if (mant == 0u) {
            bits = sign; /* +-0 */
        } else {
            /* subnormal: normalize to 1.mant form */
            while (!(mant & 0x400u)) { mant <<= 1u; e--; }
            mant &= 0x3FFu;
            bits = sign | ((uint32_t)(e + 112) << 23) | (mant << 13);
        }
    } else if (e == 31) {
        bits = sign | 0x7F800000u | (mant << 13); /* Inf / NaN */
    } else {
        bits = sign | ((uint32_t)(e + 112) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

/* One row of cols elements (cols % 32 == 0) from the block layout
 * into out. The per-block F16 scale is decoded once per block; the
 * product d*(q-8) is a single F32 multiply (q-8 exact in F32), so the
 * value matches the Python reference decode bit-for-bit. */
static void q40_row_dequant(const uint8_t *row, float *out, uint32_t cols)
{
    const uint32_t nblocks = cols / 32u;
    for (uint32_t b = 0; b < nblocks; b++) {
        const uint8_t *blk = row + (size_t)b * 18u;
        uint16_t dh = (uint16_t)(blk[0] | ((uint16_t)blk[1] << 8));
        const float d = f16_to_f32(dh);
        const uint8_t *nb = blk + 2;
        for (uint32_t j = 0; j < 16u; j++) {
            uint8_t byte = nb[j];
            out[b * 32u + 2u * j] = d * ((float)(byte & 0x0Fu) - 8.0f);
            out[b * 32u + 2u * j + 1u] = d * ((float)(byte >> 4) - 8.0f);
        }
    }
}

/* Public (test-visible): flat Q4_0 dequant, see the header contract
 * (n % 32 == 0, no aliasing between payload and out). */
int hydra_moe_dequant_q40(const uint8_t *payload, uint64_t n, float *out)
{
    if (!payload || !out || (n % 32u) || n == 0) return -1;
    for (uint64_t off = 0; off < n; off += 32u)
        q40_row_dequant(payload + off * 18u / 32u, out + off, 32u);
    return 0;
}

/* Q4_0 mat-vec with the SAME accumulation order as moe_matvec
 * (y[i] = sum_j Wq[i,j] * x[j]): one row is dequantized into the
 * caller's scratch (cols floats) and the row dot runs in F32 source
 * order. The resident bytes stay 4-bit (18 per 32 values); the only
 * extra state is the one-row scratch. */
static void moe_matvec_q40(const uint8_t *Wq, const float *x, float *y,
                           uint32_t rows, uint32_t cols, float *scratch)
{
    const size_t row_bytes = (size_t)cols * 18u / 32u;
    for (uint32_t i = 0; i < rows; i++) {
        float acc = 0.0f;
        q40_row_dequant(Wq + (size_t)i * row_bytes, scratch, cols);
        for (uint32_t j = 0; j < cols; j++)
            acc += scratch[j] * x[j];
        y[i] = acc;
    }
}

/* Dispatch: F32 tensors take the plain kernel, Q4_0 tensors the
 * in-kernel dequant path (the caller passes the shared row scratch). */
static void moe_mv(const moe_tensor_t *t, const float *x, float *y,
                   uint32_t rows, uint32_t cols, float *q4_scratch)
{
    if (t->type_id == HYDRA2_Q4_0)
        moe_matvec_q40((const uint8_t *)t->data, x, y, rows, cols,
                       q4_scratch);
    else
        moe_matvec(t->data, x, y, rows, cols);
}

/* Top-k selection with the container tie rule (tie -> lower id).
 * Writes the selected expert ids in DESCENDING logit order into sel. */
static void moe_top_k(float *logits, uint32_t n, uint32_t k,
                      uint32_t *sel)
{
    uint32_t done = 0;
    while (done < k) {
        int32_t best = -1;
        float bestv = -INFINITY;
        for (uint32_t j = 0; j < n; j++) {
            if (best < 0 || logits[j] > bestv) {
                bestv = logits[j];
                best = (int32_t)j;
            }
        }
        sel[done++] = (uint32_t)best;
        logits[(size_t)best] = -INFINITY; /* remove */
    }
}

int hydra_moe_forward(hydra_moe_t *m, uint32_t token_in,
                      uint32_t pos, float *logits_out,
                      hydra_kv_cache_t *kv_per_layer)
{
    if (!m || !m->container_open || !logits_out) return -1;
    const hydra_moe_config_t *c = &m->cfg;
    if (token_in >= c->vocab_size) return -1;
    const uint32_t dim = c->dim, hd = c->head_dim;
    const uint32_t nh = c->n_heads, nkv = c->n_kv_heads;
    const uint32_t inter = c->inter;
    if (kv_per_layer && c->layers > 0) {
        for (uint32_t l = 0; l < c->layers; l++) {
            if (!kv_per_layer[l].k || !kv_per_layer[l].v) return -1;
            /* Round 13 (Qodo #28): the cache geometry must match the
             * model's KV heads - with a mismatched cache dim the
             * head-strided reads (kbuf + kh * hd) wander outside the
             * slots the caller actually allocated. */
            if (kv_per_layer[l].dim != nkv * hd) return -1;
        }
    }

    /* buffers (dim <= 4096 is the rmsnorm bound; inter <= 65536 here is
     * not checked against that bound - the swiglu call uses inter, so
     * cap it at the same 4096 through the shape check? No: inter may
     * exceed 4096 in real models; hydra_swiglu's bound is 4096. The
     * gate/up mix therefore uses a LOCAL swiglu loop for inter > 4096.
     * The fixture uses small inter; both paths are tested. */
    float *h     = (float *)malloc((size_t)dim * sizeof(float));
    float *a     = (float *)malloc((size_t)dim * sizeof(float));
    float *q     = (float *)malloc((size_t)dim * sizeof(float));
    float *k     = (float *)malloc((size_t)(nkv * hd) * sizeof(float));
    float *v     = (float *)malloc((size_t)(nkv * hd) * sizeof(float));
    float *co    = (float *)malloc((size_t)dim * sizeof(float));
    float *b     = (float *)malloc((size_t)dim * sizeof(float));
    float *gl    = (float *)malloc((size_t)inter * sizeof(float));
    float *ul    = (float *)malloc((size_t)inter * sizeof(float));
    float *hh    = (float *)malloc((size_t)inter * sizeof(float));
    float *rlog  = (float *)malloc((size_t)c->experts_per_layer * sizeof(float));
    float *exp   = (float *)malloc((size_t)dim * sizeof(float));
    float *outf  = (float *)malloc((size_t)dim * sizeof(float));
    float *kbuf  = (float *)malloc((size_t)(nkv * hd) * sizeof(float));
    float *vbuf  = (float *)malloc((size_t)(nkv * hd) * sizeof(float));
    if (!h || !a || !q || !k || !v || !co || !b || !gl || !ul || !hh ||
        !rlog || !exp || !outf || !kbuf || !vbuf) {
        free(h); free(a); free(q); free(k); free(v); free(co); free(b);
        free(gl); free(ul); free(hh); free(rlog); free(exp); free(outf);
        free(kbuf); free(vbuf);
        return -1;
    }
    int rc = 0;

    /* h = embed[token] */
    if (m->embed.type_id == HYDRA2_Q4_0) {
        /* one row of the block layout (dim % 32 == 0, checked at open) */
        q40_row_dequant((const uint8_t *)m->embed.data +
                        (size_t)token_in * dim * 18u / 32u, h, dim);
    } else {
        memcpy(h, m->embed.data + (size_t)token_in * dim,
               dim * sizeof(float));
    }

    for (uint32_t l = 0; l < c->layers; l++) {
        const moe_layer_t *L = &m->layers[l];

        /* --- attention (pre-norm, GQA) --- */
        if (hydra_rmsnorm(h, L->attn_norm.data, dim, 1e-6f, a) != 0) {
            rc = -16; goto done;
        }
        moe_mv(&L->attn_q, a, q, dim, dim, m->q4_scratch);
        moe_mv(&L->attn_k, a, k, nkv * hd, dim, m->q4_scratch);
        moe_mv(&L->attn_v, a, v, nkv * hd, dim, m->q4_scratch);

        /* RoPE per head */
        for (uint32_t i = 0; i < nh; i++)
            if (hydra_rope_inplace(q + i * hd, hd, pos, c->rope_theta) != 0) {
                rc = -16; goto done;
            }
        for (uint32_t i = 0; i < nkv; i++)
            if (hydra_rope_inplace(k + i * hd, hd, pos, c->rope_theta) != 0) {
                rc = -16; goto done;
            }

        /* attention over resident entries + current token */
        const float scale = 1.0f / sqrtf((float)hd);
        uint32_t n_res = kv_per_layer ? hydra_kv_cache_size(&kv_per_layer[l]) : 0u;
        const uint32_t gqa = nh / nkv; /* kv heads per group */
        for (uint32_t i = 0; i < nh; i++) {
            /* BLOCK mapping (llama.cpp convention): contiguous query
             * heads share one kv head - head i reads kv head
             * i / (nh/nkv). The old i % nkv INTERLEAVED the heads,
             * which no reference implementation does and which gives
             * wrong attention for any n_kv_heads > 1 (round 13,
             * Qodo #26; identical for n_kv_heads == 1, so all pinned
             * fixtures are unaffected). */
            const uint32_t kh = i / gqa;
            const float *qh = q + i * hd;
            const float *kh_base = k + kh * hd;
            uint32_t n = n_res + 1;
            float *sc = (float *)malloc((size_t)n * sizeof(float));
            if (!sc) { rc = -1; goto done; }
            for (uint32_t t = 0; t < n; t++) {
                const float *kt;
                if (t < n_res) {
                    if (hydra_kv_cache_get(&kv_per_layer[l], t, kbuf, vbuf) != 0) {
                        free(sc); rc = -18; goto done;
                    }
                    kt = kbuf + kh * hd;
                } else {
                    kt = kh_base;
                }
                float s = 0.0f;
                for (uint32_t d2 = 0; d2 < hd; d2++)
                    s += qh[d2] * kt[d2];
                sc[t] = s * scale;
            }
            /* softmax */
            float mx = -INFINITY;
            for (uint32_t t = 0; t < n; t++)
                if (sc[t] > mx) mx = sc[t];
            float sum = 0.0f;
            for (uint32_t t = 0; t < n; t++) {
                sc[t] = expf(sc[t] - mx);
                sum += sc[t];
            }
            for (uint32_t t = 0; t < n; t++) sc[t] /= sum;
            /* accumulate value */
            float *ch = co + i * hd;
            memset(ch, 0, hd * sizeof(float));
            for (uint32_t t = 0; t < n; t++) {
                const float *vt;
                if (t < n_res) {
                    /* per-token fetch: the score loop's get() does not
                     * persist for this loop (it advances with t there) */
                    if (hydra_kv_cache_get(&kv_per_layer[l], t, kbuf, vbuf) != 0) {
                        free(sc); rc = -18; goto done;
                    }
                    vt = vbuf + kh * hd;
                } else {
                    vt = v + kh * hd;
                }
                float w = sc[t];
                for (uint32_t d2 = 0; d2 < hd; d2++)
                    ch[d2] += w * vt[d2];
            }
            free(sc);
        }

        /* residual: h += attn_o * co */
        moe_mv(&L->attn_o, co, exp, dim, dim, m->q4_scratch);
        for (uint32_t i = 0; i < dim; i++) h[i] += exp[i];

        /* append the CURRENT token's k/v (after attention, so the
         * token does not attend to its own cached entry twice). */
        if (kv_per_layer) {
            if (hydra_kv_cache_append(&kv_per_layer[l], k, v) != 0) {
                rc = -17; goto done;
            }
        }

        /* --- MoE FFN (pre-norm) --- */
        if (hydra_rmsnorm(h, L->ffn_norm.data, dim, 1e-6f, b) != 0) {
            rc = -16; goto done;
        }
        moe_mv(&L->router, b, rlog, c->experts_per_layer, dim, m->q4_scratch);
        for (uint32_t i = 0; i < c->experts_per_layer; i++) {
            if (MOE_NAN_CHECK(rlog[i])) { rc = -16; goto done; }
        }
        uint32_t sel[16];
        if (c->top_k > 16) { rc = -1; goto done; }
        float wts[16];
        /* top-k on a scratch copy (the selection removes picks by
         * writing -INFINITY into the working logits) */
        float rk[64];
        if (c->experts_per_layer > 64) { rc = -1; goto done; }
        memcpy(rk, rlog, (size_t)c->experts_per_layer * sizeof(float));
        moe_top_k(rk, c->experts_per_layer, c->top_k, sel);
        /* softmax over the selected only. The weights are computed
         * from the ORIGINAL router logits (rlog), not from the
         * scratch: moe_top_k removes its picks by writing -INFINITY
         * into rk, so rk[sel[i]] is destroyed by construction. */
        float mx = -INFINITY;
        for (uint32_t i = 0; i < c->top_k; i++)
            if (rlog[sel[i]] > mx) mx = rlog[sel[i]];
        float sum = 0.0f;
        for (uint32_t i = 0; i < c->top_k; i++) {
            wts[i] = expf(rlog[sel[i]] - mx);
            sum += wts[i];
        }
        for (uint32_t i = 0; i < c->top_k; i++) wts[i] /= sum;

        memset(exp, 0, dim * sizeof(float));
        for (uint32_t i = 0; i < c->top_k; i++) {
            const uint32_t eid = sel[i];
            const hydra2_expert_entry_t *ee;
            if (hydra2_find_expert(&m->container, l, eid, &ee) != HYDRA2_OK) {
                rc = -2; goto done;
            }
            if (hydra2_read_expert(&m->container, ee,
                                   m->expert_scratch, ee->length) != HYDRA2_OK) {
                rc = -2; goto done;
            }
            const float *gate = NULL, *up = NULL, *down = NULL;
            size_t sub = 0;
            if (m->cfg.ternary_group) {
                /* dequantize, then run the IDENTICAL F32 expert path */
                if (hydra_moe_dequant_expert(m->expert_scratch,
                                             m->expert_f32buf,
                                             c->inter, c->dim,
                                             m->cfg.ternary_group) != 0) {
                    rc = -2; goto done;
                }
                gate = m->expert_f32buf;
                up   = m->expert_f32buf + (size_t)inter * dim;
                down = m->expert_f32buf + (size_t)2 * inter * dim;
                moe_matvec(gate, b, gl, inter, dim);
                moe_matvec(up, b, ul, inter, dim);
            } else if (m->cfg.expert_layout == 2) {
                /* Q4_0 expert (round 16): the blob is the raw
                 * [gate][up][down] block payloads (inter*dim elements
                 * each, 18 bytes per 32 values); the in-kernel dequant
                 * keeps the resident bytes 4-bit. */
                sub = (size_t)inter * dim * 18u / 32u;
                moe_matvec_q40(m->expert_scratch, b, gl,
                               inter, dim, m->q4_scratch);
                moe_matvec_q40(m->expert_scratch + sub, b, ul,
                               inter, dim, m->q4_scratch);
            } else {
                /* F32 expert: the scratch buffer holds the expert's raw
                 * float32 bytes (pread'd above). The buffer is a malloc
                 * allocation (4-byte aligned) and the payload is F32 by
                 * format contract, so the float view is well defined. */
                const float *ef =
                    (const float *)m->expert_scratch; // cppcheck-suppress invalidPointerCast
                gate = ef;
                up   = ef + (size_t)inter * dim;
                down = ef + (size_t)2 * inter * dim;
                moe_matvec(gate, b, gl, inter, dim);
                moe_matvec(up, b, ul, inter, dim);
            }
            /* swiglu (local loop: no 4096 cap) */
            for (uint32_t j = 0; j < inter; j++) {
                float g = gl[j];
                hh[j] = (g / (1.0f + expf(-g))) * ul[j];
                if (MOE_NAN_CHECK(hh[j])) { rc = -16; goto done; }
            }
            float *eout = (float *)malloc((size_t)dim * sizeof(float));
            if (!eout) { rc = -1; goto done; }
            if (m->cfg.expert_layout == 2)
                moe_matvec_q40(m->expert_scratch + 2 * sub, hh, eout,
                               dim, inter, m->q4_scratch);
            else
                moe_matvec(down, hh, eout, dim, inter);
            float w = wts[i];
            for (uint32_t j = 0; j < dim; j++)
                exp[j] += w * eout[j];
            free(eout);
        }
        for (uint32_t i = 0; i < dim; i++) h[i] += exp[i];
    }

    /* --- logits --- */
    if (hydra_rmsnorm(h, m->final_norm.data, dim, 1e-6f, outf) != 0) {
        rc = -16; goto done;
    }
    const moe_tensor_t *Wt = m->has_lm_head ? &m->lm_head : &m->embed;
    moe_mv(Wt, outf, logits_out, c->vocab_size, dim, m->q4_scratch);
    for (uint32_t t = 0; t < c->vocab_size; t++) {
        if (MOE_NAN_CHECK(logits_out[t])) { rc = -16; goto done; }
    }

done:
    free(h); free(a); free(q); free(k); free(v); free(co); free(b);
    free(gl); free(ul); free(hh); free(rlog); free(exp); free(outf);
    free(kbuf); free(vbuf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* sampler (REAL logit vector)                                        */
/* ------------------------------------------------------------------ */

/* xorshift64* - the same RNG the v1 sampler uses, so seeded runs are
 * reproducible with the documented seed. */
static uint64_t moe_xorshift64s(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

int hydra_moe_sample(const float *logits, uint32_t vocab,
                     uint32_t *token_out,
                     double temperature, double min_p,
                     double rep_penalty, uint32_t window,
                     const uint32_t *recent_tokens,
                     uint64_t *rng_state)
{
    if (!logits || !token_out || vocab == 0) return -1;
    if (temperature < -1.0 || min_p < 0.0 || min_p > 1.0 ||
        rep_penalty < 0.5 || rep_penalty > 2.0)
        return -1;

    /* penalize repeats of the last `window` tokens (sign-preserving).
     * recent_tokens must hold at least `window` entries; each
     * occurrence in the window compounds the penalty (round 13,
     * Qodo #21: the old code capped the walk at `vocab`, silently
     * dropping tokens from a window longer than the vocabulary). */
    float *adj = (float *)malloc((size_t)vocab * sizeof(float));
    if (!adj) return -1;
    for (uint32_t t = 0; t < vocab; t++) {
        float v = logits[t];
        if (MOE_NAN_CHECK(v)) { free(adj); return -1; }
        if (recent_tokens) {
            for (uint32_t w = 0; w < window; w++) {
                if (recent_tokens[w] == t) {
                    v = (v > 0.0f) ? (float)(v / rep_penalty)
                                   : (float)(v * rep_penalty);
                }
            }
        }
        adj[t] = v;
    }

    /* greedy: argmax (ties -> lower id), no RNG */
    if (temperature <= 0.0 || min_p >= 1.0) {
        uint32_t best = 0;
        float bestv = adj[0];
        for (uint32_t t = 1; t < vocab; t++)
            if (adj[t] > bestv) { bestv = adj[t]; best = t; }
        *token_out = best;
        free(adj);
        return 0;
    }

    /* stable softmax with scale 1/temperature: the temperature must
     * scale the EXPONENT, not the exp result - dividing exp(v-mx) by
     * T and then renormalizing cancels T entirely (round-13, Qodo #8:
     * every temperature sampled the identical distribution). */
    const float T = (float)temperature;
    float mx = -INFINITY;
    for (uint32_t t = 0; t < vocab; t++)
        if (adj[t] > mx) mx = adj[t];
    float sum = 0.0f;
    for (uint32_t t = 0; t < vocab; t++) {
        adj[t] = expf((adj[t] - mx) / T);
        sum += adj[t];
    }
    for (uint32_t t = 0; t < vocab; t++) adj[t] /= sum;

    /* min-p filter */
    float pmax = 0.0f;
    for (uint32_t t = 0; t < vocab; t++)
        if (adj[t] > pmax) pmax = adj[t];
    const float floor = (float)min_p * pmax;
    float keep_sum = 0.0f;
    for (uint32_t t = 0; t < vocab; t++) {
        if (adj[t] >= floor) keep_sum += adj[t];
        else adj[t] = 0.0f;
    }
    if (keep_sum <= 0.0f) { /* degenerate: fall back to the max token */
        uint32_t best = 0;
        for (uint32_t t = 1; t < vocab; t++)
            if (logits[t] > logits[best]) best = t;
        *token_out = best;
        free(adj);
        return 0;
    }
    for (uint32_t t = 0; t < vocab; t++) adj[t] /= keep_sum;

    /* CDF walk with the shared xorshift64* RNG. The stochastic path
     * REQUIRES an RNG state (round-13, Qodo #19: a NULL state here
     * was an unchecked dereference; greedy never reaches this point,
     * so NULL stays legal for pure argmax callers). */
    if (!rng_state) { free(adj); return -1; }
    double r = (double)(moe_xorshift64s(rng_state) >> 11) *
               (1.0 / 9007199254740992.0); /* [0,1) 53-bit */
    float acc = 0.0f;
    for (uint32_t t = 0; t < vocab; t++) {
        acc += adj[t];
        if ((double)acc >= r) {
            *token_out = t;
            free(adj);
            return 0;
        }
    }
    *token_out = vocab - 1;
    free(adj);
    return 0;
}

/* ------------------------------------------------------------------ */
/* vocabulary + BPE text codec                                          */
/* ------------------------------------------------------------------ */

int hydra_moe_has_vocab(const hydra_moe_t *m)
{
    return (m && m->container_open) ? m->has_vocab : 0;
}

uint32_t hydra_moe_merge_count(const hydra_moe_t *m)
{
    return (m && m->container_open) ? m->merge_count : 0;
}

int hydra_moe_decode_token(const hydra_moe_t *m, uint32_t token,
                           char *buf, uint32_t cap)
{
    if (!m || !m->container_open || !m->has_vocab) return -1;
    if (!buf || token >= m->cfg.vocab_size) return -1;
    if (cap < m->vocab_len[token]) return -1;
    memcpy(buf, m->vocab_blob + m->vocab_off[token], m->vocab_len[token]);
    return (int)m->vocab_len[token];
}

int hydra_moe_encode_text(const hydra_moe_t *m, const char *text,
                          uint32_t *tokens_out, uint32_t max_tokens)
{
    if (!m || !text || !tokens_out || max_tokens == 0u) return -5;
    if (!m->container_open || !m->has_vocab) return -1;
    const uint8_t *t = (const uint8_t *)text;
    const uint32_t len = (uint32_t)strlen(text);
    {
        uint32_t uerr = 0;
        if (moe_utf8_first_error(t, len, &uerr) != 0) {
            fprintf(stderr,
                    "[MoE] text prompt: invalid UTF-8 at byte offset %u\n",
                    uerr);
            return -2;
        }
    }
    if (len > max_tokens) return -4;
    /* The working sequence never grows (merging only shrinks), so it
     * can be allocated at exactly max_tokens. */
    uint32_t *q = (uint32_t *)malloc((size_t)max_tokens * sizeof(uint32_t));
    uint32_t *q2 = (uint32_t *)malloc((size_t)max_tokens * sizeof(uint32_t));
    if (!q || !q2) {
        free(q); free(q2);
        return -5;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < len; i++) {
        const int32_t id = m->byte_map[t[i]];
        if (id < 0) {
            fprintf(stderr,
                    "[MoE] text prompt: byte 0x%02X at offset %u has no "
                    "single-byte token in the vocabulary\n", t[i], i);
            free(q); free(q2);
            return -3;
        }
        q[n++] = (uint32_t)id;
    }
    for (;;) {
        int merged_any = 0;
        for (uint32_t r = 0; r < m->merge_count; r++) {
            const uint32_t l = m->merges[r].left;
            const uint32_t rr = m->merges[r].right;
            const uint32_t target = m->merges[r].target;
            int found = 0;
            for (uint32_t i = 0; i + 1u < n; i++)
                if (q[i] == l && q[i + 1u] == rr) { found = 1; break; }
            if (!found) continue;
            uint32_t m2 = 0;
            uint32_t i = 0;
            while (i < n) {
                if (i + 1u < n && q[i] == l && q[i + 1u] == rr) {
                    q2[m2++] = target;
                    i += 2u;
                } else {
                    q2[m2++] = q[i];
                    i += 1u;
                }
            }
            n = m2;
            memcpy(q, q2, (size_t)n * sizeof(uint32_t));
            merged_any = 1;
            break; /* restart at the lowest-priority rule */
        }
        if (!merged_any) break;
    }
    memcpy(tokens_out, q, (size_t)n * sizeof(uint32_t));
    free(q); free(q2);
    return (int)n;
}

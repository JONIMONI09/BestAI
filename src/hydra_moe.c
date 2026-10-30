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
static int moe_dequant_flat(const uint8_t *payload, const float *scales,
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
        e->type_id != HYDRA2_TERNARY2_S128) {
        char d[190];
        snprintf(d, sizeof d, "tensor '%s' must be F32 or a block-scaled "
                 "ternary (0x0040/0x0041), got type 0x%04X ('%s')",
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
    /* The ternary payload is n/2 bytes on disk but materializes to n
     * F32 values: the buffer is sized for the DESTINATION width. */
    const uint64_t alloc = (e->type_id == HYDRA2_F32) ? e->n_bytes
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
    if (e->type_id != HYDRA2_F32) {
        /* Block-scaled ternary: materialize through the PAIRED
         * '<name>.scale' tensor (FORMAT-V2.md 3.2.1). */
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
        if (moe_dequant_flat((const uint8_t *)t->data, scales,
                             e->n_elements, group, t->data) != 0) {
            free(scales); free(t->data); t->data = NULL;
            return moe_refuse(path, "tensor",
                              "ternary dequantization failed", -6);
        }
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

/* UTF-8 validation (RFC 3629): returns 0 when the whole buffer is
 * valid, otherwise the offset of the first invalid byte. No
 * overlongs, no surrogates, no > U+10FFFF. */
static uint32_t moe_utf8_first_error(const uint8_t *p, uint32_t len)
{
    uint32_t i = 0;
    while (i < len) {
        const uint8_t b = p[i];
        uint32_t need;
        uint32_t cp;
        if (b < 0x80u) { i += 1; continue; }
        if (b < 0xC2u) return i;                 /* 0x80-0xC1 overlong */
        if (b < 0xE0u) { need = 1; cp = b & 0x1Fu; }
        else if (b < 0xF0u) { need = 2; cp = b & 0x0Fu; }
        else if (b < 0xF5u) { need = 3; cp = b & 0x07u; }
        else return i;                           /* > U+10FFFF */
        if (i + need >= len + 1 - 0 && i + need > len) return i;
        for (uint32_t k = 1; k <= need; k++) {
            if (p[i + k] < 0x80u || p[i + k] >= 0xC0u) return i;
            cp = (cp << 6) | (p[i + k] & 0x3Fu);
        }
        if (need == 1 && cp < 0x20u) return i;           /* overlong */
        if (need == 2 && cp < 0x800u) return i;          /* overlong */
        if (need == 3 && (cp < 0x10000u ||
                          (cp >= 0xD800u && cp <= 0xDFFFu) ||
                          cp > 0x10FFFFu)) return i;
        i += 1 + need;
    }
    return 0;
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
        if (moe_utf8_first_error(b + off, len) != 0) {
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

    /* --- shape relations (7) -------------------------------------- */
    if (m->cfg.dim == 0 || m->cfg.vocab_size == 0 || m->cfg.n_heads == 0 ||
        m->cfg.head_dim == 0 || m->cfg.inter == 0 || m->cfg.layers == 0 ||
        m->cfg.experts_per_layer == 0 || m->cfg.top_k == 0 ||
        m->cfg.top_k > m->cfg.experts_per_layer ||
        m->cfg.n_kv_heads < 1 || m->cfg.n_kv_heads > m->cfg.n_heads ||
        m->cfg.n_heads * m->cfg.head_dim != m->cfg.dim ||
        m->cfg.expert_layout > 1 ||
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
            ? HYDRA2_F32 : m->container.experts[0].type_id;
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
static int moe_dequant_flat(const uint8_t *payload, const float *scales,
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
    if (kv_per_layer && c->layers > 0) {
        for (uint32_t l = 0; l < c->layers; l++) {
            if (!kv_per_layer[l].k || !kv_per_layer[l].v) return -1;
        }
    }

    const uint32_t dim = c->dim, hd = c->head_dim;
    const uint32_t nh = c->n_heads, nkv = c->n_kv_heads;
    const uint32_t inter = c->inter;

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
    memcpy(h, m->embed.data + (size_t)token_in * dim, dim * sizeof(float));

    for (uint32_t l = 0; l < c->layers; l++) {
        const moe_layer_t *L = &m->layers[l];

        /* --- attention (pre-norm, GQA) --- */
        if (hydra_rmsnorm(h, L->attn_norm.data, dim, 1e-6f, a) != 0) {
            rc = -16; goto done;
        }
        moe_matvec(L->attn_q.data, a, q, dim, dim);
        moe_matvec(L->attn_k.data, a, k, nkv * hd, dim);
        moe_matvec(L->attn_v.data, a, v, nkv * hd, dim);

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
        for (uint32_t i = 0; i < nh; i++) {
            const uint32_t kh = i % nkv;
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
        moe_matvec(L->attn_o.data, co, exp, dim, dim);
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
        moe_matvec(L->router.data, b, rlog, c->experts_per_layer, dim);
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
            const float *ef;
            if (m->cfg.ternary_group) {
                /* dequantize, then run the IDENTICAL F32 expert path */
                if (hydra_moe_dequant_expert(m->expert_scratch,
                                             m->expert_f32buf,
                                             c->inter, c->dim,
                                             m->cfg.ternary_group) != 0) {
                    rc = -2; goto done;
                }
                ef = m->expert_f32buf;
            } else {
                /* F32 expert: the scratch buffer holds the expert's raw
                 * float32 bytes (pread'd above). The buffer is a malloc
                 * allocation (4-byte aligned) and the payload is F32 by
                 * format contract, so the float view is well defined. */
                ef = (const float *)m->expert_scratch; // cppcheck-suppress invalidPointerCast
            }
            const float *gate = ef;
            const float *up   = ef + (size_t)inter * dim;
            const float *down = ef + (size_t)2 * inter * dim;
            moe_matvec(gate, b, gl, inter, dim);
            moe_matvec(up, b, ul, inter, dim);
            /* swiglu (local loop: no 4096 cap) */
            for (uint32_t j = 0; j < inter; j++) {
                float g = gl[j];
                hh[j] = (g / (1.0f + expf(-g))) * ul[j];
                if (MOE_NAN_CHECK(hh[j])) { rc = -16; goto done; }
            }
            float *eout = (float *)malloc((size_t)dim * sizeof(float));
            if (!eout) { rc = -1; goto done; }
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
    const float *Wout = m->has_lm_head ? m->lm_head.data : m->embed.data;
    moe_matvec(Wout, outf, logits_out, c->vocab_size, dim);
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

    /* penalize repeats of the last `window` tokens (sign-preserving) */
    float *adj = (float *)malloc((size_t)vocab * sizeof(float));
    if (!adj) return -1;
    for (uint32_t t = 0; t < vocab; t++) {
        float v = logits[t];
        if (MOE_NAN_CHECK(v)) { free(adj); return -1; }
        if (recent_tokens) {
            uint32_t start = window > vocab ? vocab : window;
            for (uint32_t w = 1; w <= start; w++) {
                if (recent_tokens[w - 1] == t) {
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

    /* stable softmax with scale 1/temperature */
    const float T = (float)temperature;
    float mx = -INFINITY;
    for (uint32_t t = 0; t < vocab; t++)
        if (adj[t] > mx) mx = adj[t];
    float sum = 0.0f;
    for (uint32_t t = 0; t < vocab; t++) {
        adj[t] = expf(adj[t] - mx) / T;
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

    /* CDF walk with the shared xorshift64* RNG */
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
    if (moe_utf8_first_error(t, len) != 0) {
        fprintf(stderr,
                "[MoE] text prompt: invalid UTF-8 at byte offset %u\n",
                moe_utf8_first_error(t, len));
        return -2;
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

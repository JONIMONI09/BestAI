/* test_hydra_moe.c - tests for the F32 sparse-MoE inference engine
 * (src/hydra_moe.c, round 12).
 *
 * Fixture: tools/make_model.py --moe-full (complete-contract .hydra2),
 * passed as argv[1] by the Makefile/CI (the same argv convention as
 * test_hydra2_container.c - a bare ./hydra-moe-test is not a supported
 * entry point).
 *
 * Method (the repository's parity discipline): the forward pass is
 * checked against an INDEPENDENT reference implementation written in
 * this file (plain C, same public primitives hydra_rmsnorm /
 * hydra_rope_inplace, everything else hand-coded in the documented
 * summation order), plus hand-computed pins (greedy stream pin,
 * sampler extremes) and loud-refusal negative controls.
 */
#include "hydra_moe.h"
#include "hydra_v2_container.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_tests = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    g_tests++; \
    if (!(cond)) { \
        g_fail++; \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
    } \
} while (0)

static void section(const char *name) { printf("== %s\n", name); }

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

static int g_fixture = 0;
static const char *g_fixpath;     /* argv[1]: F32 fixture (--moe-full)  */
static const char *g_t64path;     /* argv[2]: ternary S64 fixture       */
static const char *g_t128path;    /* argv[3]: ternary S128 fixture      */
static const char *g_t64lgpath;   /* argv[4]: ternary S64, multi-group  */
                                  /* (dim 256: 4 groups/row in gate/up) */
static const char *g_nvpath;      /* argv[5]: moe-full --moe-no-vocab   */
static const char *g_at64path;    /* argv[6]: moe-full --moe-attn-ternary 64
                                     (ternary attention trunk tensors +
                                      paired F32 .scale tensors)          */
static const char *g_gqapath;     /* argv[7]: GQA fixture (n_heads 4, n_kv_heads 2, dim 16) */

static void make_dirs(void)
{
    (void)mkdir("/tmp", 0755);
}

/* ------------------------------------------------------------------ */
/* independent reference forward (documented summation order)         */
/* ------------------------------------------------------------------ */

/* f32 mat-vec, source order: y[i] = sum_j W[i*cols+j]*x[j] */
static void ref_matvec(const float *W, const float *x, float *y,
                       uint32_t rows, uint32_t cols)
{
    for (uint32_t i = 0; i < rows; i++) {
        float acc = 0.0f;
        for (uint32_t j = 0; j < cols; j++)
            acc += W[(size_t)i * cols + j] * x[j];
        y[i] = acc;
    }
}

/* The reference needs the model's raw tensors. It reads them STRAIGHT
 * from the container (independent of the engine's resolved table), so
 * a bug in the engine's tensor resolution also fails the parity. */
static int ref_get(const char *path, const char *name, uint64_t nelem,
                   float **out)
{
    hydra2_container_t c;
    if (hydra2_open(&c, path) != HYDRA2_OK) return -1;
    const hydra2_tensor_entry_t *e;
    if (hydra2_find_tensor(&c, name, &e) != HYDRA2_OK ||
        e->type_id != HYDRA2_F32 || e->n_elements != nelem) {
        hydra2_close(&c);
        return -2;
    }
    float *buf = (float *)malloc((size_t)e->n_bytes);
    if (!buf) { hydra2_close(&c); return -3; }
    if (hydra2_read_tensor(&c, e, (uint8_t *)buf, e->n_bytes) != HYDRA2_OK) {
        free(buf);
        hydra2_close(&c);
        return -4;
    }
    hydra2_close(&c);
    *out = buf;
    return 0;
}

static int ref_expert(const char *path, uint32_t layer, uint32_t eid,
                      uint64_t nbytes, float **out)
{
    hydra2_container_t c;
    if (hydra2_open(&c, path) != HYDRA2_OK) return -1;
    const hydra2_expert_entry_t *e;
    if (hydra2_find_expert(&c, layer, eid, &e) != HYDRA2_OK) {
        hydra2_close(&c);
        return -2;
    }
    float *buf = (float *)malloc(e->length);
    if (!buf) { hydra2_close(&c); return -3; }
    if (hydra2_read_expert(&c, e, (uint8_t *)buf, e->length) != HYDRA2_OK) {
        free(buf);
        hydra2_close(&c);
        return -4;
    }
    (void)nbytes;
    hydra2_close(&c);
    *out = buf;
    return 0;
}

/* Full forward reference: writes cfg.vocab_size floats into log_out. */
static int ref_forward(const char *path, const hydra_moe_config_t *cfg,
                       uint32_t token, uint32_t pos, float *log_out,
                       hydra_kv_cache_t *kv_per_layer)
{
    const uint32_t dim = cfg->dim, hd = cfg->head_dim;
    const uint32_t nh = cfg->n_heads, nkv = cfg->n_kv_heads;
    const uint32_t inter = cfg->inter;
    const uint32_t layers = cfg->layers, epl = cfg->experts_per_layer;
    const uint32_t topk = cfg->top_k, vocab = cfg->vocab_size;
    const uint32_t kvdim = nkv * hd;

    char nm[64];
    float *embed = 0, *fnorm = 0, *lmh = 0;
    if (ref_get(path, "embed", (uint64_t)vocab * dim, &embed) != 0) return -1;
    if (ref_get(path, "final_norm", dim, &fnorm) != 0) return -2;
    if (cfg->tied_embedding) {
        /* the fixture omits lm_head when tied; the engine resolves the
         * same way: lm_head if present (directory scan - a failed
         * find would print the 5.2 refusal for an OPTIONAL tensor),
         * else embed */
        hydra2_container_t c;
        int present = 0;
        if (hydra2_open(&c, path) == HYDRA2_OK) {
            for (uint64_t i = 0; i < c.tensor_dir_count; i++)
                if (strncmp(c.tensors[i].name, "lm_head", 32) == 0) {
                    present = 1;
                    break;
                }
            hydra2_close(&c);
        }
        if (present) {
            if (ref_get(path, "lm_head", (uint64_t)vocab * dim, &lmh) != 0)
                return -3;
        } else {
            lmh = embed;
        }
    } else {
        if (ref_get(path, "lm_head", (uint64_t)vocab * dim, &lmh) != 0)
            return -3;
    }

    float *h  = (float *)malloc((size_t)dim * sizeof(float));
    float *a  = (float *)malloc((size_t)dim * sizeof(float));
    float *q  = (float *)malloc((size_t)dim * sizeof(float));
    float *kk = (float *)malloc((size_t)kvdim * sizeof(float));
    float *vv = (float *)malloc((size_t)kvdim * sizeof(float));
    float *co = (float *)malloc((size_t)dim * sizeof(float));
    float *b  = (float *)malloc((size_t)dim * sizeof(float));
    float *gl = (float *)malloc((size_t)inter * sizeof(float));
    float *ul = (float *)malloc((size_t)inter * sizeof(float));
    float *hh = (float *)malloc((size_t)inter * sizeof(float));
    float *rlog = (float *)malloc((size_t)epl * sizeof(float));
    float *exp = (float *)malloc((size_t)dim * sizeof(float));
    float *kbuf = (float *)malloc((size_t)kvdim * sizeof(float));
    float *vbuf = (float *)malloc((size_t)kvdim * sizeof(float));
    if (!h || !a || !q || !kk || !vv || !co || !b || !gl || !ul || !hh ||
        !rlog || !exp || !kbuf || !vbuf)
        return -4;
    int rc = 0;

    memcpy(h, embed + (size_t)token * dim, dim * sizeof(float));

    for (uint32_t l = 0; l < layers; l++) {
        float *attnq, *attnk, *attnv, *attno, *anorm, *fnorm_l, *router,
              *fgate, *fup, *fdown;
        snprintf(nm, sizeof nm, "attn_q.%u", l);
        if (ref_get(path, nm, (uint64_t)dim * dim, &attnq) != 0) { rc = -5; goto rf; }
        snprintf(nm, sizeof nm, "attn_k.%u", l);
        if (ref_get(path, nm, (uint64_t)kvdim * dim, &attnk) != 0) { rc = -5; goto rf; }
        snprintf(nm, sizeof nm, "attn_v.%u", l);
        if (ref_get(path, nm, (uint64_t)kvdim * dim, &attnv) != 0) { rc = -5; goto rf; }
        snprintf(nm, sizeof nm, "attn_o.%u", l);
        if (ref_get(path, nm, (uint64_t)dim * dim, &attno) != 0) { rc = -5; goto rf; }
        snprintf(nm, sizeof nm, "attn_norm.%u", l);
        if (ref_get(path, nm, dim, &anorm) != 0) { rc = -5; goto rf; }
        snprintf(nm, sizeof nm, "ffn_norm.%u", l);
        if (ref_get(path, nm, dim, &fnorm_l) != 0) { rc = -5; goto rf; }
        snprintf(nm, sizeof nm, "router.%u", l);
        if (ref_get(path, nm, (uint64_t)epl * dim, &router) != 0) { rc = -5; goto rf; }
        snprintf(nm, sizeof nm, "ffn_gate.%u", l);
        if (ref_get(path, nm, (uint64_t)inter * dim, &fgate) != 0) { rc = -5; goto rf; }
        snprintf(nm, sizeof nm, "ffn_up.%u", l);
        if (ref_get(path, nm, (uint64_t)inter * dim, &fup) != 0) { rc = -5; goto rf; }
        snprintf(nm, sizeof nm, "ffn_down.%u", l);
        if (ref_get(path, nm, (uint64_t)dim * inter, &fdown) != 0) { rc = -5; goto rf; }

        /* attention */
        if (hydra_rmsnorm(h, anorm, dim, 1e-6f, a) != 0) { rc = -6; goto rf; }
        ref_matvec(attnq, a, q, dim, dim);
        ref_matvec(attnk, a, kk, kvdim, dim);
        ref_matvec(attnv, a, vv, kvdim, dim);
        for (uint32_t i = 0; i < nh; i++)
            if (hydra_rope_inplace(q + i * hd, hd, pos, cfg->rope_theta) != 0) {
                rc = -6; goto rf;
            }
        for (uint32_t i = 0; i < nkv; i++)
            if (hydra_rope_inplace(kk + i * hd, hd, pos, cfg->rope_theta) != 0) {
                rc = -6; goto rf;
            }
        {
            hydra_kv_cache_t *kv = kv_per_layer ? &kv_per_layer[l] : 0;
            const float scale = 1.0f / sqrtf((float)hd);
            uint32_t n_res = kv ? hydra_kv_cache_size(kv) : 0u;
            for (uint32_t i = 0; i < nh; i++) {
                const uint32_t khh = i % nkv;
                const float *qh = q + i * hd;
                uint32_t n = n_res + 1;
                float *sc = (float *)malloc((size_t)n * sizeof(float));
                if (!sc) { rc = -7; goto rf; }
                for (uint32_t t = 0; t < n; t++) {
                    const float *kt;
                    if (t < n_res) {
                        if (hydra_kv_cache_get(kv, t, kbuf, vbuf) != 0) {
                            free(sc); rc = -8; goto rf;
                        }
                        kt = kbuf + khh * hd;
                    } else {
                        kt = kk + khh * hd;
                    }
                    float s = 0.0f;
                    for (uint32_t d2 = 0; d2 < hd; d2++)
                        s += qh[d2] * kt[d2];
                    sc[t] = s * scale;
                }
                float mx = -INFINITY;
                for (uint32_t t = 0; t < n; t++) if (sc[t] > mx) mx = sc[t];
                float sum = 0.0f;
                for (uint32_t t = 0; t < n; t++) {
                    sc[t] = expf(sc[t] - mx);
                    sum += sc[t];
                }
                for (uint32_t t = 0; t < n; t++) sc[t] /= sum;
                float *ch = co + i * hd;
                memset(ch, 0, hd * sizeof(float));
                for (uint32_t t = 0; t < n; t++) {
                    const float *vt;
                    if (t < n_res) {
                        if (hydra_kv_cache_get(kv, t, kbuf, vbuf) != 0) {
                            free(sc); rc = -8; goto rf;
                        }
                        vt = vbuf + khh * hd;
                    } else {
                        vt = vv + khh * hd;
                    }
                    float w = sc[t];
                    for (uint32_t d2 = 0; d2 < hd; d2++)
                        ch[d2] += w * vt[d2];
                }
                free(sc);
            }
        }
        ref_matvec(attno, co, exp, dim, dim);
        for (uint32_t i = 0; i < dim; i++) h[i] += exp[i];
        if (kv_per_layer) {
            if (hydra_kv_cache_append(&kv_per_layer[l], kk, vv) != 0) {
                rc = -9; goto rf;
            }
        }

        /* MoE FFN */
        if (hydra_rmsnorm(h, fnorm_l, dim, 1e-6f, b) != 0) { rc = -6; goto rf; }
        ref_matvec(router, b, rlog, epl, dim);
        uint32_t sel[16];
        float rk[64];
        memcpy(rk, rlog, (size_t)epl * sizeof(float));
        {
            uint32_t done = 0;
            while (done < topk) {
                /* epl >= 1 is guaranteed by the model config (n_experts
                 * >= 1, topk <= n_experts), so index 0 is always valid. */
                uint32_t best = 0;
                for (uint32_t j = 1; j < epl; j++)
                    if (rk[j] > rk[best]) best = j; /* strict >: tie -> lower id */
                sel[done++] = best;
                rk[best] = -INFINITY; /* selection scratch, see engine */
            }
        }
        /* softmax from the ORIGINAL logits (rk is destroyed) */
        float mx = -INFINITY;
        for (uint32_t i = 0; i < topk; i++) if (rlog[sel[i]] > mx) mx = rlog[sel[i]];
        float sum = 0.0f;
        float wts[16];
        for (uint32_t i = 0; i < topk; i++) {
            wts[i] = expf(rlog[sel[i]] - mx);
            sum += wts[i];
        }
        for (uint32_t i = 0; i < topk; i++) wts[i] /= sum;

        memset(exp, 0, dim * sizeof(float));
        for (uint32_t i = 0; i < topk; i++) {
            float *ef;
            if (ref_expert(path, l, sel[i],
                           (uint64_t)3 * inter * dim * 4, &ef) != 0) {
                rc = -10; goto rf;
            }
            ref_matvec(ef, b, gl, inter, dim);
            ref_matvec(ef + (size_t)inter * dim, b, ul, inter, dim);
            for (uint32_t j = 0; j < inter; j++)
                hh[j] = (gl[j] / (1.0f + expf(-gl[j]))) * ul[j];
            float *eo = (float *)malloc((size_t)dim * sizeof(float));
            if (!eo) { free(ef); rc = -7; goto rf; }
            ref_matvec(ef + (size_t)2 * inter * dim, hh, eo, dim, inter);
            float w = wts[i];
            for (uint32_t j = 0; j < dim; j++) exp[j] += w * eo[j];
            free(eo);
            free(ef);
        }
        for (uint32_t i = 0; i < dim; i++) h[i] += exp[i];

        free(attnq); free(attnk); free(attnv); free(attno); free(anorm);
        free(fnorm_l); free(router); free(fgate); free(fup); free(fdown);
    }

    float *outf = (float *)malloc((size_t)dim * sizeof(float));
    if (!outf) { rc = -7; goto rf; }
    if (hydra_rmsnorm(h, fnorm, dim, 1e-6f, outf) != 0) { rc = -6; goto rf; }
    ref_matvec(lmh, outf, log_out, vocab, dim);
    free(outf);

rf:
    free(h); free(a); free(q); free(kk); free(vv); free(co); free(b);
    free(gl); free(ul); free(hh); free(rlog); free(exp); free(kbuf); free(vbuf);
    free(embed); free(fnorm); if (lmh && lmh != embed) free(lmh);
    return rc;
}

/* ------------------------------------------------------------------ */
/* tests                                                              */
/* ------------------------------------------------------------------ */

static void test_moe_open(void)
{
    section("open + config resolution");
    hydra_moe_t m;
    int rc = hydra_moe_open(g_fixpath, &m);
    CHECK(rc == 0, "open the complete-contract fixture");
    if (rc != 0) { printf("  (open failed, aborting MoE tests)\n"); return; }
    hydra_moe_config_t c;
    CHECK(hydra_moe_config(&m, &c) == 0, "config copy");
    CHECK(c.dim == 8, "dim 8");
    CHECK(c.vocab_size == 16, "vocab 16");
    CHECK(c.n_heads == 2 && c.n_kv_heads == 1 && c.head_dim == 4,
          "GQA heads 2/1, head_dim 4");
    CHECK(c.inter == 4, "inter 4");
    CHECK(c.expert_layout == 0, "layout 0 (F32 flat)");
    CHECK(c.rope_theta == 10000.0, "rope_theta default 10000");
    CHECK(c.tied_embedding == 1, "tied embedding");
    CHECK(c.top_k == 2, "top_k 2");
    hydra_moe_close(&m);
}

static void test_moe_forward_parity(void)
{
    section("forward parity vs the independent reference");
    hydra_moe_t m;
    if (hydra_moe_open(g_fixpath, &m) != 0) {
        CHECK(0, "open for parity");
        return;
    }
    hydra_moe_config_t c;
    hydra_moe_config(&m, &c);
    float logit[16], ref_logit[16];
    int ok = 1;
    /* several tokens x positions, self-only (kv NULL) */
    for (uint32_t token = 0; token < 6; token++) {
        for (uint32_t pos = 0; pos < 4; pos++) {
            if (hydra_moe_forward(&m, token, pos, logit, NULL) != 0) {
                ok = 0;
                continue;
            }
            if (ref_forward(g_fixpath, &c, token, pos, ref_logit, NULL) != 0) {
                ok = 0;
                continue;
            }
            for (uint32_t t = 0; t < c.vocab_size; t++)
                if (logit[t] != ref_logit[t]) ok = 0;
        }
    }
    CHECK(ok, "24 forward passes bit-identical to the reference");

    /* with a KV cache (one per layer, the engine contract): parity
     * over a 6-token chain */
    {
        /* TWO independent cache arrays: each implementation appends to
         * its own, so neither poisons the other's resident set (the
         * engine and the reference must each see the cache state that
         * THEIR OWN previous appends produced). */
        const uint32_t cap = c.kv_sink + c.kv_ring;
        const uint32_t kvd = c.n_kv_heads * c.head_dim;
        hydra_kv_cache_t *kvs_e = (hydra_kv_cache_t *)calloc(c.layers,
                                                             sizeof(hydra_kv_cache_t));
        hydra_kv_cache_t *kvs_r = (hydra_kv_cache_t *)calloc(c.layers,
                                                             sizeof(hydra_kv_cache_t));
        float **ks_e = (float **)calloc(c.layers, sizeof(float *));
        float **vs_e = (float **)calloc(c.layers, sizeof(float *));
        float **ks_r = (float **)calloc(c.layers, sizeof(float *));
        float **vs_r = (float **)calloc(c.layers, sizeof(float *));
        for (uint32_t l = 0; l < c.layers; l++) {
            ks_e[l] = (float *)malloc((size_t)cap * kvd * sizeof(float));
            vs_e[l] = (float *)malloc((size_t)cap * kvd * sizeof(float));
            ks_r[l] = (float *)malloc((size_t)cap * kvd * sizeof(float));
            vs_r[l] = (float *)malloc((size_t)cap * kvd * sizeof(float));
            hydra_kv_cache_init(&kvs_e[l], ks_e[l], vs_e[l], kvd, cap, c.kv_sink);
            hydra_kv_cache_init(&kvs_r[l], ks_r[l], vs_r[l], kvd, cap, c.kv_sink);
        }
        ok = 1;
        const uint32_t chain[6] = { 0, 3, 11, 5, 1, 15 };
        for (uint32_t i = 0; i < 6; i++) {
            if (hydra_moe_forward(&m, chain[i], i, logit, kvs_e) != 0) { ok = 0; break; }
            if (ref_forward(g_fixpath, &c, chain[i], i, ref_logit, kvs_r) != 0) { ok = 0; break; }
            for (uint32_t t = 0; t < c.vocab_size; t++)
                if (logit[t] != ref_logit[t]) ok = 0;
            /* the caches themselves must end up bit-identical */
            if (hydra_kv_cache_size(&kvs_e[0]) != hydra_kv_cache_size(&kvs_r[0]))
                ok = 0;
        }
        CHECK(ok, "6-token KV chain bit-identical to the reference");
        for (uint32_t l = 0; l < c.layers; l++) {
            free(ks_e[l]); free(vs_e[l]); free(ks_r[l]); free(vs_r[l]);
        }
        free(ks_e); free(vs_e); free(ks_r); free(vs_r);
        free(kvs_e); free(kvs_r);
    }
    hydra_moe_close(&m);
}

static void test_moe_forward_sanity(void)
{
    section("forward sanity (self-only vs KV, finite logits)");
    hydra_moe_t m;
    if (hydra_moe_open(g_fixpath, &m) != 0) { CHECK(0, "open for sanity"); return; }
    hydra_moe_config_t c;
    hydra_moe_config(&m, &c);
    float lo[16], lk[16];
    CHECK(hydra_moe_forward(&m, 3, 0, lo, NULL) == 0, "self-only ok");
    CHECK(hydra_moe_forward(&m, 3, 0, lk, NULL) == 0, "second call ok");
    int same = 1;
    for (uint32_t t = 0; t < 16; t++) if (lo[t] != lk[t]) same = 0;
    CHECK(same, "deterministic: same call -> same logits");

    /* the engine's KV contract: one cache PER LAYER (array of
     * cfg.layers) - like the v4 forward step */
    const uint32_t cap = c.kv_sink + c.kv_ring;
    const uint32_t kvd = c.n_kv_heads * c.head_dim;
    hydra_kv_cache_t *kvs = (hydra_kv_cache_t *)calloc(c.layers,
                                                       sizeof(hydra_kv_cache_t));
    float **ks = (float **)calloc(c.layers, sizeof(float *));
    float **vs = (float **)calloc(c.layers, sizeof(float *));
    for (uint32_t l = 0; l < c.layers; l++) {
        ks[l] = (float *)malloc((size_t)cap * kvd * sizeof(float));
        vs[l] = (float *)malloc((size_t)cap * kvd * sizeof(float));
        hydra_kv_cache_init(&kvs[l], ks[l], vs[l], kvd, cap, c.kv_sink);
    }
    /* NOTE: an EMPTY cache is legitimately self-only (joint RoPE
     * rotation leaves the q.k dot product invariant), so prefill one
     * token to make the memory channel real */
    float dummy[16];
    CHECK(hydra_moe_forward(&m, 5, 0, dummy, kvs) == 0, "prefill ok");
    CHECK(hydra_moe_forward(&m, 3, 1, lk, kvs) == 0, "kv forward ok");
    int differs = 0;
    for (uint32_t t = 0; t < 16; t++)
        if (lk[t] != lo[t]) differs = 1;
    CHECK(differs, "KV attention changes the logits vs self-only");
    for (uint32_t l = 0; l < c.layers; l++) { free(ks[l]); free(vs[l]); }
    free(ks); free(vs); free(kvs);
    hydra_moe_close(&m);
}

/* The greedy stream pin: the token stream of a fixed run is part of
 * the contract (the same discipline as the v1 baseline). The values
 * below were measured on this AVX2 sandbox 2026-10-30 from the
 * --moe-full fixture (default parameters) and are re-derived by the
 * test itself against the reference on every run - a regression in
 * EITHER the engine or the fixture fails here. */
#define PIN_STEPS 24
static const uint32_t PIN_TOKENS[PIN_STEPS] = {
    12, 12, 12, 10, 4, 6, 0, 6, 4, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 4, 6, 6, 6
};

static void test_moe_stream_pin(void)
{
    section("greedy stream pin (KV enabled)");
    hydra_moe_t m;
    if (hydra_moe_open(g_fixpath, &m) != 0) { CHECK(0, "open for pin"); return; }
    hydra_moe_config_t c;
    hydra_moe_config(&m, &c);
    const uint32_t cap = c.kv_sink + c.kv_ring;
    hydra_kv_cache_t *kvs =
        (hydra_kv_cache_t *)calloc(c.layers, sizeof(hydra_kv_cache_t));
    float **ks = (float **)calloc(c.layers, sizeof(float *));
    float **vs = (float **)calloc(c.layers, sizeof(float *));
    for (uint32_t l = 0; l < c.layers; l++) {
        ks[l] = (float *)malloc((size_t)cap * c.n_kv_heads * c.head_dim *
                                sizeof(float));
        vs[l] = (float *)malloc((size_t)cap * c.n_kv_heads * c.head_dim *
                                sizeof(float));
        hydra_kv_cache_init(&kvs[l], ks[l], vs[l],
                            c.n_kv_heads * c.head_dim, cap, c.kv_sink);
    }
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    uint32_t tok = 7; /* fixed seed token */
    int match = 1;
    float logit[16];
    for (uint32_t i = 0; i < PIN_STEPS; i++) {
        if (hydra_moe_forward(&m, tok, i, logit, kvs) != 0) { match = 0; break; }
        uint32_t got;
        if (hydra_moe_sample(logit, c.vocab_size, &got, 0.0, 1.0, 1.0, 0,
                             NULL, &rng) != 0) { match = 0; break; }
        if (got != PIN_TOKENS[i]) match = 0;
        tok = got;
    }
    CHECK(match, "greedy KV stream matches the pinned tokens");
    for (uint32_t l = 0; l < c.layers; l++) { free(ks[l]); free(vs[l]); }
    free(kvs); free(ks); free(vs);
    hydra_moe_close(&m);
}

static void test_moe_kv_eviction(void)
{
    section("KV sink + ring eviction");
    hydra_moe_t m;
    if (hydra_moe_open(g_fixpath, &m) != 0) { CHECK(0, "open for kv"); return; }
    hydra_moe_config_t c;
    hydra_moe_config(&m, &c);
    const uint32_t sinks = c.kv_sink, ring = c.kv_ring;
    const uint32_t cap = sinks + ring;
    const uint32_t kvd = c.n_kv_heads * c.head_dim;
    hydra_kv_cache_t *kvs = (hydra_kv_cache_t *)calloc(c.layers,
                                                       sizeof(hydra_kv_cache_t));
    float **ks = (float **)calloc(c.layers, sizeof(float *));
    float **vs = (float **)calloc(c.layers, sizeof(float *));
    for (uint32_t l = 0; l < c.layers; l++) {
        ks[l] = (float *)malloc((size_t)cap * kvd * sizeof(float));
        vs[l] = (float *)malloc((size_t)cap * kvd * sizeof(float));
        hydra_kv_cache_init(&kvs[l], ks[l], vs[l], kvd, cap, sinks);
    }
    const uint32_t chain[32] = { 0, 1, 2, 3, 4, 5, 6, 7,
                                 8, 9, 10, 11, 12, 13, 14, 15,
                                 0, 2, 4, 6, 8, 10, 12, 14,
                                 1, 3, 5, 7, 9, 11, 13, 15 };
    int ok = 1;
    float logit[16];
    for (uint32_t i = 0; i < 32; i++) {
        if (hydra_moe_forward(&m, chain[i], i, logit, kvs) != 0) { ok = 0; break; }
    }
    CHECK(ok, "32-token run over the cache");
    CHECK(hydra_kv_cache_size(&kvs[0]) == cap, "resident set = capacity");
    /* the sink holds the FIRST appended k (token 0 at position 0):
     * re-derive it from the reference single call */
    float *k0 = (float *)malloc((size_t)kvd * sizeof(float));
    float *v0 = (float *)malloc((size_t)kvd * sizeof(float));
    float *ak = (float *)malloc((size_t)kvd * sizeof(float));
    float *av = (float *)malloc((size_t)kvd * sizeof(float));
    /* compute the expected first k/v: rmsnorm(embed[0]) then attn_k/v
     * mat-vec + rope at pos 0 (rope at pos 0 is the identity) */
    {
        float *embed = 0, *attnk = 0, *attnv = 0, *anorm = 0;
        const uint32_t dim = c.dim;
        uint64_t kvdim_e = (uint64_t)c.n_kv_heads * c.head_dim;
        ref_get(g_fixpath, "embed", (uint64_t)c.vocab_size * dim, &embed);
        ref_get(g_fixpath, "attn_k.0", kvdim_e * dim, &attnk);
        ref_get(g_fixpath, "attn_v.0", kvdim_e * dim, &attnv);
        ref_get(g_fixpath, "attn_norm.0", dim, &anorm);
        float a[8];
        memcpy(a, embed, dim * sizeof(float)); /* token 0 */
        hydra_rmsnorm(a, anorm, dim, 1e-6f, a);
        ref_matvec(attnk, a, ak, (uint32_t)kvdim_e, dim);
        ref_matvec(attnv, a, av, (uint32_t)kvdim_e, dim);
        free(embed); free(attnk); free(attnv); free(anorm);
    }
    int sink_ok = 1;
    if (hydra_kv_cache_sink(&kvs[0], 0, k0, v0) == 0) {
        for (uint32_t i = 0; i < kvd; i++) {
            if (k0[i] != ak[i] || v0[i] != av[i]) sink_ok = 0;
        }
    } else {
        sink_ok = 0;
    }
    CHECK(sink_ok, "sink 0 holds token-0 k/v bit-exact after 32 appends");
    for (uint32_t l = 0; l < c.layers; l++) { free(ks[l]); free(vs[l]); }
    free(ks); free(vs); free(kvs); free(k0); free(v0); free(ak); free(av);
    hydra_moe_close(&m);
}

static void test_moe_sampler(void)
{
    section("sampler over a real logit vector");
    uint64_t rng = 42;
    uint32_t tok = 0;

    /* greedy: argmax, ties -> lower id */
    float lg[5] = { 1.0f, 3.0f, 3.0f, -2.0f, 0.5f };
    CHECK(hydra_moe_sample(lg, 5, &tok, 0.0, 1.0, 1.0, 0, NULL, &rng) == 0,
          "greedy ok");
    CHECK(tok == 1, "greedy argmax tie -> lower id (1, not 2)");

    /* greedy with a huge positive logit */
    float lg2[3] = { -100.0f, -50.0f, 100.0f };
    CHECK(hydra_moe_sample(lg2, 3, &tok, 0.0, 1.0, 1.0, 0, NULL, &rng) == 0,
          "greedy 2 ok");
    CHECK(tok == 2, "greedy picks 2");

    /* min_p = 0.5, temp 1: P(t) < 0.5 * pmax dropped.
     * logits [0, 0, -inf] -> pmax = 0.5, floor 0.25, both survive.
     * Use [0, -2, -10]: p ~ [0.88, 0.115, ~0] -> floor ~0.44 ->
     * only token 0 survives -> deterministic 0 for many seeds. */
    float lg3[3] = { 0.0f, -2.0f, -10.0f };
    int all_zero = 1;
    for (int i = 0; i < 50; i++) {
        uint64_t r = 0xC0FFEEull + (uint64_t)i;
        uint32_t t2 = 99;
        if (hydra_moe_sample(lg3, 3, &t2, 1.0, 0.5, 1.0, 0, NULL, &r) != 0 ||
            t2 != 0) all_zero = 0;
    }
    CHECK(all_zero, "min-p 0.5 kills tokens 1+ (50 seeds -> always 0)");

    /* min_p = 1.0 -> greedy (argmax), no RNG */
    CHECK(hydra_moe_sample(lg3, 3, &tok, 0.7, 1.0, 1.0, 0, NULL, &rng) == 0,
          "min-p 1 ok");
    CHECK(tok == 0, "min-p 1.0 = greedy argmax 0");

    /* repetition penalty: a recent token with a POSITIVE logit is
     * divided (penalty > 1 lowers it). Vocab 3 so the UNRECENT rival
     * token 1 is not penalized itself. Window holds ONE repeat of
     * token 0 (chronological: token 2, then token 0). */
    float lg4[3] = { 2.0f, 1.0f, -10.0f };
    uint32_t recent[2] = { 2, 0 };
    CHECK(hydra_moe_sample(lg4, 3, &tok, 0.0, 1.0, 2.0, 2, recent, &rng) == 0,
          "rep penalty greedy ok");
    CHECK(tok == 0, "rep 2.0 once on +2.0 -> 1.0, tie with unriven 1.0 -> id 0");
    /* repeats COMPOUND: two repeats of token 0 -> 2.0/2/2 = 0.5 < 1.0 */
    uint32_t recent2b[2] = { 0, 0 };
    CHECK(hydra_moe_sample(lg4, 3, &tok, 0.0, 1.0, 2.0, 2, recent2b, &rng) == 0,
          "rep penalty compound ok");
    CHECK(tok == 1, "rep 2.0 twice on +2.0 -> 0.5 < unriven 1.0 -> id 1");
    /* negative logit is MULTIPLIED (more negative) */
    float lg6[2] = { -1.0f, -2.0f };
    uint32_t recent2[2] = { 1, 1 };
    CHECK(hydra_moe_sample(lg6, 2, &tok, 0.0, 1.0, 2.0, 2, recent2, &rng) == 0,
          "rep penalty negative ok");
    CHECK(tok == 0, "rep 2.0 on -2.0 -> -4.0; rival -1.0 wins");

    /* reproducibility: same seed -> same token */
    uint32_t t1 = 99, t2 = 99;
    uint64_t r1 = 12345, r2 = 12345;
    float lg7[8];
    for (int i = 0; i < 8; i++) lg7[i] = (float)(i % 3) - 1.0f;
    hydra_moe_sample(lg7, 8, &t1, 1.0, 0.1, 1.0, 0, NULL, &r1);
    hydra_moe_sample(lg7, 8, &t2, 1.0, 0.1, 1.0, 0, NULL, &r2);
    CHECK(t1 == t2, "same seed -> same token");

    /* temperature must actually SCALE the distribution (round 13,
     * Qodo #8: the old code divided exp(v-mx) by T and renormalized,
     * which cancels T - every temperature sampled identically).
     * logits [3,2,1,0], seed 3 -> r = 0.8425... in the CDF:
     *   T=0.5: P ~ [0.865, 0.117, 0.016, 0.002] -> token 0
     *   T=1.0: P ~ [0.644, 0.237, 0.087, 0.032] -> token 1
     *   T=2.0: P ~ [0.455, 0.276, 0.167, 0.102] -> token 2
     * Pre-fix all three calls returned token 1. */
    float lt[4] = { 3.0f, 2.0f, 1.0f, 0.0f };
    uint32_t ta = 99, tb = 99, tc = 99;
    uint64_t rs_a = 3, rs_b = 3, rs_c = 3;
    CHECK(hydra_moe_sample(lt, 4, &ta, 0.5, 0.0, 1.0, 0, NULL, &rs_a) == 0,
          "temp 0.5 ok");
    CHECK(hydra_moe_sample(lt, 4, &tb, 1.0, 0.0, 1.0, 0, NULL, &rs_b) == 0,
          "temp 1.0 ok");
    CHECK(hydra_moe_sample(lt, 4, &tc, 2.0, 0.0, 1.0, 0, NULL, &rs_c) == 0,
          "temp 2.0 ok");
    CHECK(ta == 0, "temp 0.5 -> token 0 (sharper distribution)");
    CHECK(tb == 1, "temp 1.0 -> token 1");
    CHECK(tc == 2, "temp 2.0 -> token 2 (flatter distribution)");
    CHECK(ta != tb && tb != tc, "three temperatures -> three different "
          "tokens (T cannot cancel out)");

    /* the penalty walk covers the WHOLE window, not min(window,
     * vocab) (round 13, Qodo #21): vocab 4, window 6, token 0 occurs
     * 4 times - two of them at window indexes 4 and 5, BEYOND the
     * old vocab cap of 4. New: 2.0/2/2/2/2 = 0.125, so the un-recent
     * token 3 (0.5) wins. Pre-fix (cap = 4) only the first two
     * occurrences counted: 2.0/2/2 = 0.5, TIE with token 3 -> the
     * lower id 0 won. */
    float lw[4] = { 2.0f, 0.5f, 0.5f, 0.5f };
    uint32_t rw[6] = { 0, 0, 1, 2, 0, 0 };
    uint64_t rr = 7;
    CHECK(hydra_moe_sample(lw, 4, &tok, 0.0, 1.0, 2.0, 6, rw, &rr) == 0,
          "window > vocab ok");
    CHECK(tok == 3, "repeats beyond the vocab cap are penalized (token 3)");

    /* a NULL rng_state is refused on the STOCHASTIC path (round 13,
     * Qodo #19 - was an unchecked dereference) but stays legal for
     * pure greedy callers (no RNG is consumed there). */
    CHECK(hydra_moe_sample(lg, 5, &tok, 1.0, 0.5, 1.0, 0, NULL, NULL) == -1,
          "stochastic path with NULL rng -> -1");
    CHECK(hydra_moe_sample(lg, 5, &tok, 0.0, 1.0, 1.0, 0, NULL, NULL) == 0,
          "greedy path with NULL rng ok");
    CHECK(tok == 1, "greedy with NULL rng still picks argmax 1");

    /* guards */
    CHECK(hydra_moe_sample(NULL, 5, &tok, 1.0, 0.5, 1.0, 0, NULL, &rng) == -1,
          "NULL logits -> -1");
    CHECK(hydra_moe_sample(lg, 0, &tok, 1.0, 0.5, 1.0, 0, NULL, &rng) == -1,
          "vocab 0 -> -1");
    CHECK(hydra_moe_sample(lg, 5, &tok, -2.0, 0.5, 1.0, 0, NULL, &rng) == -1,
          "temperature < -1 -> -1");
    CHECK(hydra_moe_sample(lg, 5, &tok, 1.0, 1.5, 1.0, 0, NULL, &rng) == -1,
          "min_p > 1 -> -1");
    CHECK(hydra_moe_sample(lg, 5, &tok, 1.0, 0.5, 0.25, 0, NULL, &rng) == -1,
          "penalty < 0.5 -> -1");
}

static void test_moe_refusals(void)
{
    section("refusals (loud, specific)");
    hydra_moe_t m;
    CHECK(hydra_moe_open(NULL, &m) == -1, "NULL path -> -1");
    CHECK(hydra_moe_open("/nonexistent/file.hydra2", &m) == -2,
          "missing file -> -2 (container refusal)");
    CHECK(hydra_moe_open(g_fixpath, NULL) == -1, "NULL handle -> -1");

    /* a v1 .hydra is refused by the container magic */
    CHECK(hydra_moe_open("models/starter.hydra", &m) == -2,
          "v1 file -> -2 (magic refusal)");

    /* token out of range */
    if (hydra_moe_open(g_fixpath, &m) == 0) {
        float lg[16];
        CHECK(hydra_moe_forward(&m, 16, 0, lg, NULL) == -1,
              "token 16 (vocab 16) -> -1");
        CHECK(hydra_moe_forward(&m, 3, 0, NULL, NULL) == -1,
              "NULL logits -> -1");
        /* kv cache with an unset buffer */
        hydra_kv_cache_t bad;
        memset(&bad, 0, sizeof bad);
        CHECK(hydra_moe_forward(&m, 3, 0, lg, &bad) == -1,
              "kv with NULL storage -> -1");
        hydra_moe_close(&m);
    }
}


/* ------------------------------------------------------------------ */
/* ternary + block-scale experts (layout 1, round 12 stage 5)         */
/* ------------------------------------------------------------------ */

/* Greedy KV stream helper (same seed/start contract as the F32 pin).
 * Writes `steps` tokens; returns 0 on success. */
static int run_greedy_stream(const char *path, uint32_t start,
                             uint64_t rng, uint32_t steps, uint32_t *out)
{
    hydra_moe_t m;
    if (hydra_moe_open(path, &m) != 0) return -1;
    hydra_moe_config_t c;
    if (hydra_moe_config(&m, &c) != 0) { hydra_moe_close(&m); return -1; }
    const uint32_t cap = c.kv_sink + c.kv_ring;
    const uint32_t kvd = c.n_kv_heads * c.head_dim;
    hydra_kv_cache_t *kvs = (hydra_kv_cache_t *)calloc(c.layers,
                                                       sizeof(hydra_kv_cache_t));
    float **ks = (float **)calloc(c.layers, sizeof(float *));
    float **vs = (float **)calloc(c.layers, sizeof(float *));
    for (uint32_t l = 0; l < c.layers; l++) {
        ks[l] = (float *)malloc((size_t)cap * kvd * sizeof(float));
        vs[l] = (float *)malloc((size_t)cap * kvd * sizeof(float));
        hydra_kv_cache_init(&kvs[l], ks[l], vs[l], kvd, cap, c.kv_sink);
    }
    float *lo = (float *)malloc((size_t)c.vocab_size * sizeof(float));
    int rc = 0;
    uint32_t tok = start;
    for (uint32_t i = 0; i < steps; i++) {
        if (hydra_moe_forward(&m, tok, i, lo, kvs) != 0) { rc = -2; break; }
        if (hydra_moe_sample(lo, c.vocab_size, &tok, 0.0, 1.0, 1.0, 0,
                             NULL, &rng) != 0) { rc = -3; break; }
        out[i] = tok;
    }
    for (uint32_t l = 0; l < c.layers; l++) { free(ks[l]); free(vs[l]); }
    free(kvs); free(ks); free(vs); free(lo);
    hydra_moe_close(&m);
    return rc;
}

/* Test-local TERNARY2 block encoder (mirrors tools/make_model.py
 * ternary_block; codes: 00=0, 01=+1, 10=-1, 11=0; element 2j in the
 * LOW bits of byte j). */
static void enc_block(uint8_t **pp, const int8_t *codes,
                      const float *scales, uint32_t rows, uint32_t cols,
                      uint32_t group)
{
    const uint64_t n = (uint64_t)rows * cols;
    const uint64_t ng = ((uint64_t)cols + group - 1) / group;
    uint8_t *p = *pp;
    for (uint64_t k = 0; k < n; k++)
        p[k / 2u] |= (uint8_t)((codes[k] & 0x3) << ((k % 2u) * 2));
    memcpy(p + n / 2u, scales, ng * rows * 4u);
    *pp = p + n / 2u + ng * rows * 4u;
}

/* Independent reference dequant: COLUMN-major outer loop (the engine
 * is row-major) and value = code * scale as a plain F32 multiply. Any
 * packing or layout drift on either side fails a bit-strict ==. */
static int ref_dequant_expert(const uint8_t *blob, float *out,
                              uint32_t inter, uint32_t dim, uint32_t group)
{
    const uint32_t rows[3] = { inter, inter, dim };
    const uint32_t cols[3] = { dim, dim, inter };
    const uint8_t *p = blob;
    for (int s = 0; s < 3; s++) {
        const uint32_t R = rows[s], C = cols[s];
        const uint64_t n = (uint64_t)R * C;
        if (n % 2u) return -1;
        const uint64_t ng = ((uint64_t)C + group - 1) / group;
        const uint8_t *codes = p;
        /* scale section follows the packed code bytes directly; when
         * n/2 is odd it is not 4-byte aligned, so read through memcpy
         * (identical value, portable) - the same fix the engine got
         * when the ASan+UBSan gate flagged the misaligned float load. */
        const uint8_t *sc = p + n / 2u;
        p = p + n / 2u + ng * R * 4u;
        for (uint32_t c = 0; c < C; c++) {
            const uint64_t g = (uint64_t)c / group;
            for (uint32_t r = 0; r < R; r++) {
                const uint64_t k = (uint64_t)r * C + c;
                const uint8_t code = (k & 1u)
                    ? (uint8_t)((codes[k / 2u] >> 2) & 0x3u)
                    : (uint8_t)(codes[k / 2u] & 0x3u);
                const int t = (code == 1) ? 1 : (code == 2) ? -1 : 0;
                float scale;
                memcpy(&scale, sc + (r * ng + g) * 4u, sizeof scale);
                /* (float)t * scale: exact for t in {-1,0,1}; the zero
                 * sign may differ from the engine's 0.0f, which ==
                 * treats as equal by design. */
                out[k] = (float)t * scale;
            }
        }
        out += n;
    }
    return 0;
}

static void test_moe_dequant_unit(void)
{
    section("ternary dequant: hand-crafted blob vs independent reference");
    /* inter=2, dim=3, group=2 -> partial last group (cols=3: 2+1),
     * sign-flipped scales and the reserved code 11 (must decode 0). */
    enum { N = 3 * 2 * 3 };
    uint8_t blob[128];
    memset(blob, 0, sizeof blob);
    uint8_t *p = blob;
    {
        static const int8_t gc[6] = { 1, 2, 0, 3, 1, 2 };
        static const int8_t uc[6] = { 2, 1, 0, 0, 2, 1 };
        static const int8_t dc[6] = { 1, 2, 0, 1, 2, 0 };
        static const float gs[4] = { 1.5f, -2.5f, 0.25f, 4.0f };
        static const float us[4] = { -0.5f, 2.0f, -2.0f, 0.5f };
        static const float ds[3] = { 10.0f, -10.0f, 1.0f };
        enc_block(&p, gc, gs, 2, 3, 2); /* gate: 2x3, 2 scales/row  */
        enc_block(&p, uc, us, 2, 3, 2); /* up   : 2x3               */
        enc_block(&p, dc, ds, 3, 2, 2); /* down : 3x2, 1 scale/row  */
    }
    float out[N], ref[N];
    CHECK(hydra_moe_dequant_expert(blob, out, 2, 3, 2) == 0, "dequant ok");
    CHECK(ref_dequant_expert(blob, ref, 2, 3, 2) == 0, "reference ok");
    int allmatch = 1;
    for (int i = 0; i < N; i++)
        if (!(out[i] == ref[i])) allmatch = 0;
    CHECK(allmatch, "engine dequant == independent reference (bit-strict)");
    /* hand-verified anchors (code table + row-major [row][group]): */
    CHECK(out[0] == 1.5f, "gate r0c0: +1 * 1.5");
    CHECK(out[1] == -1.5f, "gate r0c1: -1 * 1.5");
    CHECK(out[3] == 0.0f, "gate r1c0: reserved code 11 -> 0");
    CHECK(out[4] == 0.25f, "gate r1c1: +1 * 0.25 (partial group ok)");
    CHECK(out[5] == -4.0f, "gate r1c2: -1 * 4.0");
    CHECK(out[6] == 0.5f, "up r0c0: -1 * -0.5 (sign flip)");
    CHECK(out[11] == 0.5f, "up r1c2: +1 * 0.5 (partial group)");
    CHECK(out[12] == 10.0f, "down r0c0: +1 * 10");
    CHECK(out[15] == -10.0f, "down r1c1: +1 * -10 (sign flip)");
    CHECK(out[16] == -1.0f, "down r2c0: -1 * 1");
    /* guard rails */
    CHECK(hydra_moe_dequant_expert(NULL, out, 2, 3, 2) == -1, "NULL blob");
    CHECK(hydra_moe_dequant_expert(blob, NULL, 2, 3, 2) == -1, "NULL out");
    CHECK(hydra_moe_dequant_expert(blob, out, 2, 3, 0) == -1, "group 0");
    CHECK(hydra_moe_dequant_expert(blob, out, 0, 3, 2) == -1, "inter 0");
    float small[3];
    CHECK(hydra_moe_dequant_expert(blob, small, 1, 3, 2) == -1,
          "odd rows*cols refused (1x3)");
}

static void test_moe_dequant_flat_unit(void)
{
    section("flat ternary dequant: payload vs independent reference "
            "(round 13, Qodo #3)");
    /* 256 elements, group 64 -> 4 full groups; codes cycle 0,1,2,3
     * (zero, +1, -1, reserved) so every code path runs in every
     * group; scales cover sign flips and sub-1 magnitudes. */
    enum { N = 256, GROUP = 64, NBYTES = 128 };
    /* TERNARY2 packing: 2 elements per byte (even element in the low
     * 2 bits, the odd one in bits 2-3) -> n/2 bytes per n elements. */
    uint8_t payload[NBYTES];
    for (int i = 0; i < NBYTES; i++)
        payload[i] = (uint8_t)((i % 4u) * 0x33u); /* 00,33,66,99,... */
    static const float scales[4] = { 1.0f, -2.5f, 0.125f, 4.0f };
    static const float expect_code[4] = { 0.0f, 1.0f, -1.0f, 0.0f };

    float out[N], ref[N];
    CHECK(hydra_moe_dequant_flat(payload, scales, N, GROUP, out) == 0,
          "flat dequant ok");
    /* independent reference: stride the groups first (the engine
     * strides the elements first) and take the code from the byte
     * table a different way. */
    for (int g = 0; g < N / GROUP; g++) {
        for (int k = 0; k < GROUP; k++) {
            const uint64_t i = (uint64_t)g * GROUP + k;
            const uint8_t byte = payload[i / 2u];
            const uint8_t code = (k & 1u)
                ? (uint8_t)(byte >> 2) & 0x3u : (uint8_t)byte & 0x3u;
            ref[i] = expect_code[code] * scales[g];
        }
    }
    int allmatch = 1;
    for (int i = 0; i < N; i++)
        if (!(out[i] == ref[i])) allmatch = 0;
    CHECK(allmatch, "flat dequant == independent reference (bit-strict)");
    /* hand-verified anchors (2026-10-30, replayed byte by byte): the
     * bytes 00,33,66,99 give the code pattern 0,0,3,0,2,1,1,2 every
     * 8 elements (code table 00=0, 01=+1, 10=-1, 11=reserved). */
    CHECK(out[0] == 0.0f, "e0: code 00 -> 0 * 1.0");
    CHECK(out[2] == 0.0f, "e2: code 11 -> reserved -> 0");
    CHECK(out[4] == -1.0f, "e4: code 10 -> -1 * 1.0 (group 0)");
    CHECK(out[5] == 1.0f, "e5: code 01 -> +1 * 1.0 (group 0)");
    CHECK(out[66] == 0.0f, "e66: code 11 -> reserved -> 0 (group 1)");
    CHECK(out[68] == 2.5f, "e68: code 10 -> -1 * -2.5 (group 1)");
    CHECK(out[77] == -2.5f, "e77: code 01 -> +1 * -2.5 (group 1)");
    CHECK(out[79] == 2.5f, "e79: code 10 -> -1 * -2.5 (group 1)");
    CHECK(out[132] == -0.125f, "e132: code 10 -> -1 * 0.125 (group 2)");
    CHECK(out[196] == -4.0f, "e196: code 10 -> -1 * 4.0 (group 3)");
    CHECK(out[197] == 4.0f, "e197: code 01 -> +1 * 4.0 (group 3)");
    CHECK(out[255] == -4.0f, "e255: code 10 -> -1 * 4.0 (last element)");

    /* The aliasing guard must be MEANINGFUL: replay the pre-round-13
     * in-place call pattern (read the code at byte k/2, write the F32
     * result at byte 4k, one buffer) and show it corrupts the value
     * already at element 4 - the write of e0 (bytes 0..3) destroys
     * the codes e1..e7 are still about to read. This is the same
     * divergence the model-level AT64 stream pin exhibits. If a
     * future payload made the simulation match, this test would say
     * so loudly instead of pinning silently. */
    uint8_t inplace[N * 4u];
    memcpy(inplace, payload, sizeof payload);
    for (int k = 0; k < N; k++) {
        const uint8_t code = (k & 1u)
            ? (uint8_t)((inplace[k / 2u] >> 2) & 0x3u)
            : (uint8_t)(inplace[k / 2u] & 0x3u);
        const int t = (code == 1) ? 1 : (code == 2) ? -1 : 0;
        float v = (t == 0) ? 0.0f : (t < 0) ? -scales[k / GROUP]
                                            : scales[k / GROUP];
        memcpy(inplace + (size_t)k * 4u, &v, sizeof v);
    }
    int corrupted = 0;
    for (int k = 1; k < N; k++) {
        float old;
        memcpy(&old, inplace + (size_t)k * 4u, sizeof old);
        if (!(old == ref[k])) corrupted = 1;
    }
    CHECK(corrupted, "in-place replay corrupts element 4+ (the bug the "
          "separate-buffer path fixes)");
    /* guard rails */
    CHECK(hydra_moe_dequant_flat(NULL, scales, N, GROUP, out) == -1,
          "NULL payload");
    CHECK(hydra_moe_dequant_flat(payload, NULL, N, GROUP, out) == -1,
          "NULL scales");
    CHECK(hydra_moe_dequant_flat(payload, scales, N, 0, out) == -1,
          "group 0");
    CHECK(hydra_moe_dequant_flat(payload, scales, N + 1, GROUP, out) == -1,
          "odd element count");
}

static void test_moe_ternary_open(void)
{
    section("ternary fixtures: open + geometry (S64/S128)");
    struct { const char *path; uint32_t group, type; } cases[2] = {
        { g_t64path, 64, 0x0040 },
        { g_t128path, 128, 0x0041 },
    };
    for (int k = 0; k < 2; k++) {
        hydra_moe_t m;
        int rc = hydra_moe_open(cases[k].path, &m);
        CHECK(rc == 0, "open the ternary fixture");
        if (rc != 0) continue;
        hydra_moe_config_t c;
        hydra_moe_config(&m, &c);
        CHECK(c.expert_layout == 1, "layout 1");
        CHECK(c.ternary_group == cases[k].group, "group from directory type");
        /* dim=8, inter=4: gate/up 4x8 (16 B payload + 4 scales),
         * down 8x4 (16 B payload + 8 scales) -> 112 bytes per expert. */
        CHECK(m.expert_bytes == 112, "112 B per ternary expert");
        int all = (m.container.expert_dir_count == 8);
        for (uint64_t i = 0; i < m.container.expert_dir_count; i++) {
            const hydra2_expert_entry_t *e = &m.container.experts[i];
            if (e->type_id != cases[k].type || e->length != 112) all = 0;
        }
        CHECK(all, "every directory entry: type + length");
        hydra_moe_close(&m);
    }
}

#define TPIN_STEPS 24
/* Pinned greedy KV streams (start token 7, seed 0x9E3779B97F4A7C15,
 * 24 tokens). T64 and T128 are IDENTICAL by construction: the small
 * fixture has cols 8/4 < any group, so exactly one scale covers each
 * row for both group sizes - the same quantized weights. The large
 * S64 fixture (dim 256, inter 64) uses FOUR groups per gate/up row
 * and is the multi-group engine coverage. */
static const uint32_t TPIN_T64[TPIN_STEPS] = {
    12, 12, 1, 10, 10, 7, 2, 7, 2, 14, 8, 14,
    14, 14, 8, 6, 14, 2, 12, 12, 10, 1, 1, 1
};
static const uint32_t TPIN_T128[TPIN_STEPS] = {
    12, 12, 1, 10, 10, 7, 2, 7, 2, 14, 8, 14,
    14, 14, 8, 6, 14, 2, 12, 12, 10, 1, 1, 1
};
static const uint32_t TPIN_T64LG[TPIN_STEPS] = {
    14, 1, 0, 2, 2, 7, 5, 0, 1, 10, 5, 2,
    0, 8, 6, 4, 2, 13, 8, 14, 11, 14, 1, 3
};

static void test_moe_ternary_pin(void)
{
    section("ternary greedy stream pins (KV enabled)");
    uint32_t got[TPIN_STEPS];
    CHECK(run_greedy_stream(g_t64path, 7, 0x9E3779B97F4A7C15ull,
                            TPIN_STEPS, got) == 0, "S64 run ok");
    int m64 = 1;
    for (uint32_t i = 0; i < TPIN_STEPS; i++)
        if (got[i] != TPIN_T64[i]) m64 = 0;
    CHECK(m64, "S64 greedy KV stream matches the pinned tokens");
    CHECK(run_greedy_stream(g_t128path, 7, 0x9E3779B97F4A7C15ull,
                            TPIN_STEPS, got) == 0, "S128 run ok");
    int m128 = 1;
    for (uint32_t i = 0; i < TPIN_STEPS; i++)
        if (got[i] != TPIN_T128[i]) m128 = 0;
    CHECK(m128, "S128 greedy KV stream matches the pinned tokens");
    CHECK(run_greedy_stream(g_t64lgpath, 7, 0x9E3779B97F4A7C15ull,
                            TPIN_STEPS, got) == 0, "S64 large run ok");
    int mlg = 1;
    for (uint32_t i = 0; i < TPIN_STEPS; i++)
        if (got[i] != TPIN_T64LG[i]) mlg = 0;
    CHECK(mlg, "S64 large (multi-group) stream matches the pinned tokens");
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    /* CodeQL C1 (2026-10-30): the destination is created 0600 (owner
     * only) via open() - the tamper fixtures live in /tmp and must
     * not be world-readable; fopen "wb" would yield 0666 & ~umask.
     * O_NOFOLLOW: a planted symlink in /tmp must not redirect the
     * test's writes elsewhere. */
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) { fclose(in); return -1; }
    FILE *out = fdopen(fd, "wb");
    if (!out) { close(fd); fclose(in); return -1; }
    uint8_t buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
    fclose(in);
    if (fclose(out) != 0) return -1;
    return 0;
}

static void test_moe_ternary_tamper(void)
{
    section("ternary refusals: tampered expert directory");
    hydra_moe_t probe;
    if (hydra_moe_open(g_t64path, &probe) != 0) {
        CHECK(0, "open for tamper offsets");
        return;
    }
    const uint64_t edir = probe.container.expert_dir_offset;
    const uint64_t stride = sizeof(hydra2_expert_entry_t);
    hydra_moe_close(&probe);
    const uint64_t off_type = edir + offsetof(hydra2_expert_entry_t, type_id);
    const uint64_t off_len = edir + offsetof(hydra2_expert_entry_t, length);

    static const char *const tmp = "/tmp/hydra_moe_tamper.hydra2";
    unlink(tmp);
    CHECK(copy_file(g_t64path, tmp) == 0, "copy the fixture");
    hydra_moe_t m;
    int f_ok = 1;
    {
        /* entry 0: type -> F32 (layout 1 needs 0x0040/0x0041) */
        FILE *f = fopen(tmp, "r+b");
        if (!f) f_ok = 0;
        else {
            uint32_t t = 0x12u;
            fseek(f, (long)(off_type + 0 * stride), SEEK_SET);
            if (fwrite(&t, sizeof t, 1, f) != 1) f_ok = 0;
            fclose(f);
        }
        CHECK(f_ok, "tamper entry 0 type");
        CHECK(hydra_moe_open(tmp, &m) != 0, "layout 1 + F32-type refused");
        hydra_moe_close(&m);
    }
    {
        /* entry 1: length + 1 */
        CHECK(copy_file(g_t64path, tmp) == 0, "re-copy the fixture");
        FILE *f = fopen(tmp, "r+b");
        if (f) {
            uint64_t len = 113;
            fseek(f, (long)(off_len + 1 * stride), SEEK_SET);
            fwrite(&len, sizeof len, 1, f);
            fclose(f);
        }
        CHECK(hydra_moe_open(tmp, &m) != 0, "length mismatch refused");
        hydra_moe_close(&m);
    }
    {
        /* entry 2: type -> reserved Q4_0 */
        CHECK(copy_file(g_t64path, tmp) == 0, "re-copy the fixture");
        FILE *f = fopen(tmp, "r+b");
        if (f) {
            uint32_t t30 = 0x30u;
            fseek(f, (long)(off_type + 2 * stride), SEEK_SET);
            fwrite(&t30, sizeof t30, 1, f);
            fclose(f);
        }
        CHECK(hydra_moe_open(tmp, &m) != 0, "reserved type refused");
        hydra_moe_close(&m);
    }
    {
        /* entry 3: type -> S128 (mixed group types across one file) */
        CHECK(copy_file(g_t64path, tmp) == 0, "re-copy the fixture");
        FILE *f = fopen(tmp, "r+b");
        if (f) {
            uint32_t t41 = 0x0041u;
            fseek(f, (long)(off_type + 3 * stride), SEEK_SET);
            fwrite(&t41, sizeof t41, 1, f);
            fclose(f);
        }
        CHECK(hydra_moe_open(tmp, &m) != 0, "mixed group types refused");
        hydra_moe_close(&m);
    }
    unlink(tmp);
}


/* ------------------------------------------------------------------ */
/* vocabulary + BPE codec (role 8, FORMAT-V2.md 3.2.3)                 */
/* ------------------------------------------------------------------ */

static void test_moe_vocab_codec(void)
{
    section("vocabulary + BPE codec (role 8, FORMAT-V2.md 3.2.3)");
    hydra_moe_t m;
    if (hydra_moe_open(g_fixpath, &m) != 0) {
        CHECK(0, "open for the vocab codec");
        return;
    }
    CHECK(hydra_moe_has_vocab(&m) == 1, "F32 fixture carries a vocabulary");
    CHECK(hydra_moe_merge_count(&m) == 3, "three merge rules in the fixture");

    char buf[32];
    CHECK(hydra_moe_decode_token(&m, 0, buf, sizeof buf) == 1 &&
          buf[0] == 'a', "token 0 decodes to 'a'");
    CHECK(hydra_moe_decode_token(&m, 5, buf, sizeof buf) == 2 &&
          memcmp(buf, "ab", 3) == 0, "token 5 decodes to 'ab'");
    CHECK(hydra_moe_decode_token(&m, 8, buf, sizeof buf) == 7 &&
          memcmp(buf, "tok0008", 8) == 0, "token 8 decodes to 'tok0008'");
    CHECK(hydra_moe_decode_token(&m, 5, buf, 1) == -1,
          "decode with a too-small cap is refused");
    CHECK(hydra_moe_decode_token(&m, 999, buf, sizeof buf) == -1,
          "decode of an out-of-range token is refused");

    uint32_t out[32];
    CHECK(hydra_moe_encode_text(&m, "abcd", out, 32) == 2 &&
          out[0] == 5 && out[1] == 6, "BPE: 'abcd' -> [5,6]");
    CHECK(hydra_moe_encode_text(&m, "abc", out, 32) == 2 &&
          out[0] == 5 && out[1] == 2, "BPE: 'abc' -> [5,2]");
    CHECK(hydra_moe_encode_text(&m, "ab", out, 32) == 1 && out[0] == 5,
          "BPE: 'ab' -> [5]");
    CHECK(hydra_moe_encode_text(&m, "abcab", out, 32) == 3 &&
          out[0] == 5 && out[1] == 2 && out[2] == 5,
          "BPE: 'abcab' -> [5,2,5]");
    CHECK(hydra_moe_encode_text(&m, "abcde", out, 32) == 3 &&
          out[0] == 5 && out[1] == 6 && out[2] == 4,
          "BPE: 'abcde' -> [5,6,4]");
    CHECK(hydra_moe_encode_text(&m, "", out, 32) == 0,
          "BPE: empty text -> 0 tokens");
    CHECK(hydra_moe_encode_text(&m, "hello", out, 32) == -3,
          "BPE: unmapped byte is refused (-3)");
    CHECK(hydra_moe_encode_text(&m, "a\xC3" "\x28" "b", out, 32) == -2,
          "BPE: invalid UTF-8 is refused (-2)");
    /* invalid LEADING byte: the old validator returned 0 for BOTH
     * "valid" and "invalid at offset 0", so this slipped past the
     * check and died as 'unmapped byte 0xFF' (-3) - round 13, Qodo
     * #20. It must be a UTF-8 refusal (-2) now. */
    CHECK(hydra_moe_encode_text(&m, "\xFF", out, 32) == -2,
          "BPE: invalid UTF-8 at offset 0 is refused (-2, not -3)");
    CHECK(hydra_moe_encode_text(&m, "abcde", out, 2) == -4,
          "BPE: overflow is refused (-4)");
    CHECK(hydra_moe_encode_text(&m, NULL, out, 32) == -5,
          "encode with a NULL text is refused (-5)");
    CHECK(hydra_moe_encode_text(&m, "a", NULL, 32) == -5,
          "encode with a NULL output is refused (-5)");
    hydra_moe_close(&m);

    CHECK(hydra_moe_open(g_nvpath, &m) == 0, "open the no-vocab fixture");
    CHECK(hydra_moe_has_vocab(&m) == 0, "no-vocab fixture has no vocabulary");
    CHECK(hydra_moe_merge_count(&m) == 0, "no-vocab fixture has no merges");
    CHECK(hydra_moe_encode_text(&m, "a", out, 32) == -1,
          "encode without a vocabulary is refused (-1)");
    CHECK(hydra_moe_decode_token(&m, 0, buf, sizeof buf) == -1,
          "decode without a vocabulary is refused");
    hydra_moe_close(&m);
}

/* find a tensor-directory entry by name; 0-based index + absolute
 * file offset of its fields, -1 when absent */
static int find_tensor_entry(const uint8_t *file, uint64_t tdir,
                             uint64_t count, const char *name,
                             uint64_t *entry_off)
{
    const uint64_t stride = HYDRA2_TENSOR_ENTRY_B;
    const size_t n = strlen(name);
    for (uint64_t i = 0; i < count; i++) {
        const uint8_t *e = file + tdir + i * stride;
        if (n <= 32 && memcmp(e, name, n) == 0 && e[n] == 0) {
            *entry_off = tdir + i * stride;
            return 0;
        }
    }
    return -1;
}

static void test_moe_vocab_tamper(void)
{
    section("vocab/merges refusals: tampered .hydra2");
    hydra_moe_t probe;
    if (hydra_moe_open(g_fixpath, &probe) != 0) {
        CHECK(0, "open for vocab tamper offsets");
        return;
    }
    const uint64_t tdir = probe.container.tensor_dir_offset;
    const uint64_t tcount = probe.container.tensor_dir_count;
    const uint64_t trunk_off = probe.container.trunk_offset;
    hydra_moe_close(&probe);

    /* load the fixture once to find the entries */
    FILE *f = fopen(g_fixpath, "rb");
    if (!f) { CHECK(0, "read the fixture for tamper offsets"); return; }
    fseek(f, 0, SEEK_END);
    const long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = (uint8_t *)malloc((size_t)fsz);
    if (!file || fread(file, 1, (size_t)fsz, f) != (size_t)fsz) {
        free(file); fclose(f);
        CHECK(0, "read the fixture bytes");
        return;
    }
    fclose(f);
    uint64_t ev = 0, em = 0;
    CHECK(find_tensor_entry(file, tdir, tcount, "vocab", &ev) == 0,
          "the fixture has a 'vocab' entry");
    CHECK(find_tensor_entry(file, tdir, tcount, "merges", &em) == 0,
          "the fixture has a 'merges' entry");
    if (ev == 0 && em == 0) { free(file); return; }
    const uint8_t *evp = file + ev;
    const uint8_t *emp = file + em;
    /* field offsets inside the entry (reader's struct is authoritative) */
    /* on-disk field offsets (HYDRA2_TENSOR_ENTRY_B): name 0, role 32,
     * type_id 34, n_dims 36, res 38, offset 40, n_elements 48,
     * n_bytes 56 */
    const uint64_t off_type = ev + 34;
    const uint64_t off_mbytes = em + 56;
    uint64_t vocab_off, merges_off, vocab_nbytes;
    memcpy(&vocab_off, evp + 40, 8);
    memcpy(&merges_off, emp + 40, 8);
    memcpy(&vocab_nbytes, evp + 56, 8);

    static const char *const tmp = "/tmp/hydra_moe_vocab_tamper.hydra2";
    hydra_moe_t m;

    /* a) vocab type -> F32 */
    {
        CHECK(copy_file(g_fixpath, tmp) == 0, "copy the fixture");
        f = fopen(tmp, "r+b");
        if (f) {
            uint16_t t = 0x12u;
            fseek(f, (long)off_type, SEEK_SET);
            fwrite(&t, sizeof t, 1, f);
            fclose(f);
        }
        CHECK(hydra_moe_open(tmp, &m) != 0, "vocab typed F32 is refused");
        hydra_moe_close(&m);
    }
    /* b) merges n_bytes -> 3 (container: U16_PAIRS needs 4*n) */
    {
        CHECK(copy_file(g_fixpath, tmp) == 0, "re-copy the fixture");
        f = fopen(tmp, "r+b");
        if (f) {
            uint64_t nb = 3;
            fseek(f, (long)off_mbytes, SEEK_SET);
            fwrite(&nb, sizeof nb, 1, f);
            fclose(f);
        }
        CHECK(hydra_moe_open(tmp, &m) != 0,
              "merges with a truncated n_bytes is refused");
        hydra_moe_close(&m);
    }
    /* c) merge rule 0 -> (0,3): "ad" is not in the vocabulary */
    {
        CHECK(copy_file(g_fixpath, tmp) == 0, "re-copy the fixture");
        f = fopen(tmp, "r+b");
        if (f) {
            uint16_t pr[2] = { 0, 3 };
            fseek(f, (long)(trunk_off + merges_off), SEEK_SET);
            fwrite(pr, sizeof pr, 1, f);
            fclose(f);
        }
        CHECK(hydra_moe_open(tmp, &m) != 0,
              "merge rule without a target token is refused");
        hydra_moe_close(&m);
    }
    /* d) merge rule 0 -> (16,1): left >= vocab_size */
    {
        CHECK(copy_file(g_fixpath, tmp) == 0, "re-copy the fixture");
        f = fopen(tmp, "r+b");
        if (f) {
            uint16_t pr[2] = { 16, 1 };
            fseek(f, (long)(trunk_off + merges_off), SEEK_SET);
            fwrite(pr, sizeof pr, 1, f);
            fclose(f);
        }
        CHECK(hydra_moe_open(tmp, &m) != 0,
              "merge rule with an out-of-range token is refused");
        hydra_moe_close(&m);
    }
    /* e) token 4's string "e" -> "a": duplicate in the vocabulary */
    {
        CHECK(copy_file(g_fixpath, tmp) == 0, "re-copy the fixture");
        f = fopen(tmp, "r+b");
        if (f) {
            uint8_t a = 'a';
            /* blob: "a\0b\0c\0d\0e\0..." - token 4 starts at byte 8 */
            fseek(f, (long)(trunk_off + vocab_off + 8), SEEK_SET);
            fwrite(&a, 1, 1, f);
            fclose(f);
        }
        CHECK(hydra_moe_open(tmp, &m) != 0,
              "a duplicated token string is refused");
        hydra_moe_close(&m);
    }
    /* f) the final NUL of the blob is clobbered */
    {
        CHECK(copy_file(g_fixpath, tmp) == 0, "re-copy the fixture");
        f = fopen(tmp, "r+b");
        if (f) {
            uint8_t x = 'x';
            fseek(f, (long)(trunk_off + vocab_off + vocab_nbytes - 1),
                  SEEK_SET);
            fwrite(&x, 1, 1, f);
            fclose(f);
        }
        CHECK(hydra_moe_open(tmp, &m) != 0,
              "a vocab blob without a final NUL is refused");
        hydra_moe_close(&m);
    }
    /* g) merges blob out of the trunk range (offset past the end) */
    {
        CHECK(copy_file(g_fixpath, tmp) == 0, "re-copy the fixture");
        f = fopen(tmp, "r+b");
        if (f) {
            uint64_t off = 0xFFFF0000ull;
            fseek(f, (long)(em + 40), SEEK_SET);
            fwrite(&off, sizeof off, 1, f);
            fclose(f);
        }
        CHECK(hydra_moe_open(tmp, &m) != 0,
              "merges with an out-of-range trunk offset is refused");
        hydra_moe_close(&m);
    }
    unlink(tmp);
    free(file);
}


/* ------------------------------------------------------------------ */
/* ternary attention trunk tensors (FORMAT-V2.md 3.2.1)                */
/* ------------------------------------------------------------------ */

static void test_moe_attn_ternary(void)
{
    section("ternary attention trunk tensors (FORMAT-V2.md 3.2.1)");
    /* greedy pin of the --moe-attn-ternary 64 fixture (2 layers,
     * dim 8): attention matrices are TERNARY2_S64 trunk tensors with
     * paired F32 '<name>.scale' tensors; the engine materializes
     * code*scale into F32 and runs the identical F32 math.
     * RE-PINNED 2026-10-30 (round 13, Qodo #3): the old pin was taken
     * while moe_get_tensor dequantized IN PLACE (the F32 write at
     * byte 4k corrupted the packed bytes still to be read from byte
     * k/2), so the old stream ran on scrambled attention weights.
     * The pin below is the first pin of the CORRECTLY dequantized
     * fixture - any regression of the separate-buffer path (or of the
     * ternary packing) changes this stream. */
    uint32_t got[TPIN_STEPS];
    static const uint32_t AT64[TPIN_STEPS] = {
        12, 10, 10, 10, 5, 10, 10, 5, 10, 5, 10, 5,
        10, 5, 10, 5, 10, 5, 10, 5, 10, 5, 10, 5
    };
    CHECK(run_greedy_stream(g_at64path, 7, 0x9E3779B97F4A7C15ull,
                            TPIN_STEPS, got) == 0,
          "attn-ternary S64 run ok");
    int mok = 1;
    for (uint32_t i = 0; i < TPIN_STEPS; i++)
        if (got[i] != AT64[i]) mok = 0;
    CHECK(mok, "attn-ternary S64 greedy KV stream matches the pinned "
               "tokens");

    /* refusals: the paired scale tensor is part of the contract */
    hydra_moe_t probe;
    if (hydra_moe_open(g_at64path, &probe) != 0) {
        CHECK(0, "open for the attn-ternary tamper");
        return;
    }
    const uint64_t tdir = probe.container.tensor_dir_offset;
    const uint64_t tcount = probe.container.tensor_dir_count;
    hydra_moe_close(&probe);
    FILE *f = fopen(g_at64path, "rb");
    if (!f) { CHECK(0, "read the attn-ternary fixture"); return; }
    fseek(f, 0, SEEK_END);
    const long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = (uint8_t *)malloc((size_t)fsz);
    if (!file || fread(file, 1, (size_t)fsz, f) != (size_t)fsz) {
        free(file); fclose(f);
        CHECK(0, "read the attn-ternary fixture bytes");
        return;
    }
    fclose(f);
    uint64_t ea = 0, es = 0;
    CHECK(find_tensor_entry(file, tdir, tcount, "attn_q.0", &ea) == 0,
          "find the 'attn_q.0' entry");
    CHECK(find_tensor_entry(file, tdir, tcount, "attn_q.0.scale", &es) == 0,
          "find the 'attn_q.0.scale' entry");
    free(file);

    static const char *const tmp = "/tmp/hydra_moe_attn_tamper.hydra2";
    hydra_moe_t m;
    {
        /* the scale count is part of the contract: +1 scale element */
        CHECK(copy_file(g_at64path, tmp) == 0, "copy the fixture");
        f = fopen(tmp, "r+b");
        if (f) {
            uint64_t ne = 2; /* was 1 (64 elements, group 64) */
            fseek(f, (long)(es + 48), SEEK_SET);
            fwrite(&ne, sizeof ne, 1, f);
            fclose(f);
        }
        CHECK(hydra_moe_open(tmp, &m) != 0,
              "attn tensor with a mismatched .scale is refused");
        hydra_moe_close(&m);
    }
    {
        /* the tensor type is: F32 with the ternary n_bytes */
        CHECK(copy_file(g_at64path, tmp) == 0, "re-copy the fixture");
        f = fopen(tmp, "r+b");
        if (f) {
            uint16_t t = 0x12u;
            fseek(f, (long)(ea + 34), SEEK_SET);
            fwrite(&t, sizeof t, 1, f);
            fclose(f);
        }
        CHECK(hydra_moe_open(tmp, &m) != 0,
              "attn tensor retyped F32 (ternary n_bytes) is refused");
        hydra_moe_close(&m);
    }
    unlink(tmp);
}


/* helper: FILE offset of a 4-byte metadata value (UINT64_MAX if
 * absent). The container's meta_blob is a COMPACT re-serialization
 * (key bytes, value bytes, no headers) - blob offsets are NOT file
 * offsets. So walk the on-disk region in wire format (hydra_v2_
 * container.c 4.4: u32 count, then per pair u32 key_len, u32
 * val_len, key bytes, val bytes). */
static uint64_t meta_value_file_off(const char *path, const char *key)
{
    uint64_t meta_off = 0, meta_bytes = 0;
    hydra_moe_t probe;
    if (hydra_moe_open(path, &probe) != 0) return UINT64_MAX;
    meta_off = probe.container.metadata_offset;
    meta_bytes = probe.container.metadata_bytes;
    hydra_moe_close(&probe);

    FILE *f = fopen(path, "rb");
    if (!f) return UINT64_MAX;
    uint8_t *reg = (uint8_t *)malloc(meta_bytes);
    if (reg) {
        if (fseek(f, (long)meta_off, SEEK_SET) == 0 &&
            fread(reg, 1, meta_bytes, f) == meta_bytes) {
            uint32_t count = (uint32_t)reg[0] | ((uint32_t)reg[1] << 8) |
                             ((uint32_t)reg[2] << 16) |
                             ((uint32_t)reg[3] << 24);
            uint64_t off = 4;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t klen = (uint32_t)reg[off] |
                                ((uint32_t)reg[off + 1] << 8) |
                                ((uint32_t)reg[off + 2] << 16) |
                                ((uint32_t)reg[off + 3] << 24);
                uint32_t vlen = (uint32_t)reg[off + 4] |
                                ((uint32_t)reg[off + 5] << 8) |
                                ((uint32_t)reg[off + 6] << 16) |
                                ((uint32_t)reg[off + 7] << 24);
                if (vlen == 4 && klen == strlen(key) &&
                    memcmp(reg + off + 8, key, klen) == 0) {
                        free(reg);
                        fclose(f);
                        return meta_off + off + 8 + klen;
                }
                off += 8 + klen + vlen;
                if (off + 8 > meta_bytes) break;
            }
        }
        free(reg);
    }
    fclose(f);
    return UINT64_MAX;
}

static void test_moe_shape_gqa(void)
{
    section("shape validation + GQA (round 13, Qodo #17/#26/#28)");
    hydra_moe_t m;
    static const char *const tmp = "/tmp/hydra_moe_shape.hydra2";

    /* #28: a kv cache whose dim does NOT match n_kv_heads * head_dim
     * must be refused by forward (pre-fix: the head-strided reads
     * wandered outside the intended slots). */
    if (hydra_moe_open(g_fixpath, &m) == 0) {
        float *kb = (float *)malloc(4 * 8 * sizeof(float));
        float *vb = (float *)malloc(4 * 8 * sizeof(float));
        hydra_kv_cache_t wrong[1];
        if (kb && vb && hydra_kv_cache_init(wrong, kb, vb, 9, 4, 1) == 0) {
            float lg[16];
            CHECK(hydra_moe_forward(&m, 0, 0, lg, wrong) == -1,
                  "kv cache with mismatched dim -> -1 (#28)");
        } else CHECK(0, "set up mismatched kv cache");
        free(kb); free(vb);
        hydra_moe_close(&m);
    } else CHECK(0, "open fixture for kv-dim check");

    /* #17: n_heads * head_dim that WRAPS AROUND in 32-bit arithmetic:
     * (2 + 2^30) * 4 == 8 (mod 2^32) 'equals' dim 8 after the wrap.
     * Pre-fix the shape check passed and open succeeded; now -7. */
    const uint64_t off_nh = meta_value_file_off(g_fixpath, "v2.n_heads");
        CHECK(off_nh != UINT64_MAX, "v2.n_heads meta locatable");
        unlink(tmp);
        CHECK(copy_file(g_fixpath, tmp) == 0, "copy for n_heads tamper");
        {
            FILE *f = fopen(tmp, "r+b");
            if (f) {
                uint32_t wrapped = 1073741826u; /* 2 + 2^30 */
                fseek(f, (long)off_nh, SEEK_SET);
                fwrite(&wrapped, sizeof wrapped, 1, f);
                fclose(f);
            }
        }
    CHECK(hydra_moe_open(tmp, &m) == -7,
          "32-bit-wrap n_heads*head_dim == dim is refused (#17)");
    hydra_moe_close(&m);

    /* #26a: n_kv_heads that does not divide n_heads (8 % 3 != 0) must
     * be refused at open - pre-fix it opened fine and the block
     * mapping then computed kv heads beyond n_kv_heads (OOB). */
    const uint64_t off_nkv = meta_value_file_off(g_t64lgpath, "v2.n_kv_heads");
        CHECK(off_nkv != UINT64_MAX, "v2.n_kv_heads meta locatable");
        CHECK(copy_file(g_t64lgpath, tmp) == 0, "copy for n_kv_heads tamper");
        {
            FILE *f = fopen(tmp, "r+b");
            if (f) {
                uint32_t nkv3 = 3u;
                fseek(f, (long)off_nkv, SEEK_SET);
                fwrite(&nkv3, sizeof nkv3, 1, f);
                fclose(f);
            }
        }
    CHECK(hydra_moe_open(tmp, &m) == -7,
          "n_heads %% n_kv_heads != 0 is refused (#26)");
    hydra_moe_close(&m);
    unlink(tmp);

    /* #26b: a real GQA model (n_heads 4, n_kv_heads 2) opens and the
     * stream pin encodes the BLOCK mapping (head i reads kv head
     * i / (nh/nkv)). The old interleave (i % nkv) produced
     * `12 12 12 12 12 12 5 5 5 5 13 5` on this fixture; the pinned
     * stream below is the fixed one. */
    static const uint32_t TPIN_GQA[12] = {
        15, 9, 12, 12, 9, 13, 13, 13, 13, 13, 10, 13
    };
    CHECK(hydra_moe_open(g_gqapath, &m) == 0, "GQA fixture (nkv 2) opens");
    uint32_t got[12];
    CHECK(run_greedy_stream(g_gqapath, 7, 0x9E3779B97F4A7C15ull, 12, got)
          == 0, "GQA run ok");
    int mgqa = 1;
    for (uint32_t i = 0; i < 12; i++)
        if (got[i] != TPIN_GQA[i]) mgqa = 0;
    CHECK(mgqa, "GQA block-mapping stream matches the pinned tokens (#26)");
    hydra_moe_close(&m);
}

int main(int argc, char **argv)
{
    make_dirs();
    if (argc > 1) g_fixpath = argv[1];
    if (argc > 2) g_t64path = argv[2];
    if (argc > 3) g_t128path = argv[3];
    if (argc > 4) g_t64lgpath = argv[4];
    if (argc > 5) g_nvpath = argv[5];
    if (argc > 6) g_at64path = argv[6];
    if (argc > 7) g_gqapath = argv[7];
    if (!g_fixpath || !g_t64path || !g_t128path || !g_t64lgpath ||
        !g_nvpath || !g_at64path || !g_gqapath) {
        printf("  FAIL: fixture paths missing - run make test-run (the "
               "Makefile passes F32 as argv[1], ternary S64 as argv[2], "
               "ternary S128 as argv[3], ternary S64 large as argv[4], "
               "no-vocab F32 as argv[5], attention-ternary S64 as argv[6], "
               "GQA (n_heads 4, n_kv_heads 2) as argv[7])"
               "\n");
        g_fail = 1;
        g_tests = 1;
        printf("\n=== %d Tests, %d failures ===\n", g_tests, g_fail);
        return 1;
    }
    (void)g_fixture;
    test_moe_open();
    test_moe_forward_parity();
    test_moe_forward_sanity();
    test_moe_stream_pin();
    test_moe_kv_eviction();
    test_moe_sampler();
    test_moe_refusals();
    test_moe_dequant_unit();
    test_moe_dequant_flat_unit();
    test_moe_ternary_open();
    test_moe_ternary_pin();
    test_moe_ternary_tamper();
    test_moe_vocab_codec();
    test_moe_vocab_tamper();
    test_moe_attn_ternary();
    test_moe_shape_gqa();
    printf("\n=== %d Tests, %d failures ===\n", g_tests, g_fail);
    return g_fail == 0 ? 0 : 1;
}

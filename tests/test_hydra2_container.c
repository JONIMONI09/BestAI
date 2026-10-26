/* ====================================================================
 * test_hydra2_container.c - tests for the .hydra2 sparse-MoE container.
 *
 * Builds a valid .hydra2 file in C (independent of tools/make_model.py's
 * Python writer - the two are cross-checked in CI), then exercises every
 * validation rule in src/hydra_v2_container.c:
 *
 *   - the CRC-32 implementation against known answers,
 *   - the happy path (open, lookups, trunk + expert streaming reads,
 *     deterministic top-k routing),
 *   - BOTH-ENDS refusals (R14): every region offset checked against its
 *     base AND its end, every count overflow-checked before the multiply,
 *     file_size checked against the real size on disk,
 *   - loud, specific refusals (5.2): a few are checked to name the field,
 *     value, or bound (v1 magic named, architecture echoed, CRC names the
 *     expert),
 *   - the mutual v1/v2 refusal (FORMAT-V2.md 7.2/7.3).
 *
 * Build: see the `hydra2-test` Makefile target.
 * ==================================================================== */
#include "hydra_v2_container.h"
#include "hydra_model.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/* ------------------------------------------------------------------ */
/* tiny test harness                                                  */
/* ------------------------------------------------------------------ */

static int g_tests = 0, g_fail = 0;

/* Non-variadic on purpose: the format string at the printf call must be
 * a compile-time constant (flawfinder CWE-134, the same discipline as
 * test_engine.c). CHECKs that need values format them into a buffer
 * first (the snprintf call next to the CHECK). */
#define CHECK(cond, msg) do { \
    g_tests++; \
    if (!(cond)) { \
        g_fail++; \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
    } \
} while (0)

static void section(const char *name)
{
    printf("== %s\n", name);
}

/* ------------------------------------------------------------------ */
/* LE helpers (memory)                                                */
/* ------------------------------------------------------------------ */

static void put_u16le(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put_u32le(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static void put_u64le(uint8_t *p, uint64_t v) {
    for (int i =0;i<8;i++) p[i]=(uint8_t)(v>>(8*i));
}
static uint64_t pad16(uint64_t x) { return (x + 15ull) & ~15ull; }

/* ------------------------------------------------------------------ */
/* fixture builder: a valid .hydra2 file, all offsets recorded        */
/* ------------------------------------------------------------------ */

#define FX_LAYERS   2u
#define FX_EXPERTS  4u
#define FX_TOPK     2u
#define FX_HIDDEN   8u
#define FX_VOCAB    4u
#define FX_EELEM    16u      /* F32 elements per expert -> 64 bytes   */

typedef struct {
    uint8_t *buf;
    size_t   len;
    /* header field file offsets - FIXED by the spec, never reassigned */
    uint64_t h_vmaj, h_vmin, h_hbytes, h_fsize;
    uint64_t h_meta_off, h_meta_bytes;
    uint64_t h_td_off, h_td_count, h_td_bytes;
    uint64_t h_ed_off, h_ed_count, h_ed_bytes;
    uint64_t h_trunk_off, h_trunk_bytes;
    uint64_t h_exp_off, h_exp_bytes;
    uint64_t h_hcrc, h_resv;
    /* actual region positions and sizes (written into the header) */
    uint64_t meta_pos, meta_len;
    uint64_t td_pos, td_len;
    uint64_t ed_pos, ed_len;
    uint64_t trunk_pos, trunk_len;
    uint64_t exp_pos, exp_len;
    /* metadata */
    uint64_t meta_count_off;
    uint64_t meta_topk_val, meta_epl_val, meta_arch_val;
    uint64_t meta_layers_val, meta_runs_val;
    /* tensor dir: base offset of entry i (each entry is 64 bytes) */
    uint64_t tentry[4];
    /* expert dir: base offset of entry i (each entry is 48 bytes) */
    uint64_t eentry[8];
    /* absolute offset of each expert's payload */
    uint64_t exp_payload[8];
    uint64_t exp_crc0_val;   /* stored crc32 of expert (0,0), for the read test */
} fx;

/* metadata key lengths used below */
static const char *M_ARCH = "v2.architecture";
static const char *M_LAY  = "v2.layers";
static const char *M_MOE  = "v2.moe_layers";
static const char *M_TOPK = "v2.top_k";
static const char *M_EPL  = "v2.experts_per_layer";
static const char *M_RD   = "v2.router_dtype";
static const char *M_ETYP = "v2.expert_bytes_typ";
static const char *M_RUNS = "v2.router_runs_ahead";

static int build_fx(fx *f)
{
    memset(f, 0, sizeof(*f));
    size_t cap = 1 << 16;
    f->buf = (uint8_t *)malloc(cap);
    if (!f->buf) return -1;
    uint64_t pos = 0;

    /* header fields (fixed offsets) */
    f->h_vmaj=8; f->h_vmin=10; f->h_hbytes=12; f->h_fsize=16;
    f->h_meta_off=24; f->h_meta_bytes=32;
    f->h_td_off=40; f->h_td_count=48; f->h_td_bytes=56;
    f->h_ed_off=64; f->h_ed_count=72; f->h_ed_bytes=80;
    f->h_trunk_off=88; f->h_trunk_bytes=96;
    f->h_exp_off=104; f->h_exp_bytes=112;
    f->h_hcrc=120; f->h_resv=124;
    pos = 128;

    /* ---- metadata K/V block ------------------------------------ */
    f->meta_pos = pos;
    f->meta_count_off = pos;
    uint8_t *meta = f->buf + pos;
    uint32_t npairs = 8;
    put_u32le(meta + 0, npairs);
    uint64_t mp = 4;
    /* helper to append one pair, recording the VALUE field offset */
    #define ADDPAIR(key, valbytes, vallen, val_off_out) do { \
        uint32_t kl = (uint32_t)strlen(key); \
        put_u32le(meta + mp, kl); mp += 4; \
        put_u32le(meta + mp, (uint32_t)(vallen)); mp += 4; \
        memcpy(meta + mp, key, kl); mp += kl; \
        val_off_out = pos + mp; \
        memcpy(meta + mp, valbytes, (vallen)); mp += (vallen); \
    } while (0)
    uint32_t u32v[1];
    uint64_t u64v[1];
    uint64_t vo_unused = 0;
    {
        const char *arch = "moe-transformer-v1";
        ADDPAIR(M_ARCH, (const uint8_t *)arch, strlen(arch), f->meta_arch_val);
        put_u32le((uint8_t *)u32v, FX_LAYERS);
        ADDPAIR(M_LAY, (const uint8_t *)u32v, 4, f->meta_layers_val);
        put_u32le((uint8_t *)u32v, FX_LAYERS);
        ADDPAIR(M_MOE, (const uint8_t *)u32v, 4, vo_unused);
        put_u32le((uint8_t *)u32v, FX_TOPK);
        ADDPAIR(M_TOPK, (const uint8_t *)u32v, 4, f->meta_topk_val);
        put_u32le((uint8_t *)u32v, FX_EXPERTS);
        ADDPAIR(M_EPL, (const uint8_t *)u32v, 4, f->meta_epl_val);
        put_u32le((uint8_t *)u32v, 0x12); /* F32 */
        ADDPAIR(M_RD, (const uint8_t *)u32v, 4, vo_unused);
        put_u64le((uint8_t *)u64v, FX_EELEM * 4);
        ADDPAIR(M_ETYP, (const uint8_t *)u64v, 8, vo_unused);
        put_u32le((uint8_t *)u32v, 1);
        ADDPAIR(M_RUNS, (const uint8_t *)u32v, 4, f->meta_runs_val);
    }
    (void)vo_unused; /* pairs without a recorded value offset */
    #undef ADDPAIR
    f->meta_len = mp;
    uint64_t after_meta = pos + mp;
    pos = pad16(after_meta);

    /* ---- tensor directory + trunk payloads --------------------- */
    f->td_pos = pos;
    uint64_t td_base = pos;
    /* trunk payload contents: deterministic F32 pattern */
    /* layout the tensor entries first (we know the trunk layout) */
    uint64_t trunk_offsets[4] = {0, 16, 32, 160};
    uint32_t trunk_nelem[4]   = {FX_EXPERTS, FX_EXPERTS, FX_VOCAB*FX_HIDDEN, FX_HIDDEN};
    uint16_t trunk_role[4]    = {HYDRA2_ROLE_ROUTER, HYDRA2_ROLE_ROUTER,
                                 HYDRA2_ROLE_EMBEDDING, HYDRA2_ROLE_NORM};
    uint16_t trunk_ndims[4]   = {1, 1, 2, 1};
    const char *tname[4]      = {"router.0", "router.1", "embed", "norm"};
    f->td_len = 4 * 64;
    for (uint32_t i = 0; i < 4; i++) {
        f->tentry[i] = td_base + i * 64;
        uint8_t *e = f->buf + f->tentry[i];
        memset(e, 0, 64);
        memcpy(e + 0, tname[i], strlen(tname[i]));
        put_u16le(e + 32, trunk_role[i]);
        put_u16le(e + 34, HYDRA2_F32);
        put_u16le(e + 36, trunk_ndims[i]);
        put_u16le(e + 38, 0);
        put_u64le(e + 40, trunk_offsets[i]);
        put_u64le(e + 48, trunk_nelem[i]);
        put_u64le(e + 56, (uint64_t)trunk_nelem[i] * 4u);
    }
    pos = td_base + f->td_len;

    /* ---- expert directory -------------------------------------- */
    f->ed_pos = pos;
    uint64_t ed_base = pos;
    f->ed_len = (FX_LAYERS * FX_EXPERTS) * 48;
    uint64_t off = 0;
    uint64_t fe = 0;
    for (uint32_t i = 0; i < FX_LAYERS * FX_EXPERTS; i++) {
        uint32_t layer = i / FX_EXPERTS;
        uint32_t eid   = i % FX_EXPERTS;
        f->eentry[i] = ed_base + i * 48;
        uint8_t *e = f->buf + f->eentry[i];
        memset(e, 0, 48);
        put_u32le(e + 0, layer);
        put_u32le(e + 4, eid);
        put_u64le(e + 8, off);
        put_u64le(e + 16, FX_EELEM * 4);
        put_u64le(e + 24, fe);
        put_u32le(e + 32, HYDRA2_F32);
        /* crc filled after the payload exists (below) */
        put_u32le(e + 36, 0);
        put_u64le(e + 40, 0);
        f->exp_payload[i] = 0; /* set once experts region base is known */
        off += FX_EELEM * 4;
        fe  += FX_EELEM;
    }
    pos = ed_base + f->ed_len;

    /* ---- trunk payloads ---------------------------------------- */
    f->trunk_pos = pos;
    f->trunk_len = 192; /* 16+16+128+32 */
    for (uint64_t b = 0; b < f->trunk_len; b++)
        f->buf[pos + b] = (uint8_t)((b * 31 + 7) & 0xFF);
    pos += f->trunk_len;

    /* ---- expert payloads --------------------------------------- */
    f->exp_pos = pos;
    f->exp_len = FX_LAYERS * FX_EXPERTS * (FX_EELEM * 4);
    for (uint32_t i = 0; i < FX_LAYERS * FX_EXPERTS; i++) {
        f->exp_payload[i] = pos + i * (FX_EELEM * 4);
        for (uint64_t b = 0; b < FX_EELEM * 4; b++)
            f->buf[pos + i * (FX_EELEM * 4) + b] =
                (uint8_t)((i * 97 + b * 13 + 5) & 0xFF);
    }
    /* fill the stored CRCs now that the payload bytes exist */
    for (uint32_t i = 0; i < FX_LAYERS * FX_EXPERTS; i++) {
        uint32_t crc = hydra2_crc32(f->buf + f->exp_payload[i], FX_EELEM * 4);
        put_u32le(f->buf + f->eentry[i] + 36, crc);
    }
    f->exp_crc0_val = hydra2_crc32(f->buf + f->exp_payload[0], FX_EELEM * 4);
    pos += f->exp_len;

    /* ---- header ------------------------------------------------ */
    f->len = (size_t)pos;
    memcpy(f->buf + 0, "HYDRA2\0\0", 8);
    put_u16le(f->buf + 8, 2);
    put_u16le(f->buf + 10, 0);
    put_u32le(f->buf + 12, 128);
    put_u64le(f->buf + 16, pos);   /* file_size */
    put_u64le(f->buf + 24, f->meta_pos);
    put_u64le(f->buf + 32, f->meta_len);
    put_u64le(f->buf + 40, f->td_pos);
    put_u64le(f->buf + 48, 4);                 /* tensor count */
    put_u64le(f->buf + 56, f->td_len);
    put_u64le(f->buf + 64, f->ed_pos);
    put_u64le(f->buf + 72, (uint64_t)(FX_LAYERS * FX_EXPERTS)); /* expert count */
    put_u64le(f->buf + 80, f->ed_len);
    put_u64le(f->buf + 88, f->trunk_pos);
    put_u64le(f->buf + 96, f->trunk_len);
    put_u64le(f->buf + 104, f->exp_pos);
    put_u64le(f->buf + 112, f->exp_len);
    put_u32le(f->buf + 120, hydra2_crc32(f->buf, 120));
    put_u32le(f->buf + 124, 0);
    return 0;
}

static void free_fx(fx *f) { if (f->buf) { free(f->buf); f->buf = NULL; } }

static int write_file(const char *path, const uint8_t *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, buf + done, len - done);
        if (n <= 0) { close(fd); return -1; }
        done += (size_t)n;
    }
    close(fd);
    return 0;
}

/* patch helpers on the fixture buffer */
static void p32(fx *f, uint64_t off, uint32_t v) { put_u32le(f->buf + off, v); }
static void p64(fx *f, uint64_t off, uint64_t v) { put_u64le(f->buf + off, v); }
static void p16(fx *f, uint64_t off, uint16_t v) { put_u16le(f->buf + off, v); }

/* ------------------------------------------------------------------ */
/* stderr capture (for the "names the field" checks)                  */
/* ------------------------------------------------------------------ */

static int g_saved_stderr = -1;

static int cap_stderr_begin(const char *path)
{
    fflush(NULL);
    g_saved_stderr = dup(2);
    if (g_saved_stderr < 0) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { close(g_saved_stderr); g_saved_stderr = -1; return -1; }
    dup2(fd, 2);
    close(fd);
    return 0;
}

static void cap_stderr_end(void)
{
    fflush(NULL);
    if (g_saved_stderr >= 0) {
        dup2(g_saved_stderr, 2);
        close(g_saved_stderr);
        g_saved_stderr = -1;
    }
}

static int file_contains(const char *path, const char *needle)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char *b = (char *)malloc(1 << 16);
    if (!b) { close(fd); return 0; }
    size_t n = 0;
    for (;;) {
        if (n >= (1 << 16) - 1) break;
        ssize_t r = read(fd, b + n, (1 << 16) - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    b[n] = '\0';
    int found = (strstr(b, needle) != NULL);
    free(b);
    close(fd);
    return found;
}

/* ------------------------------------------------------------------ */
/* the fixture file is (re)created for every open; this keeps each    */
/* case hermetic and the suite order-independent.                     */
/* ------------------------------------------------------------------ */

#define FIXPATH "/tmp/hydra2_fixture.hydra2"
#define FIXDIR  "/tmp/hydra2_fixture.d"
#define V1PATH  "/tmp/hydra2_v1ref.hydra"
#define SYMLINK "/tmp/hydra2_link.hydra2"
#define CAPPATH "/tmp/hydra2_cap.stderr"
#define CAPFILE "/tmp/hydra2_capfile.hydra2"

static void make_dirs(void)
{
    (void)mkdir(FIXDIR, 0755);
}

static int open_fixture(const char *path)
{
    hydra2_container_t c;
    return hydra2_open(&c, path);
}

/* build + write the pristine fixture; returns 0 on success */
static int fresh_fixture(void)
{
    fx f;
    if (build_fx(&f) != 0) return -1;
    int rc = write_file(FIXPATH, f.buf, f.len);
    free_fx(&f);
    return rc;
}

/* build, apply the mutation fn, write, open, and return the code. */
typedef void (*mutate_fn)(fx *f);
static int open_mutated(mutate_fn fn)
{
    fx f;
    if (build_fx(&f) != 0) return -999;
    if (fn) fn(&f);
    int wr = write_file(FIXPATH, f.buf, f.len);
    free_fx(&f);
    if (wr != 0) return -998;
    return open_fixture(FIXPATH);
}

/* ================================================================== */
/* tests                                                              */
/* ================================================================== */

static void test_crc_known_answers(void)
{
    section("crc32 known answers");
        char mb_412[160];
    snprintf(mb_412, sizeof(mb_412), "empty crc should be 0, got 0x%08X", hydra2_crc32((const uint8_t *)"", 0));
    CHECK(hydra2_crc32((const uint8_t *)"", 0) == 0x00000000u, mb_412);
        char mb_414[160];
    snprintf(mb_414, sizeof(mb_414), "crc32('123456789') should be 0xCBF43926, got 0x%08X", hydra2_crc32((const uint8_t *)"123456789", 9));
    CHECK(hydra2_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u, mb_414);
    /* "abc" -> 0x352441C2 (IEEE) */
        char mb_418[160];
    snprintf(mb_418, sizeof(mb_418), "crc32('abc') should be 0x352441C2, got 0x%08X", hydra2_crc32((const uint8_t *)"abc", 3));
    CHECK(hydra2_crc32((const uint8_t *)"abc", 3) == 0x352441C2u, mb_418);
}

static void test_happy_path(void)
{
    section("happy path: open + lookups + streaming reads + routing");
    CHECK(fresh_fixture() == 0, "could not write the pristine fixture");

    hydra2_container_t c;
    int rc = hydra2_open(&c, FIXPATH);
        char mb_430[160];
    snprintf(mb_430, sizeof(mb_430), "open of a valid fixture failed: %d", rc);
    CHECK(rc == HYDRA2_OK, mb_430);
    if (rc != HYDRA2_OK) return;

    CHECK(c.version_major == 2 && c.version_minor == 0, "version != 2.0");
    CHECK(c.header_bytes == 128, "header_bytes != 128");
    {
        struct stat fst;
        CHECK(stat(FIXPATH, &fst) == 0, "stat fixture");
                char mb_438[160];
        snprintf(mb_438, sizeof(mb_438), "file_size %llu != real size %lld", (unsigned long long)c.file_size, (long long)fst.st_size);
        CHECK(c.file_size == (uint64_t)fst.st_size, mb_438);
    }
        char mb_442[160];
    snprintf(mb_442, sizeof(mb_442), "tensor_dir_count %llu != 4", (unsigned long long)c.tensor_dir_count);
    CHECK(c.tensor_dir_count == 4, mb_442);
        char mb_444[160];
    snprintf(mb_444, sizeof(mb_444), "expert_dir_count %llu != 8", (unsigned long long)c.expert_dir_count);
    CHECK(c.expert_dir_count == 8, mb_444);
        char mb_446[160];
    snprintf(mb_446, sizeof(mb_446), "top_k %u != %u", c.top_k, (unsigned)FX_TOPK);
    CHECK(c.top_k == FX_TOPK, mb_446);
        char mb_447[160];
    snprintf(mb_447, sizeof(mb_447), "epl %u != %u", c.experts_per_layer, (unsigned)FX_EXPERTS);
    CHECK(c.experts_per_layer == FX_EXPERTS, mb_447);
    CHECK(c.layers == FX_LAYERS && c.moe_layers == FX_LAYERS, "layers wrong");
    CHECK(c.router_runs_ahead == 1, "router_runs_ahead != 1");
        char mb_451[160];
    snprintf(mb_451, sizeof(mb_451), "architecture '%s' wrong", c.architecture);
    CHECK(strcmp(c.architecture, "moe-transformer-v1") == 0, mb_451);

    const hydra2_tensor_entry_t *t;
    CHECK(hydra2_find_tensor(&c, "router.0", &t) == HYDRA2_OK, "find router.0");
    CHECK(t && t->role == HYDRA2_ROLE_ROUTER && t->n_bytes == 16,
          "router.0 fields wrong");
    CHECK(hydra2_find_tensor(&c, "does-not-exist", &t) == HYDRA2_E_ARG,
          "find missing tensor should fail");

    const hydra2_expert_entry_t *e;
    CHECK(hydra2_find_expert(&c, 0, 0, &e) == HYDRA2_OK, "find expert (0,0)");
    CHECK(e && e->length == FX_EELEM * 4, "expert (0,0) length wrong");
    CHECK(hydra2_find_expert(&c, 1, 3, &e) == HYDRA2_OK, "find expert (1,3)");
    CHECK(hydra2_find_expert(&c, 9, 0, &e) == HYDRA2_E_ARG,
          "find missing expert should fail");

    /* read the router.0 trunk tensor (16 bytes) and verify the bytes */
    uint8_t tbuf[32] = {0};
    CHECK(hydra2_read_tensor(&c, t, tbuf, sizeof(tbuf)) == HYDRA2_OK,
          "read_tensor router.0");
        char mb_472[160];
    snprintf(mb_472, sizeof(mb_472), "trunk payload bytes wrong (got %02X %02X)", tbuf[0], tbuf[15]);
    CHECK(tbuf[0] == 7 && tbuf[15] == (uint8_t)((15 * 31 + 7) & 0xFF), mb_472);
    /* too-small buffer */
        char mb_475[160];
    snprintf(mb_475, sizeof(mb_475), "read_tensor with cap 8 should be E_CAP, got %d", hydra2_read_tensor(&c, t, tbuf, 8));
    CHECK(hydra2_read_tensor(&c, t, tbuf, 8) == HYDRA2_E_CAP, mb_475);

    /* stream expert (0,0): 64 bytes, CRC must pass on read */
    const hydra2_expert_entry_t *e0;
    CHECK(hydra2_find_expert(&c, 0, 0, &e0) == HYDRA2_OK, "re-find (0,0)");
    uint8_t ebuf[128] = {0};
    CHECK(hydra2_read_expert(&c, e0, ebuf, sizeof(ebuf)) == HYDRA2_OK,
          "read_expert (0,0)");
    /* payload[0] byte pattern: (i*97 + b*13 + 5), i=0 -> b*13+5 */
        char mb_486[160];
    snprintf(mb_486, sizeof(mb_486), "expert payload bytes wrong (got %02X %02X %02X)", ebuf[0], ebuf[1], ebuf[63]);
    CHECK(ebuf[0] == 5 && ebuf[1] == 18 && ebuf[63] == (uint8_t)((63 * 13 + 5) & 0xFF), mb_486);
    CHECK(hydra2_crc32(ebuf, 64) == e0->crc32,
          "streamed expert CRC disagrees with the directory");
    CHECK(hydra2_read_expert(&c, e0, ebuf, 32) == HYDRA2_E_CAP,
          "read_expert with cap 32 should be E_CAP");

    /* deterministic top-k routing over 4 router logits, top_k = 2 */
    float logits[4] = {0.5f, 3.0f, 1.25f, -2.0f};
    uint32_t sel[2]; float w[2];
    rc = hydra2_route_top_k(logits, 4, 2, sel, w);
        char mb_498[160];
    snprintf(mb_498, sizeof(mb_498), "route_top_k failed: %d", rc);
    CHECK(rc == HYDRA2_OK, mb_498);
    if (rc == HYDRA2_OK) {
                char mb_500[160];
        snprintf(mb_500, sizeof(mb_500), "top-2 should be experts {1,2}, got {%u,%u}", sel[0], sel[1]);
        CHECK(sel[0] == 1 && sel[1] == 2, mb_500);
        float sum = w[0] + w[1];
                char mb_503[160];
        snprintf(mb_503, sizeof(mb_503), "weights sum %f != 1", sum);
        CHECK(fabsf(sum - 1.0f) < 1e-6f, mb_503);
        CHECK(w[0] > w[1], "weight of the larger logit should be bigger");
    }

    /* determinism: 100 repeated routings must be bit-identical */
    uint32_t ref_s[2]; float ref_w[2];
    hydra2_route_top_k(logits, 4, 2, ref_s, ref_w);
    int same = 1;
    for (int i = 0; i < 100; i++) {
        uint32_t si[2]; float wi[2];
        hydra2_route_top_k(logits, 4, 2, si, wi);
        if (si[0] != ref_s[0] || si[1] != ref_s[1] ||
            memcmp(wi, ref_w, sizeof(ref_w)) != 0)
            same = 0;
    }
    CHECK(same, "routing was not deterministic across 100 runs");

    /* tie-break: two equal maxima -> the LOWER expert id comes first */
    float tie[4] = {2.0f, 1.0f, 2.0f, 0.0f};
    uint32_t ts[2]; float tw[2];
    CHECK(hydra2_route_top_k(tie, 4, 2, ts, tw) == HYDRA2_OK, "tie route");
        char mb_524[160];
    snprintf(mb_524, sizeof(mb_524), "tie should pick {0,2} (lower id first), got {%u,%u}", ts[0], ts[1]);
    CHECK(ts[0] == 0 && ts[1] == 2, mb_524);

    /* top_k == n_experts selects all */
    float all4[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    uint32_t asel[4]; float aw[4];
    CHECK(hydra2_route_top_k(all4, 4, 4, asel, aw) == HYDRA2_OK, "top_k==n");
    CHECK(asel[0] == 3 && asel[3] == 0, "top_k==n order wrong");

    /* rejections: NaN, Inf, top_k out of range */
    float nanv[2] = {NAN, 1.0f};
    CHECK(hydra2_route_top_k(nanv, 2, 1, sel, w) == HYDRA2_E_ARG,
          "NaN logit should be refused");
    float infv[2] = {INFINITY, 1.0f};
    CHECK(hydra2_route_top_k(infv, 2, 1, sel, w) == HYDRA2_E_ARG,
          "Inf logit should be refused");
    float ok2[2] = {1.0f, 2.0f};
    CHECK(hydra2_route_top_k(ok2, 2, 0, sel, w) == HYDRA2_E_ARG, "top_k=0");
    CHECK(hydra2_route_top_k(ok2, 2, 3, sel, w) == HYDRA2_E_ARG, "top_k>n");
    CHECK(hydra2_route_top_k(NULL, 2, 1, sel, w) == HYDRA2_E_ARG, "NULL logits");

    hydra2_close(&c);
}

/* ---- mutations ---------------------------------------------------- */

static void mut_v1magic(fx *f)   { memcpy(f->buf, "HYDR\0\0\0\0", 8); }
static void mut_garbage_magic(fx *f) { f->buf[0] = 'X'; }
static void mut_version_major(fx *f) { p16(f, f->h_vmaj, 3); }
static void mut_version_minor(fx *f) { p16(f, f->h_vmin, 1); }
static void mut_header_bytes(fx *f)  { p32(f, f->h_hbytes, 256); }
static void mut_reserved(fx *f)      { p32(f, f->h_resv, 1); }
static void mut_fsize_larger(fx *f)  { p64(f, f->h_fsize, f->len + 64); }
static void mut_fsize_smaller(fx *f) { p64(f, f->h_fsize, 120); }
static void mut_meta_off_low(fx *f)  { p64(f, f->h_meta_off, 112); }
static void mut_meta_off_unaligned(fx *f) { p64(f, f->h_meta_off, f->meta_pos + 8); }
static void mut_meta_bytes_zero(fx *f) { p64(f, f->h_meta_bytes, 0); }
static void mut_meta_overrun(fx *f) { p64(f, f->h_meta_bytes, f->meta_len + 0x1000); }
static void mut_hcrc_bad(fx *f)    { p32(f, f->h_hcrc, 0xDEADBEEF); }
static void mut_topk_zero(fx *f)   { p32(f, f->meta_topk_val, 0); }
static void mut_topk_65(fx *f)     { p32(f, f->meta_topk_val, 65); }
static void mut_epl_lt_topk(fx *f) { p32(f, f->meta_epl_val, 1); }
static void mut_arch_bad(fx *f)    {
    /* overwrite the architecture value bytes (keep the length) */
    memcpy(f->buf + f->meta_arch_val, "not-a-real-arch-xx", 18);
}
static void mut_layers_zero(fx *f) { p32(f, f->meta_layers_val, 0); }
static void mut_runs_two(fx *f)    { p32(f, f->meta_runs_val, 2); }
static void mut_td_count_zero(fx *f)   { p64(f, f->h_td_count, 0); }
static void mut_td_count_huge(fx *f)   { p64(f, f->h_td_count, 0xFFFFFFFFull); }
static void mut_td_bytes_mismatch(fx *f) { p64(f, f->h_td_bytes, f->td_len + 64); }
static void mut_ed_bytes_mismatch(fx *f) { p64(f, f->h_ed_bytes, f->ed_len + 48); }
static void mut_t_name_full(fx *f) {
    /* fill all 32 name bytes (no NUL terminator) */
    memset(f->buf + f->tentry[0], 'a', 32);
    p16(f, f->tentry[0] + 32, HYDRA2_ROLE_ROUTER);
    p16(f, f->tentry[0] + 34, HYDRA2_F32);
    p16(f, f->tentry[0] + 36, 1);
}
static void mut_t_role_high(fx *f)   { p16(f, f->tentry[0] + 32, 9); }
static void mut_t_type_q40(fx *f)    { p16(f, f->tentry[0] + 34, HYDRA2_Q4_0); }
static void mut_t_type_unknown(fx *f){ p16(f, f->tentry[0] + 34, 0x99); }
static void mut_t_ndims_zero(fx *f)  { p16(f, f->tentry[0] + 36, 0); }
static void mut_t_ndims_five(fx *f)  { p16(f, f->tentry[0] + 36, 5); }
static void mut_t_reserved(fx *f)    { p16(f, f->tentry[0] + 38, 7); }
static void mut_t_nelem_zero(fx *f)  { p64(f, f->tentry[0] + 48, 0); }
static void mut_t_q80_nonmultiple(fx *f) {
    /* make router.0 a Q8_0 tensor with a non-multiple-of-32 element count */
    p16(f, f->tentry[0] + 34, HYDRA2_Q8_0);
    p64(f, f->tentry[0] + 48, 34);      /* 34 % 32 != 0 */
    p64(f, f->tentry[0] + 56, 37);      /* arbitrary; the group check fires first */
}
static void mut_t_nbytes_mismatch(fx *f) { p64(f, f->tentry[0] + 56, 17); }
static void mut_t_offset_unaligned(fx *f) { p64(f, f->tentry[0] + 40, 4); }
static void mut_t_offset_overrun(fx *f)   { p64(f, f->tentry[0] + 40, 5000); }
static void mut_e_unsorted(fx *f) {
    /* swap expert (0,1) before (0,0): put eid 0 after eid 1 in row 0 */
    uint8_t tmp[48];
    memcpy(tmp, f->buf + f->eentry[0], 48);
    memcpy(f->buf + f->eentry[0], f->buf + f->eentry[1], 48);
    memcpy(f->buf + f->eentry[1], tmp, 48);
    /* (entry 0's offset now also violates the prefix rule, but the ordering
       rule is checked first for i > 0 - the code is what makes the point) */
}
static void mut_e_length_zero(fx *f) { p64(f, f->eentry[0] + 16, 0); }
static void mut_e_offset_gap(fx *f)  { p64(f, f->eentry[1] + 8, 16); } /* 16 != 64 prefix */
static void mut_e_offset_overrun(fx *f) { p64(f, f->eentry[0] + 8, 0x7FFFFFFF); }
static void mut_e_type_unknown(fx *f) { p32(f, f->eentry[0] + 32, 0x99); }
static void mut_e_reserved(fx *f)    { p64(f, f->eentry[0] + 40, 3); }
static void mut_e_crc_stored_bad(fx *f) { p32(f, f->eentry[0] + 36, 0x12345678); }
static void mut_exp_bytes_small(fx *f) {
    /* header claims 32 bytes fewer expert bytes than the directory needs:
       the region still fits in the file, so only the per-expert
       offset+length vs region-end check (5.1.2) can catch it. The header
       CRC must be recomputed: byte 112 sits inside the CRC-covered range
       [0,120), and the CRC check runs before the directory parsing. */
    p64(f, f->h_exp_bytes, f->exp_len - 32);
    p32(f, f->h_hcrc, hydra2_crc32(f->buf, 120));
}
static void mut_corrupt_expert0_byte(fx *f) { f->buf[f->exp_payload[0] + 3] ^= 0xFF; }

static void test_header_refusals(void)
{
    section("header / region refusals");
    CHECK(open_mutated(mut_v1magic) == HYDRA2_E_MAGIC, "v1 magic");
    CHECK(open_mutated(mut_garbage_magic) == HYDRA2_E_MAGIC, "garbage magic");
    CHECK(open_mutated(mut_version_major) == HYDRA2_E_VERSION, "version_major 3");
    CHECK(open_mutated(mut_version_minor) == HYDRA2_E_VERSION, "version_minor 1");
    CHECK(open_mutated(mut_header_bytes) == HYDRA2_E_VERSION, "header_bytes 256");
    CHECK(open_mutated(mut_reserved) == HYDRA2_E_VERSION, "reserved != 0");
    CHECK(open_mutated(mut_fsize_larger) == HYDRA2_E_FILESIZE, "file_size > disk");
    CHECK(open_mutated(mut_fsize_smaller) == HYDRA2_E_FILESIZE, "file_size < header");
    CHECK(open_mutated(mut_meta_off_low) == HYDRA2_E_META_OOB, "meta_off < 128");
    CHECK(open_mutated(mut_meta_off_unaligned) == HYDRA2_E_META_OOB, "meta_off unaligned");
    CHECK(open_mutated(mut_meta_bytes_zero) == HYDRA2_E_META_OOB, "meta_bytes 0");
    CHECK(open_mutated(mut_meta_overrun) == HYDRA2_E_META_OOB, "meta overrun");
    CHECK(open_mutated(mut_hcrc_bad) == HYDRA2_E_HDR_CRC, "header_crc mismatch");

    /* truncated file: write fewer bytes than the header's file_size */
    fx f;
    if (build_fx(&f) == 0) {
        size_t trunc = (size_t)(f.len - 64);
        CHECK(write_file(FIXPATH, f.buf, trunc) == 0, "trunc write");
        CHECK(open_fixture(FIXPATH) == HYDRA2_E_FILESIZE,
              "truncated file should be E_FILESIZE");
        free_fx(&f);
    }
    /* file smaller than the header itself */
    CHECK(write_file(FIXPATH, (const uint8_t *)"tiny", 4) == 0, "tiny write");
    CHECK(open_fixture(FIXPATH) == HYDRA2_E_SMALL, "file < 128 should be E_SMALL");
}

static void test_directory_count_refusals(void)
{
    section("directory count / size refusals (both ends + overflow guard)");
    CHECK(open_mutated(mut_td_count_zero) == HYDRA2_E_TENSOR_OOB, "td_count 0");
    CHECK(open_mutated(mut_td_count_huge) == HYDRA2_E_TENSOR_OOB,
          "td_count overflow guard");
    CHECK(open_mutated(mut_td_bytes_mismatch) == HYDRA2_E_TENSOR_OOB,
          "td_bytes != count*64");
    CHECK(open_mutated(mut_ed_bytes_mismatch) == HYDRA2_E_EXPERT_OOB,
          "ed_bytes != count*48");
}

static void test_metadata_refusals(void)
{
    section("required routing metadata refusals");
    CHECK(open_mutated(mut_topk_zero) == HYDRA2_E_META_REQUIRED, "top_k 0");
    CHECK(open_mutated(mut_topk_65) == HYDRA2_E_META_REQUIRED, "top_k 65");
    CHECK(open_mutated(mut_epl_lt_topk) == HYDRA2_E_META_REQUIRED, "epl < top_k");
    CHECK(open_mutated(mut_arch_bad) == HYDRA2_E_META_REQUIRED, "unknown arch");
    CHECK(open_mutated(mut_layers_zero) == HYDRA2_E_META_REQUIRED, "layers 0");
    CHECK(open_mutated(mut_runs_two) == HYDRA2_E_META_REQUIRED, "runs 2");

    /* the architecture refusal must echo the offending name (5.2) */
    if (cap_stderr_begin(CAPPATH) == 0) {
        (void)open_mutated(mut_arch_bad);
        cap_stderr_end();
        CHECK(file_contains(CAPPATH, "not-a-real-arch-xx"),
              "architecture refusal did not echo the name");
        CHECK(file_contains(CAPPATH, "3.1"),
              "architecture refusal did not cite the spec section");
    }
}

static void test_tensor_refusals(void)
{
    section("tensor directory entry refusals");
    CHECK(open_mutated(mut_t_name_full) == HYDRA2_E_TENSOR_BAD, "name not NUL-terminated");
    CHECK(open_mutated(mut_t_role_high) == HYDRA2_E_TENSOR_BAD, "role 9");
    CHECK(open_mutated(mut_t_type_q40) == HYDRA2_E_TENSOR_BAD, "type Q4_0");
    CHECK(open_mutated(mut_t_type_unknown) == HYDRA2_E_TENSOR_BAD, "type 0x99");
    CHECK(open_mutated(mut_t_ndims_zero) == HYDRA2_E_TENSOR_BAD, "n_dims 0");
    CHECK(open_mutated(mut_t_ndims_five) == HYDRA2_E_TENSOR_BAD, "n_dims 5");
    CHECK(open_mutated(mut_t_reserved) == HYDRA2_E_TENSOR_BAD, "reserved != 0");
    CHECK(open_mutated(mut_t_nelem_zero) == HYDRA2_E_TENSOR_BAD, "n_elements 0");
    CHECK(open_mutated(mut_t_q80_nonmultiple) == HYDRA2_E_TENSOR_BAD, "Q8_0 n_elements not a multiple of 32");
    CHECK(open_mutated(mut_t_nbytes_mismatch) == HYDRA2_E_TENSOR_BAD, "n_bytes mismatch");
    CHECK(open_mutated(mut_t_offset_unaligned) == HYDRA2_E_TENSOR_BAD, "t offset unaligned");
    CHECK(open_mutated(mut_t_offset_overrun) == HYDRA2_E_TENSOR_BAD, "t offset overrun");
}

static void test_expert_refusals(void)
{
    section("expert directory entry refusals");
    CHECK(open_mutated(mut_e_unsorted) == HYDRA2_E_EXPERT_BAD, "unsorted");
    CHECK(open_mutated(mut_e_length_zero) == HYDRA2_E_EXPERT_BAD, "length 0");
    CHECK(open_mutated(mut_e_offset_gap) == HYDRA2_E_EXPERT_BAD, "offset != prefix");
    CHECK(open_mutated(mut_e_offset_overrun) == HYDRA2_E_EXPERT_BAD, "offset overrun");
    CHECK(open_mutated(mut_e_type_unknown) == HYDRA2_E_EXPERT_BAD, "type 0x99");
    CHECK(open_mutated(mut_e_reserved) == HYDRA2_E_EXPERT_BAD, "reserved != 0");
    CHECK(open_mutated(mut_exp_bytes_small) == HYDRA2_E_EXPERT_BAD,
          "experts region end (header) not honored");

    /* stored CRC wrong: open succeeds (5.1.5), READ fails */
    fx f;
    if (build_fx(&f) == 0) {
        mut_e_crc_stored_bad(&f);
        (void)write_file(FIXPATH, f.buf, f.len);
        free_fx(&f);
        hydra2_container_t c;
        int rc = hydra2_open(&c, FIXPATH);
        CHECK(rc == HYDRA2_OK, "open with bad stored crc should still open");
        if (rc == HYDRA2_OK) {
            const hydra2_expert_entry_t *e;
            hydra2_find_expert(&c, 0, 0, &e);
            uint8_t b[128];
            int r = hydra2_read_expert(&c, e, b, sizeof(b));
                        char mb_732[160];
            snprintf(mb_732, sizeof(mb_732), "read with bad stored crc should be E_CRC, got %d", r);
            CHECK(r == HYDRA2_E_CRC, mb_732);
            hydra2_close(&c);
        }
    }
}

static void test_expert_read_crc(void)
{
    section("expert streaming read: CRC verified at read time (5.1.5)");
    CHECK(fresh_fixture() == 0, "fresh fixture");
    hydra2_container_t c;
    if (hydra2_open(&c, FIXPATH) != HYDRA2_OK) { CHECK(0, "open"); return; }
    const hydra2_expert_entry_t *e;
    hydra2_find_expert(&c, 0, 0, &e);
    uint8_t b[128];
    CHECK(hydra2_read_expert(&c, e, b, sizeof(b)) == HYDRA2_OK, "clean read");
    hydra2_close(&c);

    /* corrupt ONE payload byte on disk; the stored CRC in the directory is
       still the old one, so the read must now fail with E_CRC */
    fx f;
    if (build_fx(&f) == 0) {
        mut_corrupt_expert0_byte(&f);
        (void)write_file(FIXPATH, f.buf, f.len);
        free_fx(&f);
        if (hydra2_open(&c, FIXPATH) != HYDRA2_OK) { CHECK(0, "open corrupt"); return; }
        hydra2_find_expert(&c, 0, 0, &e);
        int r = HYDRA2_OK;
        if (cap_stderr_begin(CAPPATH) == 0) {
            /* the message must name the expert (layer 0, expert 0) */
            r = hydra2_read_expert(&c, e, b, sizeof(b));
            cap_stderr_end();
            CHECK(file_contains(CAPPATH, "layer 0, expert 0"),
                  "CRC refusal did not name the expert");
        }
                char mb_767[160];
        snprintf(mb_767, sizeof(mb_767), "corrupted expert byte should be E_CRC, got %d", r);
        CHECK(r == HYDRA2_E_CRC, mb_767);
        hydra2_close(&c);
    }
}

static void test_mutual_v1_v2_refusal(void)
{
    section("mutual v1/v2 refusal (FORMAT-V2.md 7.2/7.3)");

    /* v2 reader refuses a v1 file, naming v1 (7.3) */
    if (cap_stderr_begin(CAPPATH) == 0) {
        int rc = open_mutated(mut_v1magic);
        cap_stderr_end();
        CHECK(rc == HYDRA2_E_MAGIC, "v2 reader on v1 file");
        CHECK(file_contains(CAPPATH, "v1 .hydra file"),
              "v2 reader did not name the v1 file type");
    }

    /* v1 loader refuses a v2 file, naming HYDRA2 (7.2). Build a pristine
       v2 fixture and point the v1 loader at it. */
    CHECK(fresh_fixture() == 0, "fresh v2 fixture");
    HydraEngine eng;
    memset(&eng, 0, sizeof(eng));
    if (cap_stderr_begin(CAPPATH) == 0) {
        int rc = hydra_engine_load(&eng, FIXPATH);
        cap_stderr_end();
                char mb_793[160];
        snprintf(mb_793, sizeof(mb_793), "v1 loader on v2 file should return -5, got %d", rc);
        CHECK(rc == -5, mb_793);
        CHECK(file_contains(CAPPATH, "HYDRA2"),
              "v1 loader did not name the v2 magic");
        if (eng.mapped_weights) {
            munmap((void *)eng.mapped_weights, eng.mapped_size);
            eng.mapped_weights = NULL;
        }
        if (eng.fd >= 0) { close(eng.fd); eng.fd = -1; }
    }
}

static void test_missing_and_symlink(void)
{
    section("open failures: missing file and symlink (O_NOFOLLOW)");
    CHECK(open_fixture("/tmp/does-not-exist-hydra2.hydra2") == HYDRA2_E_OPEN,
          "missing file should be E_OPEN");

    CHECK(fresh_fixture() == 0, "fixture for symlink test");
    (void)unlink(SYMLINK);
    if (symlink(FIXPATH, SYMLINK) != 0) {
                char mb_813[160];
        snprintf(mb_813, sizeof(mb_813), "could not create symlink: %s", strerror(errno));
        CHECK(0, mb_813);
        return;
    }
    CHECK(open_fixture(SYMLINK) == HYDRA2_E_OPEN,
          "symlink should be refused by O_NOFOLLOW (E_OPEN)");
    (void)unlink(SYMLINK);
}


/* Cross-implementation check: tools/make_model.py --v2 must produce a
 * file the C reader accepts, field for field. The gated paths
 * (make test-run, CI) always set HYDRA2_PY_FIXTURE to a freshly
 * generated file; running the binary without it is a local mistake and
 * this test fails loudly instead of silently skipping. */
/* The reader's control-region cap (HYDRA2_MAX_CONTROL_REGION): a header
 * that fits inside the file but exceeds the cap must be refused before
 * any region is read (that is what bounds the allocation CodeQL's
 * untrusted-size check asks for). The file is sparse - header plus
 * ftruncate: the refusal happens before the first region read, so the
 * zero-filled remainder is never touched. The header CRC is deliberately
 * left stale: the cap check runs BEFORE header-CRC verification, and if
 * a future reorder changes that, this test turns red (good). */
static void test_control_region_cap(void)
{
    section("control region cap (256 MiB reader cap)");
    fx f;
    if (build_fx(&f) != 0) {
        g_tests++;
        g_fail++;
        printf("  FAIL: build fixture for cap test\n");
        return;
    }
    const uint64_t big = 256ull * 1024ull * 1024ull + 4096ull;
    p64(&f, f.h_meta_bytes, big);
    p64(&f, f.h_fsize, 128ull + big);
    (void)unlink(CAPFILE);
    FILE *fo = fopen(CAPFILE, "wb");
    if (!fo || fwrite(f.buf, 1, 128, fo) != 128 ||
        ftruncate(fileno(fo), (off_t)(128ull + big)) != 0) {
        g_tests++;
        g_fail++;
        printf("  FAIL: sparse cap file\n");
        if (fo) fclose(fo);
        free_fx(&f);
        (void)unlink(CAPFILE);
        return;
    }
    fclose(fo);
    free_fx(&f);
    if (cap_stderr_begin(CAPPATH) == 0) {
        int rc = open_fixture(CAPFILE);
        char mb1[160];
        snprintf(mb1, sizeof(mb1), "cap: expected E_META_OOB, got %d", rc);
        cap_stderr_end();
        CHECK(rc == HYDRA2_E_META_OOB, mb1);
        CHECK(file_contains(CAPPATH, "control region cap"),
              "cap refusal did not name the cap");
    }
    (void)unlink(CAPFILE);
}

static const char *g_fixture = NULL; /* argv[1], set in main */

static void test_python_fixture_cross_check(void)
{
    section("cross-implementation: tools/make_model.py --v2 fixture");
    /* The fixture path is argv[1], not an environment variable: argv is
     * the supported entry point (the Makefile/CI pass it explicitly) and
     * keeps the path out of the env-taint class static analysis flags
     * for env-derived open() arguments. */
    const char *path = g_fixture;
    if (!path) {
        g_tests++;
        g_fail++;
        printf("  FAIL: no fixture path argument - pass the "
               "python-generated fixture as argv[1] (make test-run and "
               "CI do this; a bare ./hydra2-test is not a supported "
               "entry point)\n");
        return;
    }
    hydra2_container_t c;
    int rc = hydra2_open(&c, path);
        char mb_841[160];
    snprintf(mb_841, sizeof(mb_841), "python-generated fixture refused: %d", rc);
    CHECK(rc == HYDRA2_OK, mb_841);
    if (rc != HYDRA2_OK) return;
    CHECK(c.version_major == 2 && c.version_minor == 0, "py fixture version");
        char mb_844[160];
    snprintf(mb_844, sizeof(mb_844), "py fixture architecture '%s'", c.architecture);
    CHECK(strcmp(c.architecture, "moe-transformer-v1") == 0, mb_844);
        char mb_846[160];
    snprintf(mb_846, sizeof(mb_846), "py fixture top_k %u > epl %u", c.top_k, c.experts_per_layer);
    CHECK(c.top_k >= 1 && c.top_k <= c.experts_per_layer, mb_846);
        char mb_848[160];
    snprintf(mb_848, sizeof(mb_848), "py fixture tensors %llu != layers+2 (%u)", (unsigned long long)c.tensor_dir_count, (unsigned)c.layers);
    CHECK(c.tensor_dir_count == (uint64_t)(c.layers + 2), mb_848);
        char mb_851[160];
    snprintf(mb_851, sizeof(mb_851), "py fixture experts %llu != layers*epl", (unsigned long long)c.expert_dir_count);
    CHECK(c.expert_dir_count ==
          (uint64_t)(c.layers * c.experts_per_layer), mb_851);
    /* every expert is exactly the nominal size declared in the metadata */
    uint64_t nominal = c.expert_bytes_typ;
    int all_nominal = 1;
    for (uint64_t i = 0; i < c.expert_dir_count; i++)
        if (c.experts[i].length != nominal) all_nominal = 0;
    CHECK(all_nominal, "py fixture expert length != v2.expert_bytes_typ");
    /* stream the first expert: one pread, CRC verified at read time */
    const hydra2_expert_entry_t *e;
    CHECK(hydra2_find_expert(&c, 0, 0, &e) == HYDRA2_OK, "py find (0,0)");
    uint8_t *eb = (uint8_t *)malloc(nominal ? nominal : 1);
    if (eb) {
        CHECK(hydra2_read_expert(&c, e, eb, nominal) == HYDRA2_OK,
              "py read expert (0,0)");
        free(eb);
    }
    /* and a trunk tensor (the layer-0 router weights) */
    const hydra2_tensor_entry_t *t;
    CHECK(hydra2_find_tensor(&c, "router.0", &t) == HYDRA2_OK,
          "py find router.0");
    if (t) {
        uint8_t *tb = (uint8_t *)malloc(t->n_bytes ? t->n_bytes : 1);
        if (tb) {
            CHECK(hydra2_read_tensor(&c, t, tb, t->n_bytes) == HYDRA2_OK,
                  "py read router.0");
            free(tb);
        }
    }
    hydra2_close(&c);
}

int main(int argc, char **argv)
{
    if (argc > 1) g_fixture = argv[1];
    make_dirs();
    test_crc_known_answers();
    test_happy_path();
    test_header_refusals();
    test_directory_count_refusals();
    test_metadata_refusals();
    test_control_region_cap();
    test_tensor_refusals();
    test_expert_refusals();
    test_expert_read_crc();
    test_python_fixture_cross_check();
    test_mutual_v1_v2_refusal();
    test_missing_and_symlink();

    (void)unlink(FIXPATH);
    (void)unlink(CAPPATH);

    printf("\n=== %d Tests, %d failures ===\n", g_tests, g_fail);
    return g_fail == 0 ? 0 : 1;
}

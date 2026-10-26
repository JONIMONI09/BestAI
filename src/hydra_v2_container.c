/* ====================================================================
 * hydra_v2_container.c - the `.hydra2` sparse-MoE container (v2).
 *
 * docs/FORMAT-V2.md, implemented. The invariants this file holds to:
 *
 *  - BOTH-ENDS validation (R14/R28, FORMAT-V2.md 5.1): every offset is
 *    checked >= its region base AND + its length <= the region end;
 *    every count is overflow-checked in 64-bit BEFORE the multiply
 *    (count > file_size / entry_size is the guard - never multiply
 *    first); file_size is checked against the real size on disk so a
 *    truncated transfer is detectable without seeking.
 *  - Loud, specific refusals (5.2): every failure names the field, the
 *    value, and the bound it violated, in the user's terms. No
 *    generic "invalid file", no internal paths, no stack traces.
 *  - Streaming: the header, the metadata block, and the two
 *    directories are read at open. Expert payloads are NEVER resident -
 *    one pread per read, and the stored CRC-32 is verified over the
 *    bytes as they are read (5.1.5: at read time, not at open).
 *  - LE byte-wise readers throughout: a big-endian host reads through
 *    explicit byte assembly, never through the native layout (4.0).
 * ==================================================================== */
/* Feature-test macros: under a strict -std=c99 compile glibc hides
 * pread, O_CLOEXEC and O_NOFOLLOW behind these. Defining them in the
 * source file (before any include) keeps the file self-contained: every
 * build and every C++ parse of this translation unit gets the same
 * symbol set instead of relying on each consumer adding
 * -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE. (Same block as
 * src/hydra_engine.c, 2026-10-26.) */
#if !defined(__APPLE__) && !defined(__MACH__)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#endif /* !__APPLE__ && !__MACH__ */

#include "hydra_v2_container.h"

/* The reader loads exactly three regions whole into RAM (metadata K/V
 * block, tensor directory, expert directory); trunk and expert payloads
 * are streamed with pread. Hostile multi-giB headers could otherwise
 * demand multi-giB allocations bounded only by file size; this cap is
 * far above any real control region (a 1 MiB expert directory holds
 * ~21,000 entries) and bounds what a header can force. Recorded in
 * FORMAT-V2.md as an implementation decision (2026-10-26). */
#define HYDRA2_MAX_CONTROL_REGION (256u * 1024u * 1024u)

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/* ------------------------------------------------------------------ */
/* Byte-wise little-endian readers (FORMAT-V2.md section 4: "without   */
/* exception"). No struct casts, no native-layout assumptions.        */
/* ------------------------------------------------------------------ */

static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64le(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

/* ------------------------------------------------------------------ */
/* CRC-32, IEEE, reflected, polynomial 0xEDB88320 (the one the expert */
/* directory stores - FORMAT-V2.md section 4.3).                      */
/* Known answers pinned by tests: "123456789" -> 0xCBF43926, "" -> 0. */
/* ------------------------------------------------------------------ */

static uint32_t crc32_table[256];
static int crc32_table_ready = 0;

static void crc32_init_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_ready = 1;
}

uint32_t hydra2_crc32(const uint8_t *data, size_t len)
{
    if (!crc32_table_ready) crc32_init_table();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc32_table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* Refusal printing: field + value + bound, always. The caller passes */
/* the refusal code; it is returned to the caller unchanged.          */
/* ------------------------------------------------------------------ */

static int refuse(int rc, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "[Hydra2] ");
    va_start(ap, fmt);
    /* flawfinder: ignore - CWE-134 false positive: every refuse() call site passes a compile-time literal format string (arguments are values, never the format; verified by grep). */
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return rc;
}

/* Allocation wrapper: prints a specific refusal on OOM and returns
 * NULL; the caller must check and bail. */
/* calloc, not malloc: callers free whatever the allocation holds even
 * when a later parse step fails (partial state), so every byte - and
 * every embedded pointer (metadata values) - must start out zero.
 * (Found by ASan, 2026-10-26: a partial metadata parse left the
 * un-parsed pairs' value pointers as heap garbage, which close freed.) */
static void *xalloc(uint64_t n)
{
    /* n == 0 allocates a live 1-byte block (never NULL): callers may
     * hold the pointer even for an empty object and free it unconditionally.
     * The branchless minimum keeps the analyzer's region model single-valued
     * (the old ternary split the region into two states and produced a
     * phantom ArrayBound finding on the caller's element access). */
    void *p = calloc(n + (n == 0), 1);
    if (!p)
        refuse(HYDRA2_E_NOMEM, "memory: could not allocate %llu bytes "
               "(out of memory)", (unsigned long long)n);
    return p;
}

/* pread into a caller buffer, looping over short reads (short reads
 * are legal even on regular files; a partial read is never a success
 * of a full-payload operation). Returns 0 or a refusal code. */
static int read_full(int fd, uint8_t *buf, uint64_t len, uint64_t off)
{
    uint64_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, buf + done, (size_t)(len - done),
                          (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return refuse(HYDRA2_E_READ, "io: pread failed at offset "
                           "%llu (%s)", (unsigned long long)(off + done),
                           strerror(errno));
        }
        if (n == 0)
            return refuse(HYDRA2_E_READ, "io: pread hit EOF at offset "
                           "%llu, wanted %llu more bytes (file is "
                           "truncated)", (unsigned long long)(off + done),
                           (unsigned long long)(len - done));
        done += (uint64_t)n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Tensor types (FORMAT-V2.md 3.2). The first five have decoders      */
/* somewhere in this repository (GGUF import path, v1); the last      */
/* three are reserved and MUST be refused by name.                    */
/* ------------------------------------------------------------------ */

static const char *type_name(uint16_t type_id)
{
    switch (type_id) {
    case HYDRA2_TERNARY2: return "TERNARY2";
    case HYDRA2_F16:      return "F16";
    case HYDRA2_BF16:     return "BF16";
    case HYDRA2_F32:      return "F32";
    case HYDRA2_Q8_0:     return "Q8_0";
    case HYDRA2_Q4_0:     return "Q4_0";
    case HYDRA2_Q4_K:     return "Q4_K";
    case HYDRA2_MXFP4:    return "MXFP4";
    default:              return "UNKNOWN";
    }
}

/* -1 for unknown/reserved types; 0 marks the "grouped" types whose
 * bytes-per-element is not a constant (see tensor_n_bytes). */
static int64_t bytes_per_element(uint16_t type_id)
{
    switch (type_id) {
    case HYDRA2_TERNARY2: return 0;   /* 2 elements/byte             */
    case HYDRA2_F16:
    case HYDRA2_BF16:     return 2;
    case HYDRA2_F32:      return 4;
    case HYDRA2_Q8_0:     return 0;   /* 34 bytes per 32-element block */
    default:              return -1;
    }
}

/* n_bytes for a tensor with n_elements of the given type; 0 on any
 * structural violation (not a multiple of the element group). */
static uint64_t tensor_n_bytes(uint16_t type_id, uint64_t n_elements)
{
    switch (type_id) {
    case HYDRA2_TERNARY2:
        return (n_elements % 2u) ? 0 : (n_elements / 2u);
    case HYDRA2_F16:
    case HYDRA2_BF16:
        return n_elements * 2u;
    case HYDRA2_F32:
        return n_elements * 4u;
    case HYDRA2_Q8_0:
        return (n_elements % 32u) ? 0 : (n_elements / 32u) * 34u;
    default:
        return 0;
    }
}

/* Free whatever a partially-parsed metadata block allocated, so
 * parse_metadata() is self-contained on every failure return (the
 * caller's hydra2_close() then sees NULL and frees nothing twice).
 * gcc -fanalyzer, 2026-10-26. */
static void meta_free_partial(hydra2_container_t *c)
{
    if (c->meta_blob) { free((void *)c->meta_blob); c->meta_blob = NULL; }
    if (c->meta) { free((void *)c->meta); c->meta = NULL; }
    c->meta_count = 0;
}

/* ------------------------------------------------------------------ */
/* Metadata K/V wire format (FORMAT-V2.md 4.4 names the keys; the     */
/* block layout is this implementation, recorded there too):          */
/*                                                                    */
/*   u32 count                                                        */
/*   per pair: u32 key_len, u32 val_len, key bytes, val bytes         */
/*                                                                    */
/* All little-endian. Keys: 1..63 ASCII, unique. Values: raw bytes,   */
/* width checked per required key.                                    */
/* ------------------------------------------------------------------ */

static int parse_metadata(hydra2_container_t *c, const uint8_t *m,
                          uint64_t bytes)
{
    /* Two flat allocations, no sub-pointers: one index array (POD) and
     * one blob holding every key and value byte. (gcc -fanalyzer cannot
     * track a heap array of structs with heap sub-pointers - it reports a
     * phantom leak on free; this layout is the verifiable one.) */
    uint64_t pos = 0;
    if (bytes < 4u) {
        meta_free_partial(c);
        return refuse(HYDRA2_E_META_BAD, "metadata: block is %llu bytes, "
                       "too short for the 4-byte count",
                       (unsigned long long)bytes);
    }
    uint32_t count = rd_u32le(m);
    pos = 4;
    if ((uint64_t)count * 8u > bytes) {
        meta_free_partial(c);
        return refuse(HYDRA2_E_META_BAD, "metadata: count %u * 8 overflows "
                       "the %llu-byte block", count,
                       (unsigned long long)bytes);
    }
    /* Both are allocated even for count == 0 (xalloc(0) returns a live
     * 1-byte block): the loop below dereferences meta_blob, and the
     * analyzer must see a single unconditional allocation point.
     *
     * The count == 0 case is branched, not folded into the size
     * expression: a single expression that changes with count
     * (n ? n : 1 / n + (n == 0)) makes the analyzer join two region
     * states (1 byte and count*16 bytes) and report a phantom
     * ArrayBound at the &c->meta[i] below. With the branch, on the
     * loop-reachable path the region is exactly count*16 bytes, which
     * is what the loop bound i < count needs. */
    c->meta_count = count;
    if (count == 0) {
        c->meta = xalloc(0);
    } else {
        c->meta = xalloc((uint64_t)count * sizeof(hydra2_meta_kv_t));
    }
    if (!c->meta) { meta_free_partial(c); return HYDRA2_E_NOMEM; }
    c->meta_blob = xalloc(bytes - 4u);
    if (!c->meta_blob) { meta_free_partial(c); return HYDRA2_E_NOMEM; }
    uint64_t blob_pos = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 8u > bytes) {
            meta_free_partial(c);
            return refuse(HYDRA2_E_META_BAD, "metadata: pair %u overruns the "
                           "%llu-byte block (needs 8 header bytes at %llu)",
                           i, (unsigned long long)bytes,
                           (unsigned long long)pos);
        }
        uint32_t klen = rd_u32le(m + pos);
        uint32_t vlen = rd_u32le(m + pos + 4);
        pos += 8;
        if (klen < 1u || klen > 63u) {
            meta_free_partial(c);
            return refuse(HYDRA2_E_META_BAD, "metadata: pair %u key length "
                           "%u outside 1..63", i, klen);
        }
        if (vlen > 65536u) {
            meta_free_partial(c);
            return refuse(HYDRA2_E_META_BAD, "metadata: pair %u value length "
                           "%u exceeds 65536", i, vlen);
        }
        if (pos + (uint64_t)klen + (uint64_t)vlen > bytes) {
            meta_free_partial(c);
            return refuse(HYDRA2_E_META_BAD, "metadata: pair %u overruns the "
                           "%llu-byte block (key %u + value %u bytes at "
                           "position %llu)", i, (unsigned long long)bytes,
                           klen, vlen, (unsigned long long)pos);
        }
        hydra2_meta_kv_t *kv = &c->meta[i];
        kv->key_len = klen;
        kv->key_off = (uint32_t)blob_pos;
        memcpy((uint8_t *)c->meta_blob + blob_pos, m + pos, klen);
        blob_pos += klen;
        kv->val_len = vlen;
        kv->val_off = (uint32_t)blob_pos;
        if (vlen > 0) memcpy((uint8_t *)c->meta_blob + blob_pos, m + pos + klen, vlen);
        blob_pos += vlen;
        pos += (uint64_t)klen + (uint64_t)vlen;
        for (uint32_t ch = 0; ch < klen; ch++) {
            uint8_t kb = c->meta_blob[kv->key_off + ch];
            if (kb < 0x21u || kb > 0x7Eu) {
                /* kb captured BEFORE the free: the message must not
                 * dereference the blob after meta_free_partial NULLs it
                 * (caught by gcc -fanalyzer, 2026-10-26). */
                meta_free_partial(c);
                return refuse(HYDRA2_E_META_BAD, "metadata: key of pair %u "
                               "contains non-ASCII byte 0x%02X at position "
                               "%u", i, kb, ch);
            }
        }
        for (uint32_t j = 0; j < i; j++)
            if (c->meta[j].key_len == klen &&
                memcmp(c->meta_blob + c->meta[j].key_off,
                       c->meta_blob + kv->key_off, klen) == 0) {
                meta_free_partial(c);
                return refuse(HYDRA2_E_META_BAD, "metadata: duplicate key "
                               "(pairs %u and %u)", j, i);
            }
    }
    if (pos != bytes) {
        meta_free_partial(c);
        return refuse(HYDRA2_E_META_BAD, "metadata: block is %llu bytes but "
                       "the pairs end at %llu (trailing bytes)",
                       (unsigned long long)bytes, (unsigned long long)pos);
    }
    return 0;
}

static const hydra2_meta_kv_t *meta_find(const hydra2_container_t *c,
                                         const char *key)
{
    size_t klen = strlen(key);
    for (uint32_t i = 0; i < c->meta_count; i++)
        if (c->meta[i].key_len == (uint32_t)klen &&
            memcmp(c->meta_blob + c->meta[i].key_off, key, klen) == 0)
            return &c->meta[i];
    return NULL;
}

static int meta_u32(const hydra2_container_t *c, const char *key,
                    uint32_t *out)
{
    const hydra2_meta_kv_t *kv = meta_find(c, key);
    if (!kv)
        return refuse(HYDRA2_E_META_REQUIRED,
                      "metadata: required key '%s' is missing - a reader "
                      "that guesses a routing key would silently route to "
                      "a different expert set (FORMAT-V2.md 4.4: top_k is "
                      "required, not defaulted)", key);
    if (kv->val_len != 4u)
        return refuse(HYDRA2_E_META_REQUIRED,
                      "metadata: '%s' is %u bytes, expected 4 "
                      "(u32 little-endian)", key, kv->val_len);
    *out = rd_u32le(c->meta_blob + kv->val_off);
    return 0;
}

static int meta_u64(const hydra2_container_t *c, const char *key,
                    uint64_t *out)
{
    const hydra2_meta_kv_t *kv = meta_find(c, key);
    if (!kv)
        return refuse(HYDRA2_E_META_REQUIRED,
                      "metadata: required key '%s' is missing", key);
    if (kv->val_len != 8u)
        return refuse(HYDRA2_E_META_REQUIRED,
                      "metadata: '%s' is %u bytes, expected 8 "
                      "(u64 little-endian)", key, kv->val_len);
    *out = rd_u64le(c->meta_blob + kv->val_off);
    return 0;
}

static int meta_str(const hydra2_container_t *c, const char *key,
                    char *out, size_t cap)
{
    const hydra2_meta_kv_t *kv = meta_find(c, key);
    if (!kv)
        return refuse(HYDRA2_E_META_REQUIRED,
                      "metadata: required key '%s' is missing", key);
    if (kv->val_len == 0u || kv->val_len >= cap)
        return refuse(HYDRA2_E_META_REQUIRED,
                      "metadata: '%s' is %u bytes, expected a non-empty "
                      "string of less than %zu bytes",
                      key, kv->val_len, cap);
    memcpy(out, c->meta_blob + kv->val_off, kv->val_len);
    out[kv->val_len] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* hydra2_open                                                        */
/* ------------------------------------------------------------------ */

int hydra2_open(hydra2_container_t *c, const char *path)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    int rc = HYDRA2_E_ARG;
    if (!path || !*path) return refuse(HYDRA2_E_ARG, "open: path is empty");

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return refuse(HYDRA2_E_OPEN, "open: cannot open file (%s)",
                       strerror(errno));
    c->fd = fd;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        rc = refuse(HYDRA2_E_OPEN, "stat: cannot stat file (%s)",
                        strerror(e));
        close(fd); c->fd = -1;
        return rc;
    }
    uint64_t real_size = (uint64_t)st.st_size;
    if (real_size < HYDRA2_HEADER_BYTES) {
        int rc = refuse(HYDRA2_E_SMALL, "file size: %llu bytes, smaller "
                        "than the 128-byte header",
                        (unsigned long long)real_size);
        close(fd); c->fd = -1;
        return rc;
    }

    uint8_t hdr[HYDRA2_HEADER_BYTES];
    if (read_full(fd, hdr, sizeof(hdr), 0) != 0) {
        int rc = HYDRA2_E_READ;
        close(fd); c->fd = -1;
        return rc;
    }

    /* Magic (4.1): exactly "HYDRA2\0\0". A v1 file is refused with the
     * v1 magic named (7.2); a bad magic is refused with the bytes named. */
    if (memcmp(hdr, "HYDRA2\0\0", HYDRA2_MAGIC_LEN) != 0) {
        int rc;
        if (memcmp(hdr, "HYDR", 4) == 0)
            rc = refuse(HYDRA2_E_MAGIC,
                        "magic: file starts with the v1 'HYDR' magic "
                        "(0x48594452) - this is a v1 .hydra file, not a "
                        ".hydra2 container; the v2 reader is not a v1 "
                        "reader (FORMAT-V2.md 7.2)");
        else
            rc = refuse(HYDRA2_E_MAGIC, "magic: first 8 bytes are "
                        "0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X "
                        "0x%02X, expected 'HYDRA2\\0\\0'",
                        (int)hdr[0], (int)hdr[1], (int)hdr[2], (int)hdr[3],
                        (int)hdr[4], (int)hdr[5], (int)hdr[6], (int)hdr[7]);
        close(fd); c->fd = -1;
        return rc;
    }

    c->version_major     = rd_u16le(hdr + 8);
    c->version_minor     = rd_u16le(hdr + 10);
    c->header_bytes      = rd_u32le(hdr + 12);
    c->file_size         = rd_u64le(hdr + 16);
    c->metadata_offset   = rd_u64le(hdr + 24);
    c->metadata_bytes    = rd_u64le(hdr + 32);
    c->tensor_dir_offset = rd_u64le(hdr + 40);
    c->tensor_dir_count  = rd_u64le(hdr + 48);
    c->tensor_dir_bytes  = rd_u64le(hdr + 56);
    c->expert_dir_offset = rd_u64le(hdr + 64);
    c->expert_dir_count  = rd_u64le(hdr + 72);
    c->expert_dir_bytes  = rd_u64le(hdr + 80);
    c->trunk_offset      = rd_u64le(hdr + 88);
    c->trunk_bytes       = rd_u64le(hdr + 96);
    c->experts_offset    = rd_u64le(hdr + 104);
    c->experts_bytes     = rd_u64le(hdr + 112);
    c->header_crc32      = rd_u32le(hdr + 120);
    uint32_t reserved    = rd_u32le(hdr + 124);

    if (c->version_major != 2u || c->version_minor != 0u) {
        rc = refuse(HYDRA2_E_VERSION, "version: %u.%u, expected 2.0",
                        c->version_major, c->version_minor);
        goto fail;
    }
    if (c->header_bytes != HYDRA2_HEADER_BYTES) {
        rc = refuse(HYDRA2_E_VERSION, "header_bytes: %u, expected 128",
                        c->header_bytes);
        goto fail;
    }
    if (reserved != 0u) {
        rc = refuse(HYDRA2_E_VERSION, "reserved: 0x%08X, expected 0",
                        reserved);
        goto fail;
    }
    if (c->file_size < c->header_bytes) {
        rc = refuse(HYDRA2_E_FILESIZE, "file_size: %llu < header_bytes "
                        "%llu", (unsigned long long)c->file_size,
                        (unsigned long long)c->header_bytes);
        goto fail;
    }
    if (c->file_size != real_size) {
        rc = refuse(HYDRA2_E_FILESIZE,
                        "file_size: header says %llu bytes but the file on "
                        "disk is %llu bytes (truncated or padded "
                        "transfer)", (unsigned long long)c->file_size,
                        (unsigned long long)real_size);
        goto fail;
    }

    /* Region bounds: both ends, always (5.1.2). Each region offset sits
     * after the header, is 16-aligned, and ends before EOF. */
    struct region {
        const char *name;
        uint64_t offset, bytes;
    };
    struct region regions[5] = {
        { "metadata_offset", c->metadata_offset, c->metadata_bytes },
        { "tensor_dir_offset", c->tensor_dir_offset, c->tensor_dir_bytes },
        { "expert_dir_offset", c->expert_dir_offset, c->expert_dir_bytes },
        { "trunk_offset", c->trunk_offset, c->trunk_bytes },
        { "experts_offset", c->experts_offset, c->experts_bytes }
    };
    for (int i = 0; i < 5; i++) {
        uint64_t o = regions[i].offset, b = regions[i].bytes;
        if (o < HYDRA2_HEADER_BYTES) {
            rc = refuse(HYDRA2_E_META_OOB, "%s: %llu < 128 (header "
                            "base)", regions[i].name,
                            (unsigned long long)o);
            goto fail;
        }
        if (o % HYDRA2_ALIGN != 0) {
            rc = refuse(HYDRA2_E_META_OOB, "%s: %llu is not %u-byte "
                            "aligned", regions[i].name,
                            (unsigned long long)o, HYDRA2_ALIGN);
            goto fail;
        }
        if (b > 0u && (o > c->file_size || b > c->file_size - o)) {
            rc = refuse(HYDRA2_E_META_OOB, "%s: offset %llu + bytes "
                            "%llu overruns the %llu-byte file",
                            regions[i].name, (unsigned long long)o,
                            (unsigned long long)b,
                            (unsigned long long)c->file_size);
            goto fail;
        }
        if (b == 0u && i == 0) {
            rc = refuse(HYDRA2_E_META_OOB,
                            "metadata_bytes: 0 - the K/V block must "
                            "exist");
            goto fail;
        }
    }

    /* Control-region cap (see HYDRA2_MAX_CONTROL_REGION): the file-size
     * validation above bounds these by the file; the cap bounds them by a
     * constant, so no header can force an allocation beyond it. */
    if (c->metadata_bytes > HYDRA2_MAX_CONTROL_REGION ||
        c->tensor_dir_bytes > HYDRA2_MAX_CONTROL_REGION ||
        c->expert_dir_bytes > HYDRA2_MAX_CONTROL_REGION) {
        rc = refuse(HYDRA2_E_META_OOB,
                    "control region cap: metadata/tensor-dir/expert-dir "
                    "bytes (%llu/%llu/%llu) exceed the %llu-byte reader "
                    "cap (FORMAT-V2.md implementation decision)",
                    (unsigned long long)c->metadata_bytes,
                    (unsigned long long)c->tensor_dir_bytes,
                    (unsigned long long)c->expert_dir_bytes,
                    (unsigned long long)HYDRA2_MAX_CONTROL_REGION);
        goto fail;
    }

    /* Directory counts: overflow-checked BEFORE the multiply (5.1.3),
     * and the *_bytes fields must equal count * entry_size. */
    if (c->tensor_dir_count < 1u) {
        rc = refuse(HYDRA2_E_TENSOR_OOB, "tensor_dir_count: %llu, "
                        "expected >= 1 (a v2 file always has trunk "
                        "tensors)", (unsigned long long)c->tensor_dir_count);
        goto fail;
    }
    if (c->tensor_dir_count > c->file_size / HYDRA2_TENSOR_ENTRY_B) {
        rc = refuse(HYDRA2_E_TENSOR_OOB, "tensor_dir_count: %llu * %u "
                        "overflows the file (count*entry > file_size)",
                        (unsigned long long)c->tensor_dir_count,
                        HYDRA2_TENSOR_ENTRY_B);
        goto fail;
    }
    if (c->tensor_dir_bytes != c->tensor_dir_count * HYDRA2_TENSOR_ENTRY_B) {
        rc = refuse(HYDRA2_E_TENSOR_OOB, "tensor_dir_bytes: %llu, "
                        "expected tensor_dir_count * 64 = %llu",
                        (unsigned long long)c->tensor_dir_bytes,
                        (unsigned long long)(c->tensor_dir_count *
                                            HYDRA2_TENSOR_ENTRY_B));
        goto fail;
    }
    if (c->expert_dir_count > c->file_size / HYDRA2_EXPERT_ENTRY_B) {
        rc = refuse(HYDRA2_E_EXPERT_OOB, "expert_dir_count: %llu * %u "
                        "overflows the file (count*entry > file_size)",
                        (unsigned long long)c->expert_dir_count,
                        HYDRA2_EXPERT_ENTRY_B);
        goto fail;
    }
    if (c->expert_dir_bytes != c->expert_dir_count * HYDRA2_EXPERT_ENTRY_B) {
        rc = refuse(HYDRA2_E_EXPERT_OOB, "expert_dir_bytes: %llu, "
                        "expected expert_dir_count * 48 = %llu",
                        (unsigned long long)c->expert_dir_bytes,
                        (unsigned long long)(c->expert_dir_count *
                                            HYDRA2_EXPERT_ENTRY_B));
        goto fail;
    }

    /* header_crc32 is verified at open (5.1.6): a mismatch is a
     * refusal, not a warning. */
    uint32_t hcrc = hydra2_crc32(hdr, 120u);
    if (hcrc != c->header_crc32) {
        rc = refuse(HYDRA2_E_HDR_CRC, "header_crc32: header says "
                        "0x%08X, computed CRC-32 of bytes [0,120) is "
                        "0x%08X", c->header_crc32, hcrc);
        goto fail;
    }

    /* ---- metadata K/V block -------------------------------------- */
    uint8_t *m = xalloc(c->metadata_bytes);
    if (!m) { rc = HYDRA2_E_NOMEM; goto fail; }
    if (read_full(fd, m, c->metadata_bytes, c->metadata_offset) != 0) {
        rc = HYDRA2_E_READ;
        free(m);
        goto fail;
    }
    int mrc = parse_metadata(c, m, c->metadata_bytes);
    free(m);
    if (mrc != 0) { rc = mrc; goto fail; }

    /* Required routing metadata (4.4): top_k is required, not
     * defaulted; experts_per_layer >= top_k; architecture by name. */
    int rrc = meta_str(c, "v2.architecture", c->architecture,
                       sizeof(c->architecture));
    if (rrc != 0) { rc = rrc; goto fail; }
    if (strcmp(c->architecture, HYDRA2_ARCH_RECURRENT) != 0 &&
        strcmp(c->architecture, HYDRA2_ARCH_MOE) != 0) {
        rc = refuse(HYDRA2_E_META_REQUIRED,
                        "metadata: v2.architecture '%s' is not in the "
                        "supported table (%s / %s) - refused by name "
                        "(FORMAT-V2.md 3.1)",
                        c->architecture, HYDRA2_ARCH_RECURRENT,
                        HYDRA2_ARCH_MOE);
        goto fail;
    }
    uint32_t v;
    rrc = meta_u32(c, "v2.layers", &v);        if (rrc != 0) { rc = rrc; goto fail; }
    c->layers = v;
    if (c->layers < 1u) {
        rc = refuse(HYDRA2_E_META_REQUIRED, "metadata: v2.layers %u "
                        "< 1", c->layers);
        goto fail;
    }
    rrc = meta_u32(c, "v2.moe_layers", &v);    if (rrc != 0) { rc = rrc; goto fail; }
    c->moe_layers = v;
    if (c->moe_layers > c->layers) {
        rc = refuse(HYDRA2_E_META_REQUIRED, "metadata: v2.moe_layers "
                        "%u > v2.layers %u", c->moe_layers, c->layers);
        goto fail;
    }
    rrc = meta_u32(c, "v2.top_k", &v);         if (rrc != 0) { rc = rrc; goto fail; }
    c->top_k = v;
    if (c->top_k < 1u || c->top_k > 64u) {
        rc = refuse(HYDRA2_E_META_REQUIRED, "metadata: v2.top_k %u "
                        "outside 1..64", c->top_k);
        goto fail;
    }
    rrc = meta_u32(c, "v2.experts_per_layer", &v);
    if (rrc != 0) { rc = rrc; goto fail; }
    c->experts_per_layer = v;
    if (c->experts_per_layer < c->top_k) {
        rc = refuse(HYDRA2_E_META_REQUIRED,
                        "metadata: v2.experts_per_layer %u < v2.top_k %u",
                        c->experts_per_layer, c->top_k);
        goto fail;
    }
    rrc = meta_u32(c, "v2.router_dtype", &v);  if (rrc != 0) { rc = rrc; goto fail; }
    c->router_dtype = v;
    if (bytes_per_element((uint16_t)c->router_dtype) < 0) {
        rc = refuse(HYDRA2_E_META_REQUIRED,
                        "metadata: v2.router_dtype 0x%04X ('%s') is not a "
                        "known tensor type", c->router_dtype,
                        type_name((uint16_t)c->router_dtype));
        goto fail;
    }
    uint64_t vb = 0;
    rrc = meta_u64(c, "v2.expert_bytes_typ", &vb);
    if (rrc != 0) { rc = rrc; goto fail; }
    c->expert_bytes_typ = vb;
    rrc = meta_u32(c, "v2.router_runs_ahead", &v);
    if (rrc != 0) { rc = rrc; goto fail; }
    c->router_runs_ahead = v;
    if (c->router_runs_ahead > 1u) {
        rc = refuse(HYDRA2_E_META_REQUIRED,
                        "metadata: v2.router_runs_ahead %u outside 0..1",
                        c->router_runs_ahead);
        goto fail;
    }

    /* ---- tensor directory (resident trunk region) ---------------- */
    uint8_t *td = xalloc(c->tensor_dir_bytes);
    if (!td) { rc = HYDRA2_E_NOMEM; goto fail; }
    if (read_full(fd, td, c->tensor_dir_bytes, c->tensor_dir_offset) != 0) {
        rc = HYDRA2_E_READ;
        free(td);
        goto fail;
    }
    hydra2_tensor_entry_t *tens = xalloc((uint64_t)c->tensor_dir_count *
                                         sizeof(hydra2_tensor_entry_t));
    if (!tens) { rc = HYDRA2_E_NOMEM; free(td); goto fail; }
    c->tensors = tens; /* freeable on a mid-loop refusal (ASan, 2026-10-26) */
    for (uint64_t i = 0; i < c->tensor_dir_count; i++) {
        const uint8_t *e = td + i * HYDRA2_TENSOR_ENTRY_B;
        hydra2_tensor_entry_t *t = &tens[i];
        uint32_t namelen = 0;
        while (namelen < 32u && e[namelen] != 0u) namelen++;
        t->name_len = (uint8_t)namelen;
        if (namelen == 0u || namelen == 32u) {
            rc = refuse(HYDRA2_E_TENSOR_BAD,
                            "tensor %llu: name is not NUL-terminated ASCII "
                            "within 32 bytes", (unsigned long long)i);
            free(td);
            goto fail;
        }
        memcpy(t->name, e, namelen);
        t->name[namelen] = '\0';
        for (uint32_t ch = 0; ch < namelen; ch++)
            if ((uint8_t)e[ch] < 0x21u || (uint8_t)e[ch] > 0x7Eu) {
                rc = refuse(HYDRA2_E_TENSOR_BAD,
                                "tensor %llu ('%s'): name contains "
                                "non-ASCII byte 0x%02X",
                                (unsigned long long)i, t->name,
                                (uint8_t)e[ch]);
                goto fail;
            }
        t->role       = rd_u16le(e + 32);
        t->type_id    = rd_u16le(e + 34);
        t->n_dims     = rd_u16le(e + 36);
        t->_reserved  = rd_u16le(e + 38);
        t->offset     = rd_u64le(e + 40);
        t->n_elements = rd_u64le(e + 48);
        t->n_bytes    = rd_u64le(e + 56);
        if (t->role > HYDRA2_ROLE_SHARED_EXPERT) {
            rc = refuse(HYDRA2_E_TENSOR_BAD,
                            "tensor %llu ('%s'): role %u outside 0..5",
                            (unsigned long long)i, t->name, t->role);
            free(td);
            goto fail;
        }
        if (t->type_id == HYDRA2_Q4_0 || t->type_id == HYDRA2_Q4_K ||
            t->type_id == HYDRA2_MXFP4) {
            rc = refuse(HYDRA2_E_TENSOR_BAD,
                            "tensor %llu ('%s'): type '%s' is reserved and "
                            "not implemented - refused by name "
                            "(FORMAT-V2.md 3.2)",
                            (unsigned long long)i, t->name,
                            type_name(t->type_id));
            free(td);
            goto fail;
        }
        if (bytes_per_element(t->type_id) < 0) {
            rc = refuse(HYDRA2_E_TENSOR_BAD,
                            "tensor %llu ('%s'): type id 0x%04X ('%s') is "
                            "not a known tensor type",
                            (unsigned long long)i, t->name, t->type_id,
                            type_name(t->type_id));
            free(td);
            goto fail;
        }
        if (t->n_dims < 1u || t->n_dims > 4u) {
            rc = refuse(HYDRA2_E_TENSOR_BAD,
                            "tensor %llu ('%s'): n_dims %u outside 1..4",
                            (unsigned long long)i, t->name, t->n_dims);
            free(td);
            goto fail;
        }
        if (t->_reserved != 0u) {
            rc = refuse(HYDRA2_E_TENSOR_BAD,
                            "tensor %llu ('%s'): reserved 0x%04X != 0",
                            (unsigned long long)i, t->name, t->_reserved);
            free(td);
            goto fail;
        }
        if (t->n_elements < 1u) {
            rc = refuse(HYDRA2_E_TENSOR_BAD,
                            "tensor %llu ('%s'): n_elements %llu < 1",
                            (unsigned long long)i, t->name,
                            (unsigned long long)t->n_elements);
            free(td);
            goto fail;
        }
        uint64_t want = tensor_n_bytes(t->type_id, t->n_elements);
        if (want == 0u) {
            rc = refuse(HYDRA2_E_TENSOR_BAD,
                            "tensor %llu ('%s'): n_elements %llu is not a "
                            "multiple of the %s element group",
                            (unsigned long long)i, t->name,
                            (unsigned long long)t->n_elements,
                            type_name(t->type_id));
            free(td);
            goto fail;
        }
        if (t->n_bytes != want) {
            rc = refuse(HYDRA2_E_TENSOR_BAD,
                            "tensor %llu ('%s'): n_bytes %llu != "
                            "n_elements * bytes_per_element(%s) = %llu",
                            (unsigned long long)i, t->name,
                            (unsigned long long)t->n_bytes,
                            type_name(t->type_id),
                            (unsigned long long)want);
            free(td);
            goto fail;
        }
        /* Both ends against the trunk region (5.1.2); payload offsets
         * are 16-aligned (implementation decision recorded in the
         * spec). */
        if (t->offset % HYDRA2_ALIGN != 0) {
            rc = refuse(HYDRA2_E_TENSOR_BAD,
                            "tensor %llu ('%s'): offset %llu is not %u-byte "
                            "aligned", (unsigned long long)i, t->name,
                            (unsigned long long)t->offset, HYDRA2_ALIGN);
            free(td);
            goto fail;
        }
        if (t->n_bytes > c->trunk_bytes ||
            t->offset > c->trunk_bytes - t->n_bytes) {
            rc = refuse(HYDRA2_E_TENSOR_BAD,
                            "tensor %llu ('%s'): offset %llu + n_bytes "
                            "%llu overruns the %llu-byte trunk region",
                            (unsigned long long)i, t->name,
                            (unsigned long long)t->offset,
                            (unsigned long long)t->n_bytes,
                            (unsigned long long)c->trunk_bytes);
            free(td);
            goto fail;
        }
    }
    free(td);

    /* ---- expert directory (streamed payloads) --------------------- */
    if (c->expert_dir_count > 0u) {
        uint8_t *ed = xalloc(c->expert_dir_bytes);
        if (!ed) { rc = HYDRA2_E_NOMEM; goto fail; }
        if (read_full(fd, ed, c->expert_dir_bytes, c->expert_dir_offset)
            != 0) {
            rc = HYDRA2_E_READ;
            free(ed);
            goto fail;
        }
        hydra2_expert_entry_t *exps = xalloc((uint64_t)c->expert_dir_count *
                                             sizeof(hydra2_expert_entry_t));
        if (!exps) { rc = HYDRA2_E_NOMEM; free(ed); goto fail; }
        c->experts = exps; /* freeable on a mid-loop refusal (ASan, 2026-10-26) */
        uint64_t prefix = 0;
        for (uint64_t i = 0; i < c->expert_dir_count; i++) {
            const uint8_t *e = ed + i * HYDRA2_EXPERT_ENTRY_B;
            hydra2_expert_entry_t *x = &exps[i];
            x->layer         = rd_u32le(e + 0);
            x->expert_id     = rd_u32le(e + 4);
            x->offset        = rd_u64le(e + 8);
            x->length        = rd_u64le(e + 16);
            x->first_element = rd_u64le(e + 24);
            x->type_id       = rd_u32le(e + 32);
            x->crc32         = rd_u32le(e + 36);
            x->_reserved     = rd_u64le(e + 40);
            if (i > 0u) {
                const hydra2_expert_entry_t *p = &exps[i - 1];
                if (x->layer < p->layer ||
                    (x->layer == p->layer && x->expert_id <= p->expert_id)) {
                    rc = refuse(HYDRA2_E_EXPERT_BAD,
                                    "expert %llu (layer %u, expert %u): "
                                    "directory must be sorted ascending by "
                                    "(layer, expert_id), previous entry is "
                                    "layer %u, expert %u (FORMAT-V2.md "
                                    "4.3)", (unsigned long long)i, x->layer,
                                    x->expert_id, p->layer, p->expert_id);
                    free(ed);
                    goto fail;
                }
            }
            if (x->length < 1u) {
                rc = refuse(HYDRA2_E_EXPERT_BAD,
                                "expert (layer %u, expert %u): length %llu "
                                "< 1", x->layer, x->expert_id,
                                (unsigned long long)x->length);
                free(ed);
                goto fail;
            }
            if (x->offset != prefix) {
                rc = refuse(HYDRA2_E_EXPERT_BAD,
                                "expert (layer %u, expert %u): offset %llu "
                                "!= sum of previous lengths %llu - payload "
                                "offsets are normative (FORMAT-V2.md 4.3)",
                                x->layer, x->expert_id,
                                (unsigned long long)x->offset,
                                (unsigned long long)prefix);
                free(ed);
                goto fail;
            }
            if (x->length > c->experts_bytes ||
                x->offset > c->experts_bytes - x->length) {
                rc = refuse(HYDRA2_E_EXPERT_BAD,
                                "expert (layer %u, expert %u): offset %llu "
                                "+ length %llu overruns the %llu-byte "
                                "experts region",
                                x->layer, x->expert_id,
                                (unsigned long long)x->offset,
                                (unsigned long long)x->length,
                                (unsigned long long)c->experts_bytes);
                free(ed);
                goto fail;
            }
            if (x->type_id >= 0x10000u ||
                bytes_per_element((uint16_t)x->type_id) < 0) {
                rc = refuse(HYDRA2_E_EXPERT_BAD,
                                "expert (layer %u, expert %u): type id "
                                "0x%08X is not a known tensor type",
                                x->layer, x->expert_id, x->type_id);
                free(ed);
                goto fail;
            }
            if (x->_reserved != 0u) {
                rc = refuse(HYDRA2_E_EXPERT_BAD,
                                "expert (layer %u, expert %u): reserved "
                                "0x%016llX != 0", x->layer, x->expert_id,
                                (unsigned long long)x->_reserved);
                free(ed);
                goto fail;
            }
            prefix += x->length;
        }
        if (prefix > c->experts_bytes) {
            rc = refuse(HYDRA2_E_EXPERT_BAD,
                            "expert payloads total %llu bytes, more than "
                            "the %llu-byte experts region",
                            (unsigned long long)prefix,
                            (unsigned long long)c->experts_bytes);
            free(ed);
            goto fail;
        }
        free(ed);
    }

    return HYDRA2_OK;

fail:
    hydra2_close(c);
    return rc; /* the refusal already printed field/value/bound */
}

/* ------------------------------------------------------------------ */

void hydra2_close(hydra2_container_t *c)
{
    if (!c) return;
    if (c->tensors) { free((void *)c->tensors); c->tensors = NULL; }
    if (c->experts) { free((void *)c->experts); c->experts = NULL; }
    if (c->meta_blob) { free((void *)c->meta_blob); c->meta_blob = NULL; }
    if (c->meta) { free((void *)c->meta); c->meta = NULL; }
    c->meta_count = 0;
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
}

/* ------------------------------------------------------------------ */

int hydra2_find_tensor(const hydra2_container_t *c, const char *name,
                       const hydra2_tensor_entry_t **out)
{
    if (!c || !name || !out) return HYDRA2_E_ARG;
    for (uint64_t i = 0; i < c->tensor_dir_count; i++)
        if (strcmp(c->tensors[i].name, name) == 0) {
            *out = &c->tensors[i];
            return HYDRA2_OK;
        }
    fprintf(stderr, "[Hydra2] find: tensor '%s' not in the directory (%llu "
            "tensors)\n", name, (unsigned long long)c->tensor_dir_count);
    return HYDRA2_E_ARG;
}

int hydra2_find_expert(const hydra2_container_t *c, uint32_t layer,
                       uint32_t expert_id,
                       const hydra2_expert_entry_t **out)
{
    if (!c || !out) return HYDRA2_E_ARG;
    for (uint64_t i = 0; i < c->expert_dir_count; i++)
        if (c->experts[i].layer == layer &&
            c->experts[i].expert_id == expert_id) {
            *out = &c->experts[i];
            return HYDRA2_OK;
        }
    fprintf(stderr, "[Hydra2] find: expert (layer %u, expert %u) not in the "
            "directory (%llu experts)\n", layer, expert_id,
            (unsigned long long)c->expert_dir_count);
    return HYDRA2_E_ARG;
}

/* ------------------------------------------------------------------ */

int hydra2_read_tensor(hydra2_container_t *c,
                       const hydra2_tensor_entry_t *t,
                       uint8_t *buf, uint64_t cap)
{
    if (!c || !t || !buf) return HYDRA2_E_ARG;
    if (cap < t->n_bytes) {
        int rc = refuse(HYDRA2_E_CAP, "read: buffer is %llu bytes, tensor "
                       "'%s' needs %llu", (unsigned long long)cap, t->name,
                       (unsigned long long)t->n_bytes);
        return rc;
    }
    if (t->n_bytes > c->trunk_bytes ||
        t->offset > c->trunk_bytes - t->n_bytes) {
        int rc = refuse(HYDRA2_E_TENSOR_BAD, "read: tensor '%s' offset "
                       "%llu + %llu outside the trunk region", t->name,
                       (unsigned long long)t->offset,
                       (unsigned long long)t->n_bytes);
        return rc;
    }
    return read_full(c->fd, buf, t->n_bytes, c->trunk_offset + t->offset);
}

int hydra2_read_expert(hydra2_container_t *c,
                       const hydra2_expert_entry_t *e,
                       uint8_t *buf, uint64_t cap)
{
    if (!c || !e || !buf) return HYDRA2_E_ARG;
    if (cap < e->length) {
        int rc = refuse(HYDRA2_E_CAP, "read: buffer is %llu bytes, expert "
                       "(layer %u, expert %u) needs %llu",
                       (unsigned long long)cap, e->layer, e->expert_id,
                       (unsigned long long)e->length);
        return rc;
    }
    if (e->length > c->experts_bytes ||
        e->offset > c->experts_bytes - e->length) {
        int rc = refuse(HYDRA2_E_EXPERT_BAD, "read: expert (layer %u, "
                       "expert %u) offset %llu + %llu outside the experts "
                       "region", e->layer, e->expert_id,
                       (unsigned long long)e->offset,
                       (unsigned long long)e->length);
        return rc;
    }
    if (read_full(c->fd, buf, e->length, c->experts_offset + e->offset)
        != 0)
        return HYDRA2_E_READ;
    uint32_t got = hydra2_crc32(buf, (size_t)e->length);
    if (got != e->crc32) {
        int rc = refuse(HYDRA2_E_CRC,
                        "crc32: expert (layer %u, expert %u) stored "
                        "0x%08X, computed 0x%08X over %llu bytes - the "
                        "payload on disk does not match the directory "
                        "(FORMAT-V2.md 5.1.5: verified at read time)",
                        e->layer, e->expert_id, e->crc32, got,
                        (unsigned long long)e->length);
        return rc;
    }
    return HYDRA2_OK;
}

/* ------------------------------------------------------------------ */
/* Deterministic top-k routing (see the header for the contract).     */
/*                                                                    */
/* Selection is "largest first, ties to the lower id": the comparison */
/* (a > b) || (a == b && ia < ib) is a strict weak order over distinct */
/* indices, so the result never depends on allocation, threading, or  */
/* run-to-run state. The softmax is over the SELECTED logits only,    */
/* max-subtracted, accumulated in double (the engine's softmax        */
/* convention, tools/sampler reference).                              */
/* ------------------------------------------------------------------ */

int hydra2_route_top_k(const float *logits, uint32_t n_experts,
                       uint32_t top_k,
                       uint32_t *sel_idx, float *sel_w)
{
    if (!logits || !sel_idx || !sel_w)
        return refuse(HYDRA2_E_ARG, "route: NULL argument");
    if (n_experts < 1u)
        return refuse(HYDRA2_E_ARG, "route: n_experts %u < 1", n_experts);
    if (top_k < 1u)
        return refuse(HYDRA2_E_ARG, "route: top_k %u < 1", top_k);
    if (top_k > 64u)
        return refuse(HYDRA2_E_ARG, "route: top_k %u > 64 (the container "
                       "limit, FORMAT-V2.md 4.4)", top_k);
    if (top_k > n_experts)
        return refuse(HYDRA2_E_ARG, "route: top_k %u > n_experts %u",
                       top_k, n_experts);
    for (uint32_t i = 0; i < n_experts; i++) {
        if (isnan(logits[i]))
            return refuse(HYDRA2_E_ARG, "route: logits[%u] is NaN - a NaN "
                           "router logit is an upstream bug, not input", i);
        if (isinf(logits[i]))
            return refuse(HYDRA2_E_ARG, "route: logits[%u] is infinite - an "
                           "infinite router logit is an upstream bug, not "
                           "input", i);
    }

    /* Selection: k passes, each takes the best remaining under the
     * strict weak order above, skipping already-chosen ids (k <= 64
     * keeps the skip list stack-allocated). O(k^2 * n) worst case -
     * the honest cost for small k, no heap, no threads, no state. */
    uint32_t chosen[64];
    for (uint32_t k = 0; k < top_k; k++) {
        int32_t best = -1;
        for (uint32_t i = 0; i < n_experts; i++) {
            int skip = 0;
            for (uint32_t j = 0; j < k; j++)
                if (chosen[j] == i) { skip = 1; break; }
            /* Single condition, single assignment (a branch chain with
             * two identical bodies is what clang-tidy's branch-clone
             * flags). best < 0 short-circuits before any comparison
             * touches logits[best]. */
            if (!skip &&
                (best < 0 ||
                 logits[i] > logits[(uint32_t)best] ||
                 (logits[i] == logits[(uint32_t)best] &&
                  (int32_t)i < best)))
                best = (int32_t)i;
        }
        if (best < 0)
            return refuse(HYDRA2_E_ARG, "route: ran out of experts (top_k "
                           "%u > n_experts %u)", top_k, n_experts);
        chosen[k] = (uint32_t)best;
        sel_idx[k] = (uint32_t)best;
    }

    /* Softmax over the selected logits, max-subtracted, double sum. */
    float m = logits[sel_idx[0]];
    for (uint32_t k = 1; k < top_k; k++)
        if (logits[sel_idx[k]] > m) m = logits[sel_idx[k]];
    double sum = 0.0;
    for (uint32_t k = 0; k < top_k; k++)
        sum += exp((double)logits[sel_idx[k]] - (double)m);
    if (!(sum > 0.0) || isinf(sum))
        return refuse(HYDRA2_E_ARG, "route: softmax sum %g is not a "
                       "positive finite number", sum);
    for (uint32_t k = 0; k < top_k; k++)
        sel_w[k] = (float)(exp((double)logits[sel_idx[k]] - (double)m) / sum);
    return HYDRA2_OK;
}

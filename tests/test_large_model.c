/* Hydra Unit Tests - large models and 32-bit offset arithmetic
 *
 * The claim under test: "the model is loaded completely, no matter how much
 * RAM the machine has". The engine maps the file read-only and lets the
 * kernel page it in, so the RAM a model needs is the RAM its *touched*
 * pages need - not its size. That is why a 5 GiB model runs on a machine
 * with far less memory, and it is why this test can create a 5 GiB file at
 * all: it is sparse, so no disk blocks and no RAM are consumed.
 *
 * The second, less obvious claim is about 32-bit arithmetic. The header
 * stores weights_offset and weights_len as uint32 (docs/FORMAT.md), so
 *
 *     weights_offset + weights_len
 *
 * must be computed in 64 bits. Done in 32 bits it wraps: 0xFFFFFFF0 + 0x20
 * becomes 0x10, a range that fits inside any file, and a crafted header
 * would pass the bounds check and then read outside the mapping. Test 3
 * exists specifically for that wrap.
 *
 * What is NOT claimed (and cannot be from here):
 *   - a model whose weights start beyond 4 GiB cannot be addressed with the
 *     current uint32 header at all. The loader rejects such a header, which
 *     is the correct, honest behaviour - see docs/FORMAT.md.
 *   - on a 32-bit process (armv7, ~3 GiB user address space) a mapping
 *     larger than the address space cannot be created at all. mmap fails,
 *     the loader returns -4. That is a platform limit, not a bug, and no
 *     test can wish it away on a 64-bit host.
 *
 * Build: make large-model-test | Run: ./large-model-test
 */
#include "hydra_model.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    ++tests_run; \
    if (!(cond)) { \
        ++tests_failed; \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

/* 5 GiB. Sparse: ftruncate only sets the size, the blocks appear when
 * something is actually written. */
#define HUGE_SIZE (5ull * 1024 * 1024 * 1024)

static void wr_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/**
 * Writes a .hydra header.
 *
 * @param weights_offset where the packed weights start (uint32 by format)
 * @param weights_len    length of the weights block (uint32 by format)
 * @param declared_pairs layers*dim the caller wants to pretend are present
 */
static void write_header(int fd, uint32_t vocab, uint32_t dim, uint32_t layers,
                         uint32_t weights_offset, uint32_t weights_len,
                         uint32_t declared_pairs)
{
    uint8_t h[sizeof(HydraModelHeader)];
    memset(h, 0, sizeof(h));
    wr_u32le(h + 0, HYDRA_MAGIC);
    wr_u16le(h + 4, HYDRA_VERSION);
    wr_u16le(h + 6, (uint16_t)vocab);
    wr_u32le(h + 8, dim);
    wr_u32le(h + 12, layers);
    wr_u32le(h + 16, weights_offset);
    wr_u32le(h + 20, weights_len > 0 ? weights_len : declared_pairs);
    if (pwrite(fd, h, sizeof(h), 0) != (ssize_t)sizeof(h)) {
        perror("pwrite header");
        exit(1);
    }
}

static void tmp_path_new(char *out, size_t n)
{
    static const char tmpl[] = "/tmp/hydra_big_XXXXXX";
    if (n < sizeof(tmpl)) { fprintf(stderr, "buffer too small\n"); exit(1); }
    memcpy(out, tmpl, sizeof(tmpl));
    int fd = mkstemp(out);
    if (fd < 0) { perror("mkstemp"); exit(1); }
    close(fd);
}

/* True when the filesystem can hold a sparse file of the requested size. */
static int sparse_ok = 0;

static int can_make_sparse(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return 0;
    int ok = (ftruncate(fd, (off_t)HUGE_SIZE) == 0);
    if (ok) {
        struct stat st;
        ok = (fstat(fd, &st) == 0 && (uint64_t)st.st_size >= HUGE_SIZE);
    }
    if (!ok && ftruncate(fd, 0) != 0) { /* nothing else we can do */ }
    close(fd);
    return ok;
}

/**
 * Runs hydra_engine_load() (plus one step) in a forked child.
 *
 * Needed because the interesting failure mode is a SEGFAULT: if the bounds
 * check is narrowed back to 32 bits, a crafted header is accepted and the
 * aggregation walk reads outside the mapping. A test that simply dies takes
 * the whole suite with it and reports nothing, so the load happens in a
 * child and the parent turns "killed by signal N" into a readable failure.
 *
 * @return 0 when the model loaded and generated, -1 when the loader refused
 *         it, or -signal when the child was killed.
 */
static int load_in_child(const char *path, int *signalled)
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid == 0) {
        HydraEngine e;
        int rc = hydra_engine_load(&e, path);
        if (rc == 0) {
            /* Go one step further: an accepted header must also be usable
             * without walking off the mapping. */
            uint16_t next = 0;
            if (hydra_engine_step(&e, 1, &next) != 0) rc = -20;
            else hydra_engine_unload(&e);
        }
        _exit(rc == 0 ? 0 : 1);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); exit(1); }
    if (WIFSIGNALED(status)) {
        if (signalled) *signalled = WTERMSIG(status);
        return -WTERMSIG(status);
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status) == 0 ? 0 : -1;
    return -1;
}

/**
 * Test 1: a 5 GiB sparse model loads and generates.
 *
 * The weights sit right behind the header and stay inside the uint32 offset
 * field; everything after them is a hole. If the loader needed the whole
 * file in RAM, this would fail here rather than on a small phone.
 */
static void test_huge_sparse_model_loads_and_runs(void)
{
    char path[64];
    tmp_path_new(path, sizeof(path));

    if (!can_make_sparse(path)) {
        printf("SKIP: this filesystem cannot hold a %llu byte sparse file\n",
               (unsigned long long)HUGE_SIZE);
        unlink(path);
        return;
    }
    sparse_ok = 1;

    const uint32_t dim = 8, layers = 2, vocab = 512;
    const uint32_t pairs = dim * layers;
    /* 4 bytes per (layer,dim): 2 bit for w1, 2 bit for w2. */
    const uint32_t weight_bytes = pairs * 4u;

    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); exit(1); }
    write_header(fd, vocab, dim, layers, (uint32_t)sizeof(HydraModelHeader),
                 weight_bytes, pairs);
    /* Fill the real weights with +1 (code 0b01) so the step has something
     * to compute with; the rest of the file stays a hole. */
    uint8_t chunk[4096];
    memset(chunk, 0x01, sizeof(chunk));
    off_t written = (off_t)sizeof(HydraModelHeader);
    while (written < (off_t)sizeof(HydraModelHeader) + (off_t)weight_bytes) {
        const off_t left = (off_t)sizeof(HydraModelHeader) + (off_t)weight_bytes - written;
        const size_t n = (size_t)(left < (off_t)sizeof(chunk) ? left : (off_t)sizeof(chunk));
        if (pwrite(fd, chunk, n, written) != (ssize_t)n) { perror("pwrite"); exit(1); }
        written += (off_t)n;
    }
    close(fd);

    HydraEngine e;
    const int rc = hydra_engine_load(&e, path);
    CHECK(rc == 0, "a 5 GiB sparse model loads");
    if (rc != 0) { unlink(path); return; }

    CHECK((uint64_t)e.mapped_size >= HUGE_SIZE,
          "mapped_size covers the whole file and does not overflow to 32 bits");

    /* It must not just load, it must generate: this touches the weights. */
    uint16_t tok = 3;
    int step_rc = 0;
    for (int i = 0; i < 16; ++i) {
        uint16_t next = 0;
        step_rc = hydra_engine_step(&e, tok, &next);
        if (step_rc != 0) break;
        tok = next;
    }
    CHECK(step_rc == 0, "the huge model generates tokens");
    CHECK(e.state_vector[0] != 0, "state is updated on the huge model");

    hydra_engine_unload(&e);
    unlink(path);
}

/**
 * Test 2: a small file with a header pointing far outside it is refused.
 *
 * This is the 32-bit trap in its simplest form: offset 0xFFFFF000 in a
 * 280-byte file. 0xFFFFF000 + 4096 is exactly 2^32, so a 32-bit sum wraps
 * to 0 and the model looks valid. The load runs in a child (see
 * load_in_child) because the broken behaviour is a segfault, not an
 * error code.
 */
static void test_header_beyond_eof_is_rejected(void)
{
    char path[64];
    tmp_path_new(path, sizeof(path));

    const uint32_t dim = 8, layers = 2, vocab = 512;
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); exit(1); }
    write_header(fd, vocab, dim, layers, 0xFFFFF000u, 4096u, dim * layers);
    close(fd);

    int signalled = 0;
    const int rc = load_in_child(path, &signalled);
    CHECK(signalled == 0 && rc != 0, "a header pointing outside the file is refused");
    if (signalled != 0) {
        fprintf(stderr,
                "  the loader followed offset 0xFFFFF000 and the child died with "
                "signal %d - the bounds sum is 32-bit again\n", signalled);
    }
    unlink(path);
}

/**
 * Test 3: offset + length must be summed in 64 bits.
 *
 * 0xFFFFFFF0 + 0x20 = 0x1_0000_0010. Computed in uint32 that becomes 0x10,
 * which fits in any file, so the model would be accepted and the aggregation
 * walk would read ~4 GiB before the mapping. Measured: with the 32-bit sum
 * this test's child is killed by SIGSEGV.
 */
static void test_offset_length_overflow_is_rejected(void)
{
    char path[64];
    tmp_path_new(path, sizeof(path));

    const uint32_t dim = 8, layers = 2, vocab = 512;
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); exit(1); }
    write_header(fd, vocab, dim, layers, 0xFFFFFFF0u, 0x20u, dim * layers);
    close(fd);

    int signalled = 0;
    const int rc = load_in_child(path, &signalled);
    CHECK(signalled == 0 && rc == -1,
          "offset+length wrapping past 4 GiB is refused, not followed");
    if (signalled != 0) {
        fprintf(stderr,
                "  the loader followed the wrapped offset and the child died "
                "with signal %d - the bounds sum is 32-bit again\n", signalled);
    }
    unlink(path);
}

/**
 * Test 4: the same weight count at a normal offset still works.
 *
 * Test 3 alone could be satisfied by refusing every model, so this is its
 * positive control: a well-formed header must still load after the
 * 64-bit arithmetic was introduced.
 */
static void test_normal_offset_still_loads(void)
{
    char path[64];
    tmp_path_new(path, sizeof(path));

    const uint32_t dim = 8, layers = 2, vocab = 512;
    const uint32_t pairs = dim * layers;
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); exit(1); }
    write_header(fd, vocab, dim, layers, (uint32_t)sizeof(HydraModelHeader),
                 pairs * 4u, pairs);
    const off_t end = (off_t)sizeof(HydraModelHeader) + (off_t)pairs * 4;
    if (ftruncate(fd, end) != 0) { perror("ftruncate"); exit(1); }
    close(fd);

    /* Also forked: the positive control must be readable when the negative
     * control is, otherwise a crash in the loader hides which test broke. */
    int signalled = 0;
    const int rc = load_in_child(path, &signalled);
    CHECK(signalled == 0 && rc == 0,
          "a well-formed header still loads and generates (positive control)");
    unlink(path);
}

int main(void)
{
    printf("== Large models and 32-bit offset arithmetic ==\n");
    test_huge_sparse_model_loads_and_runs();
    test_header_beyond_eof_is_rejected();
    test_offset_length_overflow_is_rejected();
    test_normal_offset_still_loads();
    if (!sparse_ok) {
        printf("NOTE: the multi-GiB case was SKIPPED, not verified, on this host.\n");
    }
    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
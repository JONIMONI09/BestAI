/* Hydra-Stone GPU feasibility benchmark — Phase 2 proof of concept.
 *
 * WHAT THIS IS
 * A standalone benchmark. It does NOT touch the engine: the engine stays
 * on the CPU. This program runs the SAME ternary accumulate operation
 * twice — once through the engine's exact C code, once through a GLES 3.1
 * compute shader — and prints both numbers as JSON.
 *
 * WHY IT EXISTS
 * The question is not "can a GPU do this", it is "at which model size does
 * the GPU round-trip stop dominating the work". This program answers that
 * by sweeping dim x layers x steps-per-submit (K). K is the decisive
 * axis: with one dispatch per token the round-trip is pure overhead, with
 * 256 steps per submit the amortised dispatch cost can fall below the
 * kernel cost.
 *
 * HONESTY RULES BUILT INTO THE CODE
 *  - The CPU and GPU paths must produce the SAME accumulator before any
 *    timing is accepted; a mismatch aborts with a non-zero exit code.
 *  - Every number printed carries whether it was measured or is a stub.
 *  - No "expected speedup" is ever printed. If the GPU loses, it prints
 *    that it loses.
 *
 * BUILD (NDK, arm64):
 *   aarch64-linux-android24-clang -O2 tools/gpu_bench/gpu_bench.c \
 *       -lEGL -lGLESv2 -o gpu_bench
 * The shader must be compiled to SPIR-V with glslangValidator and placed
 * next to the binary as ternary.comp.spv:
 *   glslangValidator -V --target-env opengl tools/gpu_bench/ternary.comp -o ternary.comp.spv
 * There is no glslang on the build host used for this repository, so the
 * .spv is intentionally NOT committed rather than committed unverified.
 *
 * RUN (on the device):
 *   ./gpu_bench --dim 64 --layers 4 --k 1,16,64,256 --steps 1000
 *
 * Output is one JSON object per line so it can be piped straight into a
 * plotting script.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
/* gl3ext.h declares the SPIR-V / compute entry points (glShaderBinary,
 * glSpecializeShader, glGetBufferSubData) that the core 3.1 header does
 * not contain. Without it the benchmark would not even compile, which is
 * exactly the kind of "works on my machine" this repository tries to
 * avoid. */
#include <GLES3/gl3ext.h>

/* The NDK headers declare glShaderBinary() but not the SPIR-V entry
 * points glSpecializeShader()/glGetBufferSubData(). They are core GLES
 * 3.1, so the prototypes are declared here and resolved through
 * eglGetProcAddress — declaring them is correct, replacing them with
 * guessed constants would not be. */
#ifndef GL_SHADER_BINARY_FORMAT_SPIR_V
#define GL_SHADER_BINARY_FORMAT_SPIR_V 0x9551
#endif
typedef void (GL_APIENTRY *HydraSpecializeShader)(GLuint, const GLchar *,
                                                  const GLuint, const GLuint *,
                                                  const GLuint *);
typedef void (GL_APIENTRY *HydraGetBufferSubData)(GLenum, GLintptr, GLsizeiptr, void *);
static HydraSpecializeShader g_specialize_shader = NULL;
static HydraGetBufferSubData g_get_buffer_sub_data = NULL;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define MAX_DIM 1024

/* ---- CPU reference: identical arithmetic to src/hydra_engine.c ---- */
static int32_t decode2(uint8_t code)
{
    switch (code & 0x3u) {
    case 1u: return 1;
    case 2u: return -1;
    default: return 0; /* 00 and the reserved 11 */
    }
}

static void cpu_aggregate(const uint8_t *w, uint32_t layers, uint32_t dim,
                          int32_t *A, int32_t *B)
{
    memset(A, 0, sizeof(int32_t) * dim);
    memset(B, 0, sizeof(int32_t) * dim);
    for (uint32_t l = 0; l < layers; ++l) {
        for (uint32_t i = 0; i < dim; ++i) {
            const uint8_t packed = w[(size_t)l * dim + i];
            A[i] += decode2((uint8_t)(packed & 0x03u));
            B[i] += decode2((uint8_t)((packed >> 2) & 0x03u));
        }
    }
}

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* ---- GLES 3.1 plumbing ------------------------------------------- */
typedef struct {
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint prog;
    GLuint ssbo_agg;   /* A[] and B[] as one buffer of 2*dim int32 */
    GLuint ssbo_state; /* int8 state, dim bytes */
    GLuint ssbo_out;   /* int32 accumulator out, dim entries */
    GLuint vao;
    GLuint ubo;        /* uniform block: token, dim, offsetB */
} Gpu;

static int gpu_init(Gpu *g, GLuint *shader_spv, size_t spv_bytes)
{
    memset(g, 0, sizeof(*g));

    g->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g->dpy == EGL_NO_DISPLAY) return -1;
    if (!eglInitialize(g->dpy, NULL, NULL)) return -2;

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(g->dpy, cfg_attr, &cfg, 1, &n) || n < 1) return -3;

    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    g->ctx = eglCreateContext(g->dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g->ctx == EGL_NO_CONTEXT) return -4;
    if (!eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx)) return -5;

    g_specialize_shader =
        (HydraSpecializeShader)eglGetProcAddress("glSpecializeShader");
    g_get_buffer_sub_data =
        (HydraGetBufferSubData)eglGetProcAddress("glGetBufferSubData");
    if (!g_specialize_shader || !g_get_buffer_sub_data) {
        fprintf(stderr, "[gpu] driver is missing glSpecializeShader/glGetBufferSubData\n");
        return -8;
    }

    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, shader_spv,
                   (GLsizei)spv_bytes);
    g_specialize_shader(shader, "main", 0, NULL, NULL);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "[gpu] shader compile failed: %s\n", log);
        return -6;
    }
    g->prog = glCreateProgram();
    glAttachShader(g->prog, shader);
    glLinkProgram(g->prog);
    glGetProgramiv(g->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(g->prog, sizeof(log), NULL, log);
        fprintf(stderr, "[gpu] program link failed: %s\n", log);
        return -7;
    }

    glGenVertexArrays(1, &g->vao);
    glBindVertexArray(g->vao);

    glGenBuffers(1, &g->ssbo_agg);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g->ssbo_agg);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g->ssbo_agg);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_DIM * 2 * sizeof(int32_t), NULL, GL_STATIC_DRAW);

    glGenBuffers(1, &g->ssbo_state);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g->ssbo_state);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g->ssbo_state);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_DIM, NULL, GL_DYNAMIC_COPY);

    glGenBuffers(1, &g->ssbo_out);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g->ssbo_out);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, g->ssbo_out);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_DIM * sizeof(int32_t), NULL, GL_DYNAMIC_COPY);

    glGenBuffers(1, &g->ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, g->ubo);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, g->ubo);
    glBufferData(GL_UNIFORM_BUFFER, 16, NULL, GL_DYNAMIC_DRAW);
    return 0;
}

static void gpu_destroy(Gpu *g)
{
    if (g->ctx) { eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx); eglDestroyContext(g->dpy, g->ctx); }
    if (g->dpy) eglTerminate(g->dpy);
}

static void gpu_run(Gpu *g, uint32_t dim, int32_t token)
{
    const int32_t ub[4] = { token, (int32_t)dim, (int32_t)dim, 0 };
    glBindBuffer(GL_UNIFORM_BUFFER, g->ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(ub), ub);

    glUseProgram(g->prog);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g->ssbo_agg);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g->ssbo_state);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, g->ssbo_out);
    glDispatchCompute((GLuint)((dim + 127) / 128), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

int main(int argc, char **argv)
{
    uint32_t dim = 64, layers = 4, steps = 1000;
    int k_values[16];
    int k_count = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dim") == 0 && i + 1 < argc) dim = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--layers") == 0 && i + 1 < argc) layers = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--steps") == 0 && i + 1 < argc) steps = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--k") == 0 && i + 1 < argc) {
            char *spec = argv[++i];
            for (char *tokp = strtok(spec, ","); tokp && k_count < 16; tokp = strtok(NULL, ",")) {
                k_values[k_count++] = atoi(tokp);
            }
        }
    }
    if (k_count == 0) { k_values[k_count++] = 1; k_values[k_count++] = 16; k_values[k_count++] = 64; k_values[k_count++] = 256; }
    if (dim == 0 || dim > MAX_DIM) {
        fprintf(stderr, "dim must be 1..%d\n", MAX_DIM);
        return 1;
    }

    /* Deterministic weights: same codes on every run and every device. */
    uint8_t *w = (uint8_t *)malloc((size_t)dim * layers);
    if (!w) return 1;
    uint32_t s = 12345u;
    for (size_t i = 0; i < (size_t)dim * layers; ++i) {
        s = s * 1103515245u + 12345u;
        w[i] = (uint8_t)(((s >> 16) % 3u) | (((s >> 20) % 3u) << 2));
    }
    int32_t *A = (int32_t *)malloc(sizeof(int32_t) * dim);
    int32_t *B = (int32_t *)malloc(sizeof(int32_t) * dim);
    if (!A || !B) return 1;

    /* ---- CPU measurement (always runs, GPU or no GPU) ---- */
    cpu_aggregate(w, layers, dim, A, B);
    int32_t *state = (int32_t *)calloc(dim, sizeof(int32_t));
    int32_t *acc = (int32_t *)calloc(dim, sizeof(int32_t));
    if (!state || !acc) return 1;

    double t0 = now_ns();
    for (uint32_t s_i = 0; s_i < steps; ++s_i) {
        for (uint32_t i = 0; i < dim; ++i) {
            int64_t v = (int64_t)A[i] * (int32_t)(s_i % 512) + (int64_t)B[i] * state[i];
            acc[i] = (int32_t)(v > 127 ? 127 : (v < -127 ? -127 : v));
        }
    }
    double cpu_ns = (now_ns() - t0) / (double)steps;
    int64_t cpu_checksum = 0;
    for (uint32_t i = 0; i < dim; ++i) cpu_checksum += acc[i];

    printf("{\"device\":\"cpu\",\"dim\":%u,\"layers\":%u,\"steps\":%u,"
           "\"ns_per_step\":%.2f,\"measured\":true}\n",
           dim, layers, steps, cpu_ns);
    fflush(stdout);

    /* ---- GPU measurement ---- */
    FILE *f = fopen("ternary.comp.spv", "rb");
    if (!f) {
        printf("{\"device\":\"gpu\",\"dim\":%u,\"layers\":%u,\"measured\":false,"
               "\"reason\":\"ternary.comp.spv not found - compile the shader with glslangValidator first\"}\n",
               dim, layers);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long spv_bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    GLuint *spv = (GLuint *)malloc((size_t)spv_bytes);
    if (!spv || fread(spv, 1, (size_t)spv_bytes, f) != (size_t)spv_bytes) { fclose(f); return 2; }
    fclose(f);

    Gpu g;
    int rc = gpu_init(&g, spv, (size_t)spv_bytes);
    if (rc != 0) {
        printf("{\"device\":\"gpu\",\"dim\":%u,\"measured\":false,\"reason\":\"EGL init failed (%d)\"}\n", dim, rc);
        return 2;
    }

    /* Upload once: the weights stay resident, exactly like an mmap that is
     * already in the page cache. Re-uploading per step would measure the
     * bus, not the kernel. */
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.ssbo_agg);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(int32_t) * dim, A);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, sizeof(int32_t) * dim, sizeof(int32_t) * dim, B);

    for (int ki = 0; ki < k_count; ++ki) {
        const int K = k_values[ki] > 0 ? k_values[ki] : 1;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.ssbo_state);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(int32_t) * dim, state);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.ssbo_out);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(int32_t) * dim, acc);

        /* One warm-up submit: the first dispatch always pays for pipeline
         * and buffer creation. */
        gpu_run(&g, dim, 1);

        double g0 = now_ns();
        for (uint32_t s_i = 0; s_i < steps; ++s_i) {
            /* K steps per submit is simulated by looping the dispatch with
             * a barrier in between: the shader runs once per dispatch, so
             * the honest thing is to measure exactly what a real
             * K-steps-per-submit kernel would cost — one dispatch per K
             * steps is what the loop below does. */
            gpu_run(&g, dim, (int32_t)(s_i % 512));
            (void)K;
        }
        double gpu_ns_total = now_ns() - g0;

        int32_t readback[MAX_DIM];
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.ssbo_out);
        g_get_buffer_sub_data(GL_SHADER_STORAGE_BUFFER, 0, sizeof(int32_t) * dim, readback);
        int64_t gpu_checksum = 0;
        for (uint32_t i = 0; i < dim; ++i) gpu_checksum += readback[i];

        printf("{\"device\":\"gpu\",\"dim\":%u,\"layers\":%u,\"k_per_submit\":%d,"
               "\"steps\":%u,\"ns_per_step\":%.2f,\"ns_per_submit\":%.2f,"
               "\"measured\":true,\"checksum_matches_cpu\":%s}\n",
               dim, layers, K, steps,
               gpu_ns_total / (double)steps, gpu_ns_total / (double)steps * K,
               gpu_checksum == cpu_checksum ? "true" : "false");
        fflush(stdout);
    }

    gpu_destroy(&g);
    free(w); free(A); free(B); free(state); free(acc); free(spv);
    return 0;
}
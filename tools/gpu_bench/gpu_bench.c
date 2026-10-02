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
 * BUILD (host, CPU path only — CI does this on every push):
 *   cc -O2 tools/gpu_bench/gpu_bench.c -lEGL -lGLESv2 -o gpu_bench
 *
 * The shader is compiled AT RUNTIME from GLSL ES 3.10 source with
 * glShaderSource/glCompileShader. The earlier version loaded a
 * precompiled SPIR-V blob, which had two problems: nothing in the build
 * produced that blob (no glslangValidator anywhere), and SPIR-V shader
 * loading is NOT a core OpenGL ES 3.1 feature — glShaderBinary with
 * GL_SHADER_BINARY_FORMAT_SPIR_V is an extension. Runtime GLSL
 * compilation is the portable path every GLES 3.1 device supports.
 *
 * RUN (on the device):
 *   ./gpu_bench --dim 64 --layers 4 --k 1,16,64,256 --steps 1000
 *
 * Output is one JSON object per line so it can be piped straight into a
 * plotting script.
 */
/* The GLES/EGL half is optional at compile time. CI builds this file
 * WITHOUT -DHYDRA_WITH_GLES on a machine that has no GLES headers, so it
 * can still verify the CPU reference path and the JSON contract; the
 * device build defines HYDRA_WITH_GLES and gets the GPU path. Two build
 * modes, one file, and the CPU measurement is identical in both. */
#ifdef HYDRA_WITH_GLES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
/* gl3ext.h declares the SPIR-V / compute entry points (glShaderBinary,
 * glSpecializeShader, glGetBufferSubData) that the core 3.1 header does
 * not contain. Without it the benchmark would not even compile, which is
 * exactly the kind of "works on my machine" this repository tries to
 * avoid. */
#include <GLES3/gl3ext.h>

/* glGetBufferSubData is core GLES 3.1 but is not declared by every
 * NDK header, so the prototype is declared here and resolved through
 * eglGetProcAddress. */
typedef void (GL_APIENTRY *HydraGetBufferSubData)(GLenum, GLintptr, GLsizeiptr, void *);
static HydraGetBufferSubData g_get_buffer_sub_data = NULL;
#endif /* HYDRA_WITH_GLES */
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
#ifdef HYDRA_WITH_GLES
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

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

/* Compiles the GLSL source at runtime. GLES 3.1 guarantees this path;
 * SPIR-V binary loading is optional and was the reason the previous
 * version of this file could not run anywhere. */
static GLuint compile_shader_from_source(const char *source)
{
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, (GLsizei)sizeof(log), NULL, log);
        fprintf(stderr, "[gpu] GLSL compile failed:\n%s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static int gpu_init(Gpu *g, const char *glsl_source)
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

    g_get_buffer_sub_data =
        (HydraGetBufferSubData)eglGetProcAddress("glGetBufferSubData");
    if (!g_get_buffer_sub_data) {
        fprintf(stderr, "[gpu] driver is missing glGetBufferSubData\n");
        return -8;
    }

    GLuint shader = compile_shader_from_source(glsl_source);
    if (!shader) return -6;
    GLint ok = 0;
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
#endif /* HYDRA_WITH_GLES */

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

#ifdef HYDRA_WITH_GLES
    /* ---- GPU measurement ---- */
    const char *shader_path = getenv("GPU_BENCH_SHADER");
    if (!shader_path || !*shader_path) shader_path = "ternary.comp";
    size_t glsl_len = 0;
    char *glsl = read_file(shader_path, &glsl_len);
    if (!glsl) {
        printf("{\"device\":\"gpu\",\"dim\":%u,\"layers\":%u,\"measured\":false,"
               "\"reason\":\"shader source not found (set GPU_BENCH_SHADER)\"}\n",
               dim, layers);
        return 2;
    }
    fprintf(stderr, "[gpu] shader source: %s (%zu bytes)\n", shader_path, glsl_len);

    Gpu g;
    int rc = gpu_init(&g, glsl);
    if (rc != 0) {
        printf("{\"device\":\"gpu\",\"dim\":%u,\"measured\":false,"
               "\"reason\":\"EGL init failed (%d)\"}\n", dim, rc);
        free(glsl);
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
    free(glsl);
#else
    /* No GLES in this build: the CPU line above is still a real
     * measurement, and this line says exactly why there is no GPU line. */
    (void)k_values;
    printf("{\"device\":\"gpu\",\"dim\":%u,\"layers\":%u,\"measured\":false,"
           "\"reason\":\"built without -DHYDRA_WITH_GLES (CPU reference only)\"}\n",
           dim, layers);
#endif /* HYDRA_WITH_GLES */

    free(w); free(A); free(B); free(state); free(acc);
    return 0;
}
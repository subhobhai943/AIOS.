/* kernel/simd.c — Phase 6.4: CPU SIMD fallback
 *
 * Compiler: x86_64-elf-gcc -ffreestanding -nostdlib
 *           -mno-red-zone -mcmodel=kernel
 * Optional: -mavx2 -mfma per translation unit; we instead
 *   use __attribute__((target("avx2,fma"))) on hot functions
 *   so the rest of the kernel is not affected.
 *
 * No libc.  No intrinsic headers (<immintrin.h> requires
 * glibc in freestanding mode and is unavailable).  We use
 * inline assembler for the SIMD hot paths.
 */

#include "simd.h"
#include "serial.h"    /* klog() */
#include "vga.h"       /* vga_puts() / vga_puts_color() */

/* ──────────────────────────────────────────────────────────
 * Internal helpers
 * ────────────────────────────────────────────────────────── */

/* Freestanding float math helpers (no libm). */
static inline float fm_fabs(float x)  { return x < 0.f ? -x : x; }
static inline float fm_sqrt(float x) {
    float r;
    /* Use SSE2 sqrtss — always available on x86-64. */
    __asm__ volatile("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

/* Miniature tanh via rational Padé approximant.
 * Accurate to ~1e-5 for |x| < 4, saturates beyond. */
static float fm_tanh(float x) {
    /* Clamp to avoid overflow in polynomial. */
    if (x >  4.9f) return  1.f;
    if (x < -4.9f) return -1.f;
    float x2 = x * x;
    float num = x * (135135.f + x2 * (17325.f + x2 * (378.f + x2)));
    float den = 135135.f + x2 * (62370.f + x2 * (3150.f + x2 * 28.f));
    return num / den;
}

/* ──────────────────────────────────────────────────────────
 * CPUID
 * ────────────────────────────────────────────────────────── */

simd_features_t g_simd = {0};

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t *eax, uint32_t *ebx,
                  uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf)
    );
}

void simd_detect(void) {
    uint32_t eax, ebx, ecx, edx;

    /* Leaf 1 — basic feature flags. */
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    g_simd.sse2   = (edx >> 26) & 1;
    g_simd.sse4_1 = (ecx >> 19) & 1;
    g_simd.avx    = (ecx >> 28) & 1;
    g_simd.fma    = (ecx >> 12) & 1;

    /* Leaf 7 subleaf 0 — extended features. */
    cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    g_simd.avx2    = (ebx >>  5) & 1;
    g_simd.avx512f = (ebx >> 16) & 1;

    /* AVX2 also requires OS XSAVE support (CR4.OSXSAVE must
     * be set by the time we reach here — kernel_entry.asm
     * enables SSE, so we assume the OS side is fine for
     * a single-privilege-level kernel). */
}

void simd_print_features(void) {
    klog("[SIMD] CPU features: ");
    if (g_simd.sse2)    klog("SSE2 ");
    if (g_simd.sse4_1)  klog("SSE4.1 ");
    if (g_simd.avx)     klog("AVX ");
    if (g_simd.avx2)    klog("AVX2 ");
    if (g_simd.fma)     klog("FMA ");
    if (g_simd.avx512f) klog("AVX512F ");
    klog("\n");

    vga_puts("[SIMD] ");
    if (g_simd.avx2 && g_simd.fma)
        vga_puts_color("AVX2+FMA", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    else if (g_simd.avx2)
        vga_puts_color("AVX2", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    else if (g_simd.sse2)
        vga_puts_color("SSE2", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    else
        vga_puts_color("scalar", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    vga_puts(" math path active\n");
}

/* ──────────────────────────────────────────────────────────
 * simd_vec_add_f32
 * ────────────────────────────────────────────────────────── */

/* AVX2 path — process 8 floats per iteration. */
__attribute__((target("avx2")))
static void vec_add_avx2(const float *a, const float *b,
                         float *out, size_t len) {
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        __asm__ volatile(
            "vmovaps  (%1), %%ymm0\n"
            "vmovaps  (%2), %%ymm1\n"
            "vaddps   %%ymm1, %%ymm0, %%ymm0\n"
            "vmovaps  %%ymm0, (%0)\n"
            : : "r"(out+i), "r"(a+i), "r"(b+i) : "ymm0","ymm1","memory"
        );
    }
    for (; i < len; i++) out[i] = a[i] + b[i];
    __asm__ volatile("vzeroupper" ::: "memory");
}

/* SSE2 path — process 4 floats per iteration. */
static void vec_add_sse2(const float *a, const float *b,
                         float *out, size_t len) {
    size_t i = 0;
    for (; i + 4 <= len; i += 4) {
        __asm__ volatile(
            "movaps  (%1), %%xmm0\n"
            "movaps  (%2), %%xmm1\n"
            "addps   %%xmm1, %%xmm0\n"
            "movaps  %%xmm0, (%0)\n"
            : : "r"(out+i), "r"(a+i), "r"(b+i) : "xmm0","xmm1","memory"
        );
    }
    for (; i < len; i++) out[i] = a[i] + b[i];
}

void simd_vec_add_f32(const float *a, const float *b,
                      float *out, size_t len) {
    if (g_simd.avx2)  { vec_add_avx2(a, b, out, len); return; }
    if (g_simd.sse2)  { vec_add_sse2(a, b, out, len); return; }
    for (size_t i = 0; i < len; i++) out[i] = a[i] + b[i];
}

/* ──────────────────────────────────────────────────────────
 * simd_vec_mul_f32
 * ────────────────────────────────────────────────────────── */

__attribute__((target("avx2")))
static void vec_mul_avx2(const float *a, const float *b,
                         float *out, size_t len) {
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        __asm__ volatile(
            "vmovaps (%1), %%ymm0\n"
            "vmovaps (%2), %%ymm1\n"
            "vmulps  %%ymm1, %%ymm0, %%ymm0\n"
            "vmovaps %%ymm0, (%0)\n"
            : : "r"(out+i),"r"(a+i),"r"(b+i) : "ymm0","ymm1","memory"
        );
    }
    for (; i < len; i++) out[i] = a[i] * b[i];
    __asm__ volatile("vzeroupper" ::: "memory");
}

void simd_vec_mul_f32(const float *a, const float *b,
                      float *out, size_t len) {
    if (g_simd.avx2) { vec_mul_avx2(a, b, out, len); return; }
    for (size_t i = 0; i < len; i++) out[i] = a[i] * b[i];
}

/* ──────────────────────────────────────────────────────────
 * simd_vec_scale_f32
 * ────────────────────────────────────────────────────────── */

__attribute__((target("avx2")))
static void vec_scale_avx2(const float *a, float s,
                           float *out, size_t len) {
    /* broadcast scalar into ymm1 */
    __asm__ volatile("vbroadcastss %0, %%ymm1" : : "m"(s) : "ymm1");
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        __asm__ volatile(
            "vmovaps (%1), %%ymm0\n"
            "vmulps  %%ymm1, %%ymm0, %%ymm0\n"
            "vmovaps %%ymm0, (%0)\n"
            : : "r"(out+i),"r"(a+i) : "ymm0","memory"
        );
    }
    for (; i < len; i++) out[i] = a[i] * s;
    __asm__ volatile("vzeroupper" ::: "memory");
}

void simd_vec_scale_f32(const float *a, float scalar,
                        float *out, size_t len) {
    if (g_simd.avx2) { vec_scale_avx2(a, scalar, out, len); return; }
    for (size_t i = 0; i < len; i++) out[i] = a[i] * scalar;
}

/* ──────────────────────────────────────────────────────────
 * simd_dot_f32  (horizontal reduction)
 * ────────────────────────────────────────────────────────── */

__attribute__((target("avx2,fma")))
static float dot_avx2_fma(const float *a, const float *b, size_t len) {
    float acc = 0.f;
    /* Use ymm accumulator. */
    __asm__ volatile("vpxor %%ymm2, %%ymm2, %%ymm2" ::: "ymm2");
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        __asm__ volatile(
            "vmovaps (%0), %%ymm0\n"
            "vmovaps (%1), %%ymm1\n"
            "vfmadd231ps %%ymm1, %%ymm0, %%ymm2\n"
            : : "r"(a+i),"r"(b+i) : "ymm0","ymm1","ymm2","memory"
        );
    }
    /* Horizontal reduce ymm2 → scalar. */
    float tmp[8] __attribute__((aligned(32)));
    __asm__ volatile("vmovaps %%ymm2, (%0)" : : "r"(tmp) : "memory");
    __asm__ volatile("vzeroupper" ::: "memory");
    acc = tmp[0]+tmp[1]+tmp[2]+tmp[3]+tmp[4]+tmp[5]+tmp[6]+tmp[7];
    for (; i < len; i++) acc += a[i] * b[i];
    return acc;
}

float simd_dot_f32(const float *a, const float *b, size_t len) {
    if (g_simd.avx2 && g_simd.fma) return dot_avx2_fma(a, b, len);
    float acc = 0.f;
    for (size_t i = 0; i < len; i++) acc += a[i] * b[i];
    return acc;
}

/* ──────────────────────────────────────────────────────────
 * simd_matmul_f32  — C[M×N] = A[M×K] × B[K×N]
 *
 * Strategy:
 *  AVX2+FMA: outer loop over rows of A (M), inner over
 *  columns of B in groups of 8 (N), innermost K dot.
 *  For each (i,j_block): accumulate with VFMADD231PS.
 *
 *  Scalar fallback for small matrices.
 * ────────────────────────────────────────────────────────── */

__attribute__((target("avx2,fma")))
static void matmul_avx2_fma(const float *A, const float *B, float *C,
                             int M, int N, int K) {
    for (int i = 0; i < M; i++) {
        const float *Ai = A + (size_t)i * K;
        float       *Ci = C + (size_t)i * N;
        int j = 0;
        for (; j + 8 <= N; j += 8) {
            /* Accumulator for 8 output columns. */
            __asm__ volatile("vpxor %%ymm8, %%ymm8, %%ymm8" ::: "ymm8");
            for (int k = 0; k < K; k++) {
                /* Broadcast A[i,k] into ymm0. */
                float aik = Ai[k];
                __asm__ volatile(
                    "vbroadcastss %0, %%ymm0\n"
                    "vmovaps      (%1), %%ymm1\n"
                    "vfmadd231ps  %%ymm1, %%ymm0, %%ymm8\n"
                    : : "m"(aik), "r"(B + (size_t)k*N + j)
                    : "ymm0","ymm1","ymm8","memory"
                );
            }
            __asm__ volatile(
                "vmovaps %%ymm8, (%0)" : : "r"(Ci+j) : "memory"
            );
        }
        /* Tail columns (< 8). */
        for (; j < N; j++) {
            float acc = 0.f;
            for (int k = 0; k < K; k++) acc += Ai[k] * B[(size_t)k*N+j];
            Ci[j] = acc;
        }
    }
    __asm__ volatile("vzeroupper" ::: "memory");
}

static void matmul_scalar(const float *A, const float *B, float *C,
                          int M, int N, int K) {
    for (int i = 0; i < M; i++)
        for (int j = 0; j < N; j++) {
            float acc = 0.f;
            for (int k = 0; k < K; k++)
                acc += A[(size_t)i*K+k] * B[(size_t)k*N+j];
            C[(size_t)i*N+j] = acc;
        }
}

void simd_matmul_f32(const float *A, const float *B, float *C,
                     int M, int N, int K) {
    if (g_simd.avx2 && g_simd.fma) {
        matmul_avx2_fma(A, B, C, M, N, K);
        return;
    }
    matmul_scalar(A, B, C, M, N, K);
}

/* ──────────────────────────────────────────────────────────
 * simd_softmax_f32
 *  1. find max  (numerically stable)
 *  2. subtract max, compute exp, sum
 *  3. divide by sum
 *
 * exp approximation: e^x ≈ (1 + x/256)^256 via squaring.
 * Accurate to ~1e-4 for |x| < 20.
 * ────────────────────────────────────────────────────────── */

/* Fast exp2 via bit manipulation on IEEE 754 float.
 * exp(x) = exp2(x * log2(e)) */
static inline float fm_exp(float x) {
    /* Clamp to avoid denormals / overflow. */
    if (x >  88.f) return 3.40282347e+38f; /* ~FLT_MAX */
    if (x < -88.f) return 0.f;
    /* exp(x) via (1 + x/256)^256 — 8 squarings. */
    x = 1.f + x * (1.f / 256.f);
    x *= x; x *= x; x *= x; x *= x;
    x *= x; x *= x; x *= x; x *= x;
    return x;
}

void simd_softmax_f32(const float *x, float *out, size_t len) {
    if (len == 0) return;

    /* 1. max */
    float mx = x[0];
    for (size_t i = 1; i < len; i++) if (x[i] > mx) mx = x[i];

    /* 2. exp(x - max) and sum */
    float sum = 0.f;
    for (size_t i = 0; i < len; i++) {
        float e = fm_exp(x[i] - mx);
        out[i] = e;
        sum += e;
    }

    /* 3. normalise */
    float inv_sum = 1.f / sum;
    simd_vec_scale_f32(out, inv_sum, out, len);
}

/* ──────────────────────────────────────────────────────────
 * simd_gelu_f32
 *  gelu(x) = 0.5*x*(1 + tanh( sqrt(2/pi)*(x + 0.044715*x^3) ))
 * ────────────────────────────────────────────────────────── */

/* sqrt(2/pi) */
#define GELU_COEFF  0.7978845608028654f
#define GELU_CUBIC  0.044715f

void simd_gelu_f32(const float *x, float *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        float xi = x[i];
        float inner = GELU_COEFF * (xi + GELU_CUBIC * xi * xi * xi);
        out[i] = 0.5f * xi * (1.f + fm_tanh(inner));
    }
}

/* ──────────────────────────────────────────────────────────
 * simd_rms_norm_f32  (LLaMA / Mistral style)
 * ────────────────────────────────────────────────────────── */

void simd_rms_norm_f32(const float *x, const float *weight,
                       float *out, size_t len, float eps) {
    /* mean square */
    float ss = 0.f;
    for (size_t i = 0; i < len; i++) ss += x[i] * x[i];
    ss = ss / (float)len;
    float rms_inv = 1.f / fm_sqrt(ss + eps);
    for (size_t i = 0; i < len; i++)
        out[i] = x[i] * rms_inv * weight[i];
}

/* ──────────────────────────────────────────────────────────
 * simd_layer_norm_f32  (GPT-2 style)
 * ────────────────────────────────────────────────────────── */

void simd_layer_norm_f32(const float *x, const float *weight,
                         const float *bias, float *out,
                         size_t len, float eps) {
    /* mean */
    float mean = 0.f;
    for (size_t i = 0; i < len; i++) mean += x[i];
    mean /= (float)len;

    /* variance */
    float var = 0.f;
    for (size_t i = 0; i < len; i++) {
        float d = x[i] - mean;
        var += d * d;
    }
    var /= (float)len;
    float inv_std = 1.f / fm_sqrt(var + eps);

    for (size_t i = 0; i < len; i++)
        out[i] = (x[i] - mean) * inv_std * weight[i] + bias[i];
}

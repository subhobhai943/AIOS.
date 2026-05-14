#ifndef SIMD_H
#define SIMD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ─────────────────────────────────────────────
 * Phase 6.4 — CPU SIMD feature detection + math
 * kernels used by the LLM inference engine.
 *
 * Rules:
 *  • No libc headers other than stdint/stddef/stdbool.
 *  • All float buffers must be 32-byte aligned
 *    (kmalloc_aligned(size, 32)) for AVX2 paths.
 *  • Calling convention: C only (no intrinsic headers).
 *    The implementation uses inline asm / compiler
 *    built-ins to stay freestanding.
 * ───────────────────────────────────────────── */

/* ── Feature flags ── */
typedef struct {
    bool sse2;      /* baseline — Pentium 4+ */
    bool sse4_1;
    bool avx;       /* 256-bit float          */
    bool avx2;      /* 256-bit int + FMA3      */
    bool fma;       /* fused multiply-add      */
    bool avx512f;   /* 512-bit — optional      */
} simd_features_t;

extern simd_features_t g_simd;   /* filled by simd_detect() */

/* Detect CPU features via CPUID.  Call once at boot before
 * any math kernel is used.  Results stored in g_simd. */
void simd_detect(void);

/* Print detected features to VGA + serial (uses klog). */
void simd_print_features(void);

/* ── Matrix multiply  C = A * B
 *  A : M×K row-major f32
 *  B : K×N row-major f32
 *  C : M×N row-major f32 (caller-zeroed or overwritten)
 *
 *  Dispatch: AVX2+FMA → AVX → SSE2 → scalar
 *  All pointers must be 32-byte aligned for AVX2 path.
 */
void simd_matmul_f32(const float *A, const float *B, float *C,
                     int M, int N, int K);

/* ── Element-wise vector add: out[i] = a[i] + b[i] */
void simd_vec_add_f32(const float *a, const float *b,
                      float *out, size_t len);

/* ── Element-wise vector multiply: out[i] = a[i] * b[i] */
void simd_vec_mul_f32(const float *a, const float *b,
                      float *out, size_t len);

/* ── Scale: out[i] = a[i] * scalar */
void simd_vec_scale_f32(const float *a, float scalar,
                        float *out, size_t len);

/* ── Dot product: returns sum(a[i]*b[i]) */
float simd_dot_f32(const float *a, const float *b, size_t len);

/* ── In-place softmax over a vector of length len.
 *  Uses max-subtraction for numerical stability.
 *  out and x may alias. */
void simd_softmax_f32(const float *x, float *out, size_t len);

/* ── GELU activation (tanh approximation — same as GPT-2):
 *  gelu(x) = 0.5 * x * (1 + tanh(sqrt(2/pi) * (x + 0.044715*x^3)))
 *  Fully scalar (transcendentals); SIMD wrapper added later. */
void simd_gelu_f32(const float *x, float *out, size_t len);

/* ── RMS Norm (LLaMA style):
 *  y[i] = x[i] / sqrt( mean(x^2) + eps ) * weight[i] */
void simd_rms_norm_f32(const float *x, const float *weight,
                       float *out, size_t len, float eps);

/* ── Layer Norm (GPT-2 style):
 *  y[i] = (x[i] - mean) / sqrt(var + eps) * weight[i] + bias[i] */
void simd_layer_norm_f32(const float *x, const float *weight,
                         const float *bias, float *out,
                         size_t len, float eps);

#endif /* SIMD_H */

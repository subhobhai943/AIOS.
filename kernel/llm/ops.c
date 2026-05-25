/*
 * kernel/llm/ops.c — Phase 7.2: LLM math operations
 *
 * Freestanding C (no libc).  All math dispatches to the
 * SIMD kernels in kernel/simd.h where possible.
 *
 * Compiler flags assumed:
 *   -ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel
 *
 * Only allowed headers: <stdint.h>, <stddef.h> (via tensor.h).
 */

#include "ops.h"
#include "tensor.h"
#include "../simd.h"   /* simd_matmul_f32, simd_vec_add_f32, etc. */

static void ops_memcpy_bytes(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

/* ── tiny scalar math helpers (no libm) ─────────────────── */

/* Fast inverse-square-root used for normalisation.
 * We use Newton-Raphson via the SSE rsqrtss approximation;
 * if we are on a truly minimal CPU fall back to a simple
 * 3-iteration Newton loop.  The SIMD path is always compiled
 * in because SSE2 is baseline for x86-64. */
static inline float _rsqrtf(float x)
{
    /* Use SSE2 rsqrtss for a fast one-step NR result. */
    float r;
    __asm__ volatile(
        "rsqrtss %1, %0\n\t"
        "mulss   %0, %0\n\t"   /* r^2           */
        /* one Newton-Raphson step: r1 = r0*(3-x*r0^2)/2 */
        : "=x"(r)
        : "xm"(x)
    );
    /* Compile-time constant path — do a proper NR in C
     * to avoid assembly macro mess.  The asm above gives
     * ~11-bit accuracy; one NR step makes it ~23-bit. */
    float y;
    /* GCC intrinsic-style: just do the NR in C */
    __asm__ volatile("rsqrtss %1, %0" : "=x"(y) : "xm"(x));
    y = y * (1.5f - 0.5f * x * y * y); /* NR step */
    return y;
}

/* Scalar expf — Cephes-derived minimax for [-87, 88]. */
float ops_expf_approx(float x)
{
    /* clamp to prevent NaN */
    if (x >  88.3762f) return 3.40282347e+38f;
    if (x < -87.3365f) return 0.0f;

    /* Range reduce: x = k*ln2 + r, |r| <= ln2/2 */
    const float ln2_inv = 1.4426950408f;
    const float ln2_hi  = 0.6931471806f;
    const float ln2_lo  = 1.9082149293e-10f;
    int32_t k = (int32_t)(x * ln2_inv + 0.5f);
    float r = x - (float)k * ln2_hi - (float)k * ln2_lo;

    /* Minimax polynomial: exp(r) ≈ 1 + r(1 + r/2(1 + r/3(…))) */
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (
                  0.16666667f + r * (0.04166667f + r *
                  (0.00833333f + r * 0.001388889f)))));

    /* Scale by 2^k using bit manipulation */
    int32_t e = k + 127;
    if (e <= 0) return 0.0f;
    if (e >= 255) return 3.40282347e+38f;
    uint32_t bits = (uint32_t)e << 23;
    float scale;
    union {
        uint32_t u;
        float f;
    } scale_bits = { .u = bits };
    scale = scale_bits.f;
    return p * scale;
}

float ops_sqrtf_approx(float x)
{
    if (x <= 0.0f) return 0.0f;
    return x * _rsqrtf(x);
}

/* Scalar cosf + sinf via Taylor series (for RoPE). */
static float _cosf(float x)
{
    /* Reduce to [0, 2π] */
    const float two_pi = 6.28318530718f;
    /* bring into [-π, π] */
    int32_t n = (int32_t)(x / two_pi);
    x -= (float)n * two_pi;
    if (x >  3.14159265359f) x -= two_pi;
    if (x < -3.14159265359f) x += two_pi;

    /* 8-term Taylor: cos(x) = 1 - x^2/2! + x^4/4! - ... */
    float x2 = x * x;
    return 1.0f + x2 * (-0.5f + x2 * (
           0.04166667f + x2 * (-0.00138889f + x2 *
           0.00002480f)));
}

static float _sinf(float x)
{
    const float two_pi = 6.28318530718f;
    int32_t n = (int32_t)(x / two_pi);
    x -= (float)n * two_pi;
    if (x >  3.14159265359f) x -= two_pi;
    if (x < -3.14159265359f) x += two_pi;

    /* 8-term Taylor: sin(x) = x - x^3/3! + x^5/5! - ... */
    float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (
           0.00833333f + x2 * (-0.00019841f + x2 *
           0.0000027557f))));
}

/* powf(base, exp) for positive base, integer exp used by RoPE */
static float _powf_pos(float base, float e)
{
    /* Use: a^b = exp(b * ln(a))  — we only need ln for RoPE base */
    /* ln via minimax on [0.5, 2] after range reduction */
    float x = base;
    /* range reduce: x = m * 2^e2, m in [1, 2) */
    uint32_t bits;
    union {
        uint32_t u;
        float f;
    } bitcast = { .f = x };
    bits = bitcast.u;
    int32_t e2 = (int32_t)((bits >> 23) & 0xFF) - 127;
    bits = (bits & 0x807FFFFF) | (0x7F << 23); /* set exponent to 127 */
    bitcast.u = bits;
    x = bitcast.f;
    /* x now in [1, 2); subtract 1 for the series */
    float r = x - 1.0f;
    /* ln(1+r) minimax */
    float ln_m = r * (1.0f + r * (-0.5f + r * (
                 0.33333333f + r * (-0.25f + r *
                 0.2f))));
    float ln_x = ln_m + (float)e2 * 0.6931471806f;
    return ops_expf_approx(e * ln_x);
}

/* ──────────────────────────────────────────────────────────
 * 1. Element-wise and reduction ops
 * ────────────────────────────────────────────────────────── */

void ops_add(const tensor_t *a, const tensor_t *b, tensor_t *out)
{
    /* Delegate to the SIMD vector-add kernel */
    simd_vec_add_f32(a->data, b->data, out->data, a->numel);
}

void ops_scale(const tensor_t *t, float scalar, tensor_t *out)
{
    simd_vec_scale_f32(t->data, scalar, out->data, t->numel);
}

void ops_mul(const tensor_t *a, const tensor_t *b, tensor_t *out)
{
    simd_vec_mul_f32(a->data, b->data, out->data, a->numel);
}

void ops_fill(tensor_t *t, float val)
{
    float *p   = t->data;
    size_t n   = t->numel;
    for (size_t i = 0; i < n; i++) p[i] = val;
}

void ops_copy(tensor_t *dst, const tensor_t *src)
{
    ops_memcpy_bytes(dst->data, src->data, src->numel * sizeof(float));
}

/* ──────────────────────────────────────────────────────────
 * 2. Matrix multiply
 * ────────────────────────────────────────────────────────── */

void ops_matmul(const tensor_t *A, const tensor_t *B, tensor_t *C)
{
    /*
     * A: [M, K]   B: [K, N]   C: [M, N]
     * We derive M, K, N from tensor shapes.
     */
    int M = A->dims[0];
    int K = A->dims[1];
    int N = B->dims[1];
    simd_matmul_f32(A->data, B->data, C->data, M, N, K);
}

void ops_matmul_add(const tensor_t *A, const tensor_t *B,
                    const tensor_t *bias, tensor_t *C)
{
    ops_matmul(A, B, C);
    /* Add bias row-by-row: each row of C gets bias added */
    int M   = A->dims[0];
    int N   = B->dims[1];
    float *c = C->data;
    const float *b = bias->data;
    for (int m = 0; m < M; m++) {
        simd_vec_add_f32(c + m * N, b, c + m * N, (size_t)N);
    }
}

/* ──────────────────────────────────────────────────────────
 * 3. Activation functions
 * ────────────────────────────────────────────────────────── */

void ops_softmax(tensor_t *t)
{
    /*
     * For 1-D: one row of length numel.
     * For 2-D [rows, V]: apply softmax to each row independently.
     */
    int rows, V;
    if (t->ndim == 1) {
        rows = 1;
        V    = (int)t->numel;
    } else {
        rows = t->dims[0];
        V    = t->dims[1];
    }

    float *p = t->data;
    for (int r = 0; r < rows; r++) {
        /* simd_softmax_f32 is in-place capable (x and out may alias) */
        simd_softmax_f32(p + r * V, p + r * V, (size_t)V);
    }
}

void ops_gelu(const tensor_t *t, tensor_t *out)
{
    simd_gelu_f32(t->data, out->data, t->numel);
}

/* ──────────────────────────────────────────────────────────
 * 4. Normalisation layers
 * ────────────────────────────────────────────────────────── */

void ops_layer_norm(const tensor_t *x,
                    const tensor_t *weight,
                    const tensor_t *bias,
                    tensor_t       *out,
                    float           eps)
{
    simd_layer_norm_f32(x->data, weight->data, bias->data,
                        out->data, x->numel, eps);
}

void ops_rms_norm(const tensor_t *x,
                  const tensor_t *weight,
                  tensor_t       *out,
                  float           eps)
{
    simd_rms_norm_f32(x->data, weight->data, out->data,
                      x->numel, eps);
}

/* ──────────────────────────────────────────────────────────
 * 5. Embedding lookup
 * ────────────────────────────────────────────────────────── */

void ops_embedding_lookup(const tensor_t *table,
                          const int32_t  *ids,
                          int32_t         seq_len,
                          tensor_t       *out)
{
    /*
     * table : [vocab_size, embed_dim]
     * out   : [seq_len,    embed_dim]
     */
    int32_t embed_dim = table->dims[1];
    const float *w    = table->data;
    float *dst        = out->data;

    for (int32_t i = 0; i < seq_len; i++) {
        int32_t id = ids[i];
        /* Each row is embed_dim floats */
        const float *src = w + (size_t)id * (size_t)embed_dim;
        float       *d   = dst + (size_t)i * (size_t)embed_dim;
        ops_memcpy_bytes(d, src, (size_t)embed_dim * sizeof(float));
    }
}

/* ──────────────────────────────────────────────────────────
 * 6. Rotary Position Embedding (RoPE)
 * ────────────────────────────────────────────────────────── */

void ops_rope(tensor_t *q, tensor_t *k,
              int32_t pos, float base)
{
    /*
     * q, k : [seq_len, n_heads, head_dim]
     *
     * For each (seq position, head) we rotate consecutive pairs
     * of the head_dim dimension:
     *
     *   x0'  =  x0 * cos(theta) - x1 * sin(theta)
     *   x1'  =  x0 * sin(theta) + x1 * cos(theta)
     *
     * where theta_i = (pos + s) / (base ^ (2*i / head_dim))
     * and s is the sequence index within the tensor.
     */
    int32_t seq_len  = q->dims[0];
    int32_t n_heads  = q->dims[1];
    int32_t head_dim = q->dims[2];
    int32_t half     = head_dim >> 1;  /* head_dim / 2 */

    for (int32_t s = 0; s < seq_len; s++) {
        int32_t cur_pos = pos + s;
        for (int32_t h = 0; h < n_heads; h++) {
            /* Pointer to start of this (s, h) slice */
            size_t base_idx = ((size_t)s * n_heads + h) * head_dim;
            float *qh = q->data + base_idx;
            float *kh = k->data + base_idx;

            for (int32_t i = 0; i < half; i++) {
                /*
                 * theta = pos / (base ^ (2*i / head_dim))
                 * = pos * base^(-2i/head_dim)
                 */
                float inv_freq = _powf_pos(
                    base, -(2.0f * (float)i / (float)head_dim));
                float theta = (float)cur_pos * inv_freq;
                float cos_t = _cosf(theta);
                float sin_t = _sinf(theta);

                /* Q rotation */
                float q0 = qh[i];
                float q1 = qh[i + half];
                qh[i]        = q0 * cos_t - q1 * sin_t;
                qh[i + half] = q0 * sin_t + q1 * cos_t;

                /* K rotation */
                float k0 = kh[i];
                float k1 = kh[i + half];
                kh[i]        = k0 * cos_t - k1 * sin_t;
                kh[i + half] = k0 * sin_t + k1 * cos_t;
            }
        }
    }
}

#include "llm/ops.h"

#include <stdbool.h>

#include "heap.h"
#include "simd.h"

#define OPS_ALIGN_AVX 32u
#define OPS_ALIGN_SSE 16u
#define OPS_PI        3.14159265358979323846f
#define OPS_TWO_PI    6.28318530717958647692f
#define OPS_LN_10000  9.21034037197618273607f

static bool tensor_valid(const tensor_t *t)
{
    return t && t->data && t->ndim > 0 && t->ndim <= 4 && t->numel > 0;
}

static bool same_shape(const tensor_t *a, const tensor_t *b)
{
    if (!tensor_valid(a) || !tensor_valid(b)) return false;
    if (a->ndim != b->ndim || a->numel != b->numel) return false;
    for (int32_t i = 0; i < a->ndim; i++) {
        if (a->dims[i] != b->dims[i]) return false;
    }
    return true;
}

static bool ptr_aligned(const void *p, uintptr_t align)
{
    return (((uintptr_t)p) & (align - 1u)) == 0u;
}

static bool can_use_simd_vec(const float *a, const float *b, const float *out)
{
    if (g_simd.avx2) {
        return ptr_aligned(a, OPS_ALIGN_AVX) &&
               ptr_aligned(b, OPS_ALIGN_AVX) &&
               ptr_aligned(out, OPS_ALIGN_AVX);
    }
    if (g_simd.sse2) {
        return ptr_aligned(a, OPS_ALIGN_SSE) &&
               ptr_aligned(b, OPS_ALIGN_SSE) &&
               ptr_aligned(out, OPS_ALIGN_SSE);
    }
    return true;
}

static bool can_use_simd_scale(const float *a, const float *out)
{
    if (g_simd.avx2) {
        return ptr_aligned(a, OPS_ALIGN_AVX) &&
               ptr_aligned(out, OPS_ALIGN_AVX);
    }
    return true;
}

static bool can_use_simd_matmul(const float *a,
                                const float *b,
                                const float *out,
                                int32_t n)
{
    if (g_simd.avx2 && g_simd.fma) {
        return (n & 7) == 0 &&
               ptr_aligned(a, OPS_ALIGN_AVX) &&
               ptr_aligned(b, OPS_ALIGN_AVX) &&
               ptr_aligned(out, OPS_ALIGN_AVX);
    }
    return true;
}

static void scalar_matmul(const float *a,
                          const float *b,
                          float *out,
                          int32_t m,
                          int32_t n,
                          int32_t k)
{
    for (int32_t row = 0; row < m; row++) {
        for (int32_t col = 0; col < n; col++) {
            float acc = 0.0f;
            for (int32_t inner = 0; inner < k; inner++) {
                acc += a[(size_t)row * (size_t)k + (size_t)inner] *
                       b[(size_t)inner * (size_t)n + (size_t)col];
            }
            out[(size_t)row * (size_t)n + (size_t)col] = acc;
        }
    }
}

static void scalar_softmax(const float *x, float *out, size_t len);

static float ops_sqrt(float x)
{
    float r;
    __asm__ volatile("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

int ops_matmul(const tensor_t *a, const tensor_t *b, tensor_t *out)
{
    if (!tensor_valid(a) || !tensor_valid(b) || !tensor_valid(out)) {
        return OPS_ERR_INVALID;
    }
    if (a->ndim != 2 || b->ndim != 2 || out->ndim != 2) {
        return OPS_ERR_SHAPE;
    }

    int32_t m = a->dims[0];
    int32_t k = a->dims[1];
    int32_t bk = b->dims[0];
    int32_t n = b->dims[1];
    if (k != bk || out->dims[0] != m || out->dims[1] != n) {
        return OPS_ERR_SHAPE;
    }

    if (can_use_simd_matmul(a->data, b->data, out->data, n)) {
        simd_matmul_f32(a->data, b->data, out->data, m, n, k);
    } else {
        scalar_matmul(a->data, b->data, out->data, m, n, k);
    }
    return OPS_OK;
}

int ops_add(const tensor_t *a, const tensor_t *b, tensor_t *out)
{
    if (!tensor_valid(a) || !tensor_valid(b) || !tensor_valid(out)) {
        return OPS_ERR_INVALID;
    }
    if (!same_shape(a, out)) return OPS_ERR_SHAPE;

    if (same_shape(a, b)) {
        if (can_use_simd_vec(a->data, b->data, out->data)) {
            simd_vec_add_f32(a->data, b->data, out->data, a->numel);
        } else {
            for (size_t i = 0; i < a->numel; i++) {
                out->data[i] = a->data[i] + b->data[i];
            }
        }
        return OPS_OK;
    }

    if (b->numel == 1) {
        float scalar = b->data[0];
        for (size_t i = 0; i < a->numel; i++) {
            out->data[i] = a->data[i] + scalar;
        }
        return OPS_OK;
    }

    int32_t last_dim = a->dims[a->ndim - 1];
    if (b->ndim == 1 && b->dims[0] == last_dim) {
        size_t rows = a->numel / (size_t)last_dim;
        for (size_t row = 0; row < rows; row++) {
            size_t base = row * (size_t)last_dim;
            for (int32_t col = 0; col < last_dim; col++) {
                out->data[base + (size_t)col] =
                    a->data[base + (size_t)col] + b->data[col];
            }
        }
        return OPS_OK;
    }

    return OPS_ERR_SHAPE;
}

int ops_scale(const tensor_t *a, float scale, tensor_t *out)
{
    if (!tensor_valid(a) || !tensor_valid(out)) return OPS_ERR_INVALID;
    if (!same_shape(a, out)) return OPS_ERR_SHAPE;

    if (can_use_simd_scale(a->data, out->data)) {
        simd_vec_scale_f32(a->data, scale, out->data, a->numel);
    } else {
        for (size_t i = 0; i < a->numel; i++) out->data[i] = a->data[i] * scale;
    }
    return OPS_OK;
}

int ops_softmax(const tensor_t *x, tensor_t *out)
{
    if (!tensor_valid(x) || !tensor_valid(out)) return OPS_ERR_INVALID;
    if (!same_shape(x, out)) return OPS_ERR_SHAPE;

    int32_t last_dim = x->dims[x->ndim - 1];
    if (last_dim <= 0) return OPS_ERR_SHAPE;

    size_t rows = x->numel / (size_t)last_dim;
    for (size_t row = 0; row < rows; row++) {
        const float *src = x->data + row * (size_t)last_dim;
        float *dst = out->data + row * (size_t)last_dim;
        if (can_use_simd_vec(src, dst, dst)) {
            simd_softmax_f32(src, dst, (size_t)last_dim);
        } else {
            scalar_softmax(src, dst, (size_t)last_dim);
        }
    }
    return OPS_OK;
}

int ops_layer_norm(const tensor_t *x,
                   const tensor_t *weight,
                   const tensor_t *bias,
                   tensor_t *out,
                   float eps)
{
    if (!tensor_valid(x) || !tensor_valid(weight) || !tensor_valid(out)) {
        return OPS_ERR_INVALID;
    }
    if (!same_shape(x, out)) return OPS_ERR_SHAPE;

    int32_t last_dim = x->dims[x->ndim - 1];
    if (weight->ndim != 1 || weight->dims[0] != last_dim) return OPS_ERR_SHAPE;
    if (bias && (!tensor_valid(bias) || bias->ndim != 1 ||
                 bias->dims[0] != last_dim)) {
        return OPS_ERR_SHAPE;
    }

    size_t rows = x->numel / (size_t)last_dim;
    for (size_t row = 0; row < rows; row++) {
        const float *src = x->data + row * (size_t)last_dim;
        float *dst = out->data + row * (size_t)last_dim;
        if (bias) {
            simd_layer_norm_f32(src, weight->data, bias->data, dst,
                                (size_t)last_dim, eps);
        } else {
            float mean = 0.0f;
            for (int32_t i = 0; i < last_dim; i++) mean += src[i];
            mean /= (float)last_dim;

            float var = 0.0f;
            for (int32_t i = 0; i < last_dim; i++) {
                float d = src[i] - mean;
                var += d * d;
            }
            var /= (float)last_dim;

            float inv_std = 1.0f / ops_sqrt(var + eps);
            for (int32_t i = 0; i < last_dim; i++) {
                dst[i] = (src[i] - mean) * inv_std * weight->data[i];
            }
        }
    }
    return OPS_OK;
}

int ops_gelu(const tensor_t *x, tensor_t *out)
{
    if (!tensor_valid(x) || !tensor_valid(out)) return OPS_ERR_INVALID;
    if (!same_shape(x, out)) return OPS_ERR_SHAPE;

    simd_gelu_f32(x->data, out->data, x->numel);
    return OPS_OK;
}

int ops_embedding_lookup(const tensor_t *weight,
                         const int32_t *token_ids,
                         size_t token_count,
                         tensor_t *out)
{
    if (!tensor_valid(weight) || !token_ids || !tensor_valid(out)) {
        return OPS_ERR_INVALID;
    }
    if (weight->ndim != 2 || out->ndim != 2) return OPS_ERR_SHAPE;
    if (out->dims[0] != (int32_t)token_count ||
        out->dims[1] != weight->dims[1]) {
        return OPS_ERR_SHAPE;
    }

    int32_t vocab_size = weight->dims[0];
    int32_t embed_dim = weight->dims[1];
    size_t row_bytes = (size_t)embed_dim * sizeof(float);

    for (size_t i = 0; i < token_count; i++) {
        int32_t token = token_ids[i];
        if (token < 0 || token >= vocab_size) return OPS_ERR_RANGE;
        kmemcpy(out->data + i * (size_t)embed_dim,
                weight->data + (size_t)token * (size_t)embed_dim,
                row_bytes);
    }
    return OPS_OK;
}

static float ops_exp(float x)
{
    if (x > 88.0f) return 3.40282347e+38f;
    if (x < -88.0f) return 0.0f;

    x = 1.0f + x * (1.0f / 256.0f);
    x *= x; x *= x; x *= x; x *= x;
    x *= x; x *= x; x *= x; x *= x;
    return x;
}

static void scalar_softmax(const float *x, float *out, size_t len)
{
    if (len == 0) return;

    float max = x[0];
    for (size_t i = 1; i < len; i++) {
        if (x[i] > max) max = x[i];
    }

    float sum = 0.0f;
    for (size_t i = 0; i < len; i++) {
        float e = ops_exp(x[i] - max);
        out[i] = e;
        sum += e;
    }

    float inv_sum = 1.0f / sum;
    for (size_t i = 0; i < len; i++) out[i] *= inv_sum;
}

static float wrap_pi(float x)
{
    int32_t turns = (int32_t)(x / OPS_TWO_PI);
    x -= (float)turns * OPS_TWO_PI;
    if (x > OPS_PI) x -= OPS_TWO_PI;
    if (x < -OPS_PI) x += OPS_TWO_PI;
    return x;
}

static float ops_sin(float x)
{
    x = wrap_pi(x);
    float x2 = x * x;
    return x * (1.0f + x2 * (-0.1666666667f +
           x2 * (0.0083333333f + x2 * -0.0001984127f)));
}

static float ops_cos(float x)
{
    x = wrap_pi(x);
    float x2 = x * x;
    return 1.0f + x2 * (-0.5f +
           x2 * (0.0416666667f + x2 * -0.0013888889f));
}

static void apply_rope_to_tensor(tensor_t *t, uint32_t pos, int32_t last_dim)
{
    size_t rows = t->numel / (size_t)last_dim;
    for (size_t row = 0; row < rows; row++) {
        float *base = t->data + row * (size_t)last_dim;
        for (int32_t i = 0; i < last_dim; i += 2) {
            float inv_freq = ops_exp(-((float)i / (float)last_dim) *
                                     OPS_LN_10000);
            float angle = (float)pos * inv_freq;
            float c = ops_cos(angle);
            float s = ops_sin(angle);
            float even = base[i];
            float odd = base[i + 1];
            base[i] = even * c - odd * s;
            base[i + 1] = even * s + odd * c;
        }
    }
}

int ops_rope(tensor_t *q, tensor_t *k, uint32_t pos)
{
    if (!tensor_valid(q) || !tensor_valid(k)) return OPS_ERR_INVALID;
    if (!same_shape(q, k)) return OPS_ERR_SHAPE;

    int32_t last_dim = q->dims[q->ndim - 1];
    if (last_dim <= 0 || (last_dim & 1) != 0) return OPS_ERR_SHAPE;

    apply_rope_to_tensor(q, pos, last_dim);
    apply_rope_to_tensor(k, pos, last_dim);
    return OPS_OK;
}

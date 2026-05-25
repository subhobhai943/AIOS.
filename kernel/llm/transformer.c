/* kernel/llm/transformer.c — Phase 7.4
 *
 * GPT-2 post-norm and LLaMA pre-norm/RMSNorm transformer decoder block.
 *
 * Constraints:
 *  - Freestanding: -ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel
 *  - Only <stdint.h>, <stddef.h>, <stdbool.h>
 *  - All heap: kmalloc_aligned / kfree_aligned  (heap.h)
 *  - Debug log: klog / klog_dec  (serial.h)
 *  - No libm: uses freestanding helpers from ops.c
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "transformer.h"
#include "attention.h"
#include "ops.h"
#include "../heap.h"
#include "../serial.h"

/* -------------------------------------------------------------------------
 * Internal alignment (must match SIMD requirements in simd.c / ops.c).
 * -----------------------------------------------------------------------*/
#define BLK_ALIGN 32u

/* LayerNorm epsilon for GPT-2 blocks */
#define LAYERNORM_EPS 1e-5f

/* -------------------------------------------------------------------------
 * alloc_scratch / free_scratch — thin wrappers for legibility.
 * -----------------------------------------------------------------------*/
static inline float *alloc_scratch(size_t n) {
    if (!n) return NULL;
    float *p = (float *)kmalloc_aligned(n * sizeof(float), BLK_ALIGN);
    if (p) {
        for (size_t i = 0; i < n; ++i) p[i] = 0.0f;
    }
    return p;
}
static inline void free_scratch(float *p) {
    if (p) kfree_aligned(p);
}

/* =========================================================================
 * layer_norm_forward  —  standard LayerNorm (GPT-2)
 *
 *   y[i] = gamma[i] * (x[i] - mean) / sqrt(var + eps) + beta[i]
 *
 * Uses ops_layer_norm from ops.c.
 * =========================================================================*/
static void layer_norm_forward(
    const float *x,
    const float *gamma,
    const float *beta,
    float       *out,
    int32_t      n
) {
    tensor_t tx, tgamma, tbeta, tout;

    tx.data     = (float *)x;     tx.ndim     = 1; tx.dims[0]     = n; tx.numel     = (size_t)n;
    tgamma.data = (float *)gamma; tgamma.ndim = 1; tgamma.dims[0] = n; tgamma.numel = (size_t)n;
    tbeta.data  = (float *)beta;  tbeta.ndim  = 1; tbeta.dims[0]  = n; tbeta.numel  = (size_t)n;
    tout.data   = out;            tout.ndim   = 1; tout.dims[0]   = n; tout.numel   = (size_t)n;

    ops_layer_norm(&tx, &tgamma, &tbeta, &tout, LAYERNORM_EPS);
}

/* =========================================================================
 * rms_norm_forward  —  RMSNorm (LLaMA)
 *
 *   rms = sqrt( mean(x^2) + eps )
 *   y[i] = gamma[i] * x[i] / rms
 *
 * No beta term; LLaMA omits the centering step.
 * =========================================================================*/
#define RMSNORM_EPS 1e-6f

static void rms_norm_forward(
    const float *x,
    const float *gamma,
    float       *out,
    int32_t      n
) {
    float ss = 0.0f;
    for (int32_t i = 0; i < n; ++i)
        ss += x[i] * x[i];
    ss = ss / (float)n + RMSNORM_EPS;
    float inv_rms = 1.0f / ops_sqrtf_approx(ss);

    for (int32_t i = 0; i < n; ++i)
        out[i] = gamma[i] * (x[i] * inv_rms);
}

/* =========================================================================
 * matvec_add  —  out[M] = W[M×N] * x[N] + bias[M]  (bias nullable)
 * Delegates to ops_matmul using tensor views.
 * =========================================================================*/
static void matvec_add(
    const float *W,
    const float *x,
    const float *bias,
    float       *out,
    int32_t      M,
    int32_t      N
) {
    tensor_t tW, tx, tout;

    /* W: [M, N] */
    tW.data    = (float *)W;
    tW.ndim    = 2;
    tW.dims[0] = M;
    tW.dims[1] = N;
    tW.dims[2] = tW.dims[3] = 0;
    tW.numel   = (size_t)M * (size_t)N;

    /* x: [N, 1] */
    tx.data    = (float *)x;
    tx.ndim    = 2;
    tx.dims[0] = N;
    tx.dims[1] = 1;
    tx.dims[2] = tx.dims[3] = 0;
    tx.numel   = (size_t)N;

    /* out: [M, 1] */
    tout.data    = out;
    tout.ndim    = 2;
    tout.dims[0] = M;
    tout.dims[1] = 1;
    tout.dims[2] = tout.dims[3] = 0;
    tout.numel   = (size_t)M;

    ops_matmul(&tW, &tx, &tout);

    if (bias) {
        for (int32_t i = 0; i < M; ++i)
            out[i] += bias[i];
    }
}

/* =========================================================================
 * gelu_inplace  —  apply GELU activation element-wise (GPT-2 MLP)
 * Delegates to ops_gelu.
 * =========================================================================*/
static void gelu_inplace(float *x, int32_t n) {
    tensor_t t;
    t.data    = x;
    t.ndim    = 1;
    t.dims[0] = n;
    t.dims[1] = t.dims[2] = t.dims[3] = 0;
    t.numel   = (size_t)n;
    ops_gelu(&t, &t);
}

/* =========================================================================
 * swiglu_inplace  —  SwiGLU activation for LLaMA MLP
 * =========================================================================*/
static void swiglu_inplace(float *gate_buf, float *up_buf, int32_t n) {
    for (int32_t i = 0; i < n; ++i) {
        float g = gate_buf[i];
        float sig = 1.0f / (1.0f + ops_expf_approx(-g));
        up_buf[i] = up_buf[i] * (g * sig);
    }
}

/* =========================================================================
 * vec_add  —  out[i] += src[i]
 * =========================================================================*/
static void vec_add(float *out, const float *src, int32_t n) {
    for (int32_t i = 0; i < n; ++i)
        out[i] += src[i];
}

/* =========================================================================
 * vec_copy  —  dst[i] = src[i]
 * =========================================================================*/
static void vec_copy(float *dst, const float *src, int32_t n) {
    for (int32_t i = 0; i < n; ++i)
        dst[i] = src[i];
}

/* =========================================================================
 * transformer_block_forward
 * =========================================================================*/
int transformer_block_forward(
    const transformer_block_t *block,
    const attn_config_t       *cfg,
    const float               *x,
    float                     *out,
    kv_cache_t                *kvc,
    int32_t                    layer,
    int32_t                    pos
) {
    if (!block || !cfg || !x || !out || !kvc) return -1;
    if (!block->norm1_gamma || !block->norm2_gamma) return -1;
    if (!block->wq || !block->wk || !block->wv || !block->wo) return -1;
    if (!block->w1 || !block->w2) return -1;
    if (block->style == TRANSFORMER_STYLE_LLAMA && !block->w_gate) return -1;

    const int32_t n     = cfg->n_embd;
    const int32_t ffn_h = block->ffn_hidden;

    float *normed  = alloc_scratch((size_t)n);
    float *attn_o  = alloc_scratch((size_t)n);
    float *h       = alloc_scratch((size_t)n);
    float *normed2 = alloc_scratch((size_t)n);
    float *mlp_h   = alloc_scratch((size_t)ffn_h);
    float *gate    = NULL;

    if (block->style == TRANSFORMER_STYLE_LLAMA)
        gate = alloc_scratch((size_t)ffn_h);

    if (!normed || !attn_o || !h || !normed2 || !mlp_h ||
        (block->style == TRANSFORMER_STYLE_LLAMA && !gate)) {
        klog("[transformer] OOM allocating scratch buffers\n");
        free_scratch(normed); free_scratch(attn_o); free_scratch(h);
        free_scratch(normed2); free_scratch(mlp_h); free_scratch(gate);
        return -1;
    }

    if (block->style == TRANSFORMER_STYLE_GPT2) {
        layer_norm_forward(x, block->norm1_gamma, block->norm1_beta,
                           normed, n);

        int ret = attn_forward(cfg, normed, attn_o,
                               block->wq, block->wk, block->wv, block->wo,
                               block->bq, block->bk, block->bv, block->bo,
                               kvc, layer, pos);
        if (ret != 0) goto cleanup_fail;

        vec_copy(h, x, n);
        vec_add(h, attn_o, n);

        layer_norm_forward(h, block->norm2_gamma, block->norm2_beta,
                           normed2, n);

        matvec_add(block->w1, normed2, block->b1, mlp_h, ffn_h, n);
        gelu_inplace(mlp_h, ffn_h);
        matvec_add(block->w2, mlp_h, block->b2, out, n, ffn_h);

        vec_add(out, h, n);
    }
    else if (block->style == TRANSFORMER_STYLE_LLAMA) {
        rms_norm_forward(x, block->norm1_gamma, normed, n);

        int ret = attn_forward(cfg, normed, attn_o,
                               block->wq, block->wk, block->wv, block->wo,
                               NULL, NULL, NULL, NULL,
                               kvc, layer, pos);
        if (ret != 0) goto cleanup_fail;

        vec_copy(h, x, n);
        vec_add(h, attn_o, n);

        rms_norm_forward(h, block->norm2_gamma, normed2, n);

        matvec_add(block->w_gate, normed2, NULL, gate,  ffn_h, n);
        matvec_add(block->w1,     normed2, NULL, mlp_h, ffn_h, n);
        swiglu_inplace(gate, mlp_h, ffn_h);
        matvec_add(block->w2, mlp_h, NULL, out, n, ffn_h);

        vec_add(out, h, n);
    }
    else {
        klog("[transformer] unknown style\n");
        goto cleanup_fail;
    }

    free_scratch(normed);
    free_scratch(attn_o);
    free_scratch(h);
    free_scratch(normed2);
    free_scratch(mlp_h);
    free_scratch(gate);
    return 0;

cleanup_fail:
    free_scratch(normed);
    free_scratch(attn_o);
    free_scratch(h);
    free_scratch(normed2);
    free_scratch(mlp_h);
    free_scratch(gate);
    return -1;
}

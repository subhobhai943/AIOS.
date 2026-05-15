/* kernel/llm/transformer.c — Phase 7.4
 *
 * GPT-2 post-norm and LLaMA pre-norm/RMSNorm transformer decoder block.
 *
 * Constraints:
 *  - Freestanding: -ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel
 *  - Only <stdint.h>, <stddef.h>, <stdbool.h>
 *  - All heap: kmalloc_aligned / kfree_aligned  (heap.h)
 *  - Debug log: klog / klog_dec  (serial.h)
 *  - No libm: uses __builtin_sqrtf, __builtin_expf, __builtin_fabsf
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
 * Uses ops_layer_norm from ops.c (which already does this internally).
 * We wrap it here to keep the transformer code readable.
 * =========================================================================*/
static void layer_norm_forward(
    const float *x,
    const float *gamma,
    const float *beta,
    float       *out,
    int32_t      n
) {
    /* Pack into tensor_t views for ops_layer_norm */
    tensor_t tx, tgamma, tbeta, tout;
    int32_t dims1[1] = { n };

    tx.data    = (float *)x;     tx.ndim    = 1; tx.dims[0]    = n; tx.numel    = (size_t)n;
    tgamma.data= (float *)gamma; tgamma.ndim= 1; tgamma.dims[0]= n; tgamma.numel= (size_t)n;
    tbeta.data = (float *)beta;  tbeta.ndim = 1; tbeta.dims[0] = n; tbeta.numel = (size_t)n;
    tout.data  = out;            tout.ndim  = 1; tout.dims[0]  = n; tout.numel  = (size_t)n;
    (void)dims1;

    ops_layer_norm(&tx, &tgamma, &tbeta, &tout);
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
    /* Compute RMS */
    float ss = 0.0f;
    for (int32_t i = 0; i < n; ++i)
        ss += x[i] * x[i];
    ss = ss / (float)n + RMSNORM_EPS;
    float inv_rms = 1.0f / __builtin_sqrtf(ss);

    for (int32_t i = 0; i < n; ++i)
        out[i] = gamma[i] * (x[i] * inv_rms);
}

/* =========================================================================
 * matvec_add  —  out[M] = W[M×N] * x[N] + bias[M]  (bias nullable)
 * Delegates to ops_matmul (row-major, 1-column output).
 * =========================================================================*/
static void matvec_add(
    const float *W,
    const float *x,
    const float *bias,
    float       *out,
    int32_t M, int32_t N
) {
    ops_matmul(W, x, out, M, N, 1);
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
    t.data   = x;
    t.ndim   = 1;
    t.dims[0]= n;
    t.numel  = (size_t)n;
    ops_gelu(&t, &t);   /* in-place: src == dst is safe in ops_gelu */
}

/* =========================================================================
 * swiglu_inplace  —  SwiGLU activation for LLaMA MLP
 *
 * gate_buf holds the gate projection output [ffn_hidden].
 * up_buf   holds the up   projection output [ffn_hidden].
 * Result written back into up_buf:
 *   up_buf[i] = up_buf[i] * silu(gate_buf[i])
 *
 * SiLU(x) = x * sigmoid(x) = x / (1 + exp(-x))
 * =========================================================================*/
static void swiglu_inplace(float *gate_buf, float *up_buf, int32_t n) {
    for (int32_t i = 0; i < n; ++i) {
        float g = gate_buf[i];
        /* silu(g) = g * sigmoid(g) */
        float sig = 1.0f / (1.0f + __builtin_expf(-g));
        up_buf[i] = up_buf[i] * (g * sig);
    }
}

/* =========================================================================
 * vec_add  —  out[i] += src[i]  (residual addition)
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
    /* ---- Validate ---- */
    if (!block || !cfg || !x || !out || !kvc) return -1;
    if (!block->norm1_gamma || !block->norm2_gamma) return -1;
    if (!block->wq || !block->wk || !block->wv || !block->wo) return -1;
    if (!block->w1 || !block->w2) return -1;
    if (block->style == TRANSFORMER_STYLE_LLAMA && !block->w_gate) return -1;

    const int32_t n      = cfg->n_embd;
    const int32_t ffn_h  = block->ffn_hidden;

    /* ---- Allocate scratch buffers ---- */
    float *normed  = alloc_scratch((size_t)n);      /* normed input           */
    float *attn_o  = alloc_scratch((size_t)n);      /* attention output       */
    float *h       = alloc_scratch((size_t)n);      /* post-attn hidden state */
    float *normed2 = alloc_scratch((size_t)n);      /* normed for MLP         */
    float *mlp_h   = alloc_scratch((size_t)ffn_h);  /* MLP hidden (up/gate)   */
    float *gate    = NULL;  /* SwiGLU gate (LLaMA only) */

    if (block->style == TRANSFORMER_STYLE_LLAMA) {
        gate = alloc_scratch((size_t)ffn_h);
    }

    if (!normed || !attn_o || !h || !normed2 || !mlp_h ||
        (block->style == TRANSFORMER_STYLE_LLAMA && !gate)) {
        klog("[transformer] OOM allocating scratch buffers\n");
        free_scratch(normed); free_scratch(attn_o); free_scratch(h);
        free_scratch(normed2); free_scratch(mlp_h); free_scratch(gate);
        return -1;
    }

    /* ==================================================================
     * GPT-2 STYLE  (post-norm)
     *   residual1 = x  +  Attention( LayerNorm1(x) )
     *   out       = residual1  +  MLP( LayerNorm2(residual1) )
     * =================================================================*/
    if (block->style == TRANSFORMER_STYLE_GPT2) {

        /* 1. LayerNorm1(x) → normed */
        layer_norm_forward(x, block->norm1_gamma, block->norm1_beta,
                           normed, n);

        /* 2. Attention(normed) → attn_o */
        int ret = attn_forward(cfg, normed, attn_o,
                               block->wq, block->wk, block->wv, block->wo,
                               block->bq, block->bk, block->bv, block->bo,
                               kvc, layer, pos);
        if (ret != 0) goto cleanup_fail;

        /* 3. residual1: h = x + attn_o */
        vec_copy(h, x, n);
        vec_add(h, attn_o, n);

        /* 4. LayerNorm2(h) → normed2 */
        layer_norm_forward(h, block->norm2_gamma, block->norm2_beta,
                           normed2, n);

        /* 5. MLP: up = W1 * normed2 + b1  → GELU  → out_mlp = W2 * up + b2 */
        matvec_add(block->w1, normed2, block->b1, mlp_h, ffn_h, n);
        gelu_inplace(mlp_h, ffn_h);
        matvec_add(block->w2, mlp_h, block->b2, out, n, ffn_h);

        /* 6. residual2: out += h */
        vec_add(out, h, n);
    }

    /* ==================================================================
     * LLAMA STYLE  (pre-norm, RMSNorm, SwiGLU)
     *   residual1 = x  +  Attention( RMSNorm1(x) )
     *   out       = residual1  +  MLP_swiglu( RMSNorm2(residual1) )
     * =================================================================*/
    else if (block->style == TRANSFORMER_STYLE_LLAMA) {

        /* 1. RMSNorm(x) → normed  (no beta) */
        rms_norm_forward(x, block->norm1_gamma, normed, n);

        /* 2. Attention(normed) → attn_o */
        int ret = attn_forward(cfg, normed, attn_o,
                               block->wq, block->wk, block->wv, block->wo,
                               NULL, NULL, NULL, NULL,   /* LLaMA: no QKV biases */
                               kvc, layer, pos);
        if (ret != 0) goto cleanup_fail;

        /* 3. residual1: h = x + attn_o */
        vec_copy(h, x, n);
        vec_add(h, attn_o, n);

        /* 4. RMSNorm(h) → normed2 */
        rms_norm_forward(h, block->norm2_gamma, normed2, n);

        /* 5. SwiGLU MLP:
         *    gate[i] = w_gate * normed2
         *    up[i]   = w1    * normed2
         *    act[i]  = up[i] * silu(gate[i])
         *    out_mlp = w2 * act
         */
        matvec_add(block->w_gate, normed2, NULL, gate,  ffn_h, n);
        matvec_add(block->w1,     normed2, NULL, mlp_h, ffn_h, n);
        swiglu_inplace(gate, mlp_h, ffn_h);  /* mlp_h now holds act */
        matvec_add(block->w2, mlp_h, NULL, out, n, ffn_h);

        /* 6. residual2: out += h */
        vec_add(out, h, n);
    }
    else {
        klog("[transformer] unknown style\n");
        goto cleanup_fail;
    }

    /* ---- Release scratch ---- */
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

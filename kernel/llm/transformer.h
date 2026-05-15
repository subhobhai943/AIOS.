#ifndef TRANSFORMER_H
#define TRANSFORMER_H

/* kernel/llm/transformer.h — Phase 7.4
 *
 * One transformer decoder block (GPT-2 post-norm OR LLaMA pre-norm).
 * Freestanding C — no libc.  Only <stdint.h> / <stddef.h> / <stdbool.h>.
 * All heap traffic via kmalloc_aligned / kfree_aligned.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "attention.h"   /* attn_config_t, kv_cache_t */

/* ---------------------------------------------------------------------------
 * Style selector
 * -------------------------------------------------------------------------*/
#define TRANSFORMER_STYLE_GPT2   0   /* post-LayerNorm, GELU MLP            */
#define TRANSFORMER_STYLE_LLAMA  1   /* pre-RMSNorm,    SwiGLU MLP          */

/* ---------------------------------------------------------------------------
 * transformer_block_t
 *
 * Raw weight pointers for ONE decoder layer.  The model loader (Phase 7.6)
 * will point these at the appropriate slice of the weight file.
 * All pointers must be 32-byte aligned.
 *
 * Dimensions (assuming n_embd = d, n_heads = H, head_dim = d/H,
 *             n_kv_heads = Hkv, ffn_hidden = 4*d for GPT-2 or
 *             2/3 * 4*d rounded to multiple of 256 for LLaMA):
 *
 *  norm1_gamma / norm1_beta  : [d]           (LayerNorm or RMSNorm weight)
 *  norm2_gamma / norm2_beta  : [d]           (NULL for RMSNorm beta)
 *  wq                        : [d  × d]      Q projection
 *  wk                        : [d  × (Hkv*head_dim)]
 *  wv                        : [d  × (Hkv*head_dim)]
 *  wo                        : [d  × d]      output projection
 *  bq/bk/bv/bo               : [d] or NULL  (GPT-2 has biases; LLaMA does not)
 *  w1                        : [d  × ffn_hidden]  up/gate projection
 *  w2                        : [ffn_hidden × d]   down projection
 *  w_gate                    : [d  × ffn_hidden]  SwiGLU gate (LLaMA only; NULL for GPT-2)
 *  b1 / b2                   : [ffn_hidden] / [d] or NULL
 * -------------------------------------------------------------------------*/
typedef struct {
    /* Normalization weights */
    const float *norm1_gamma;  /* LayerNorm/RMSNorm scale    [n_embd] */
    const float *norm1_beta;   /* LayerNorm bias (NULL for RMSNorm)  [n_embd] */
    const float *norm2_gamma;  /* second norm scale          [n_embd] */
    const float *norm2_beta;   /* second norm bias (NULL for RMSNorm) [n_embd] */

    /* Attention projections */
    const float *wq;           /* [n_embd × n_embd] */
    const float *wk;           /* [n_embd × (n_kv_heads * head_dim)] */
    const float *wv;           /* [n_embd × (n_kv_heads * head_dim)] */
    const float *wo;           /* [n_embd × n_embd] */
    const float *bq;           /* nullable */
    const float *bk;           /* nullable */
    const float *bv;           /* nullable */
    const float *bo;           /* nullable */

    /* MLP / FFN weights */
    const float *w1;           /* up projection   [n_embd × ffn_hidden] */
    const float *w2;           /* down projection [ffn_hidden × n_embd] */
    const float *w_gate;       /* SwiGLU gate     [n_embd × ffn_hidden] (LLaMA only) */
    const float *b1;           /* nullable */
    const float *b2;           /* nullable */

    int32_t ffn_hidden;        /* FFN hidden dimension (4*n_embd for GPT-2) */
    int32_t style;             /* TRANSFORMER_STYLE_GPT2 or TRANSFORMER_STYLE_LLAMA */
} transformer_block_t;

/* ---------------------------------------------------------------------------
 * transformer_block_forward()
 *
 * Execute one complete decoder block for a single token at position `pos`.
 *
 * Parameters:
 *   block  — weight pointers for this layer
 *   cfg    — attention config (n_heads, n_kv_heads, n_embd, max_seq_len, n_layers)
 *   x      — input  [n_embd]  (hidden state of current token)
 *   out    — output [n_embd]  (caller-allocated, same size)
 *   kvc    — KV-cache (updated in-place)
 *   layer  — which transformer layer index (0-based)
 *   pos    — sequence position of this token (0-based)
 *
 * Both GPT-2 and LLaMA paths write their result into `out`.
 * x is NOT modified; out may alias x only if the caller accepts an in-place
 * update (safe because residual additions copy before overwriting).
 *
 * Returns 0 on success, -1 on OOM or invalid arguments.
 * -------------------------------------------------------------------------*/
int transformer_block_forward(
    const transformer_block_t *block,
    const attn_config_t       *cfg,
    const float               *x,
    float                     *out,
    kv_cache_t                *kvc,
    int32_t                    layer,
    int32_t                    pos
);

#endif /* TRANSFORMER_H */

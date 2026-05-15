#ifndef ATTENTION_H
#define ATTENTION_H

/* kernel/llm/attention.h — Phase 7.3
 *
 * Multi-Head Attention with KV-Cache.
 * Freestanding C — no libc.  Only <stdint.h> / <stddef.h> / <stdbool.h>.
 * All heap traffic goes through kmalloc/kfree.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "tensor.h"

/* ---------------------------------------------------------------------------
 * Configuration passed to attn_init().
 * These match the model-level hyper-parameters; fill from model_config_t
 * once Phase 7.5 is ready.
 * -------------------------------------------------------------------------*/
typedef struct {
    int32_t n_heads;     /* number of attention heads */
    int32_t n_kv_heads;  /* number of K/V heads (same as n_heads for GPT-2;
                            can be fewer for GQA/MQA variants like LLaMA-3) */
    int32_t n_embd;      /* model embedding dimension (= head_dim * n_heads) */
    int32_t max_seq_len; /* maximum sequence length (KV-cache depth) */
    int32_t n_layers;    /* total transformer layers (for KV-cache allocation) */
} attn_config_t;

/* ---------------------------------------------------------------------------
 * KV-Cache — one struct per model instance.
 *
 * Layout: k_cache[layer][head][seq][head_dim]
 *         v_cache[layer][head][seq][head_dim]
 *
 * Both are stored as flat float arrays for cache-friendly sequential access.
 * Offset macro:  KV_IDX(cfg, layer, head, pos, d)
 *   = layer*(n_kv_heads*max_seq_len*head_dim)
 *   + head *(max_seq_len*head_dim)
 *   + pos  *(head_dim)
 *   + d
 * -------------------------------------------------------------------------*/
typedef struct {
    float   *k;          /* [n_layers][n_kv_heads][max_seq_len][head_dim] */
    float   *v;          /* same shape */
    int32_t  head_dim;   /* n_embd / n_heads */
    int32_t  n_layers;
    int32_t  n_kv_heads;
    int32_t  max_seq_len;
    int32_t  cur_len;    /* tokens appended so far (shared across all layers) */
} kv_cache_t;

/* Allocate a KV-cache for the given config.  Returns NULL on OOM. */
kv_cache_t *kvcache_alloc(const attn_config_t *cfg);

/* Release a KV-cache previously returned by kvcache_alloc. */
void        kvcache_free(kv_cache_t *kvc);

/* Reset sequence position (clear cache without re-allocating memory). */
void        kvcache_reset(kv_cache_t *kvc);

/* ---------------------------------------------------------------------------
 * Attention weight matrices — the four projection matrices for one layer.
 * Weights are external (loaded by the model loader in Phase 7.6).
 * attn_forward() receives raw float pointers to avoid a heavyweight struct.
 * -------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * attn_forward()
 *
 * Execute one decoder-style (causal) Multi-Head Attention layer for a
 * single new token at position `pos`.
 *
 * Parameters:
 *   cfg       — model config (n_heads, n_kv_heads, n_embd, max_seq_len)
 *   x         — input  [n_embd]  (the current token hidden state)
 *   out       — output [n_embd]  (must be pre-allocated by caller)
 *   wq        — Q projection weight [n_embd × n_embd], row-major
 *   wk        — K projection weight [n_embd × (n_kv_heads * head_dim)]
 *   wv        — V projection weight [n_embd × (n_kv_heads * head_dim)]
 *   wo        — O projection weight [n_embd × n_embd]
 *   bq/bk/bv/bo — optional bias vectors (NULL = no bias)
 *   kvc       — KV-cache (updated in-place at `layer`/`pos`)
 *   layer     — which transformer layer index (0 .. n_layers-1)
 *   pos       — current sequence position (0-based)
 *
 * All float buffers must be 32-byte aligned for AVX2 inner loops.
 *
 * Returns 0 on success, -1 on invalid arguments.
 * -------------------------------------------------------------------------*/
int attn_forward(
    const attn_config_t *cfg,
    const float         *x,
    float               *out,
    const float         *wq,
    const float         *wk,
    const float         *wv,
    const float         *wo,
    const float         *bq,   /* nullable */
    const float         *bk,   /* nullable */
    const float         *bv,   /* nullable */
    const float         *bo,   /* nullable */
    kv_cache_t          *kvc,
    int32_t              layer,
    int32_t              pos
);

/* ---------------------------------------------------------------------------
 * attn_forward_full()
 *
 * Prefill variant: processes an entire input sequence of length `seq_len`
 * in one call (no incremental caching — fills cache for all positions).
 * Used during the prompt-ingestion phase before autoregressive generation.
 *
 *   x_seq  — [seq_len × n_embd] input matrix (row-major)
 *   out_seq — [seq_len × n_embd] output matrix (pre-allocated)
 *
 * Internally calls attn_forward() for each position, relying on the
 * incrementally built KV-cache for causal masking.
 * -------------------------------------------------------------------------*/
int attn_forward_full(
    const attn_config_t *cfg,
    const float         *x_seq,
    float               *out_seq,
    const float         *wq,
    const float         *wk,
    const float         *wv,
    const float         *wo,
    const float         *bq,
    const float         *bk,
    const float         *bv,
    const float         *bo,
    kv_cache_t          *kvc,
    int32_t              layer
);

#endif /* ATTENTION_H */

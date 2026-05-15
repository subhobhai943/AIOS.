#ifndef AIOS_LLM_MODEL_H
#define AIOS_LLM_MODEL_H

/* kernel/llm/model.h — Phase 7.5
 *
 * Full transformer model: embedding table, N stacked transformer
 * blocks, final LayerNorm / RMSNorm, and an LM-head projection
 * that produces logits over the vocabulary.
 *
 * Supports both GPT-2 style (post-norm, GELU MLP) and LLaMA style
 * (pre-norm, RMSNorm, SwiGLU MLP) via model_config_t.arch.
 *
 * Freestanding C — no libc.  Only <stdint.h>, <stddef.h>, <stdbool.h>.
 * All heap via kmalloc/kfree (heap.h).  Float via __builtin_* / compiler
 * intrinsics only — no libm.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "tensor.h"
#include "transformer.h"
#include "attention.h"

/* ─────────────────────────────────────────────────────────────
 * Architecture tag
 * ───────────────────────────────────────────────────────────── */
typedef enum {
    MODEL_ARCH_GPT2  = 0,   /* post-norm, GELU, no RoPE tied        */
    MODEL_ARCH_LLAMA = 1,   /* pre-norm,  RMSNorm, SwiGLU, RoPE     */
} model_arch_t;

/* ─────────────────────────────────────────────────────────────
 * Model configuration
 * ───────────────────────────────────────────────────────────── */
typedef struct {
    /* architecture */
    model_arch_t arch;          /* GPT-2 or LLaMA style               */

    /* dimensions */
    uint32_t vocab_size;        /* number of tokens in vocabulary     */
    uint32_t n_embd;            /* embedding / hidden dimension       */
    uint32_t n_layers;          /* number of stacked transformer blks */
    uint32_t n_heads;           /* query heads                        */
    uint32_t n_kv_heads;        /* KV heads (< n_heads = GQA)         */
    uint32_t ffn_hidden;        /* MLP hidden dim (often 4*n_embd)    */
    uint32_t max_seq_len;       /* max context length                 */

    /* normalisation */
    float    layer_norm_eps;    /* epsilon for LayerNorm / RMSNorm    */

    /* tied weights: if true, lm_head reuses wte (GPT-2 style)        */
    bool     tie_weights;
} model_config_t;

/* ─────────────────────────────────────────────────────────────
 * Sampling configuration
 * ───────────────────────────────────────────────────────────── */
typedef struct {
    float    temperature;   /* divide logits; 0 = greedy (argmax)    */
    uint32_t top_k;         /* 0 = disabled                          */
    float    top_p;         /* nucleus sampling; 1.0 = disabled      */
    uint64_t rng_seed;      /* seed for LFSR sampler                 */
} sample_config_t;

/* ─────────────────────────────────────────────────────────────
 * Model weight tensors
 *
 * All tensors are views into a single weight blob loaded by the
 * Phase 7.6 loader.  They are NOT individually kmalloc’d.
 * ───────────────────────────────────────────────────────────── */
typedef struct {
    /* Token + positional embeddings */
    tensor_t wte;           /* [vocab_size, n_embd]  token embed      */
    tensor_t wpe;           /* [max_seq, n_embd]     pos embed (GPT2) */

    /* Stacked transformer blocks — array of n_layers */
    transformer_block_t *blocks; /* kmalloc’d array, NOT weight blob  */

    /* Final normalisation before LM head */
    tensor_t ln_f_w;        /* [n_embd]  scale                        */
    tensor_t ln_f_b;        /* [n_embd]  bias (zero for RMSNorm)      */

    /* Language-model head */
    tensor_t lm_head;       /* [vocab_size, n_embd]  (or = wte data)  */

    /* KV cache: one shared cache object, layered internally */
    kv_cache_t *kvc;
} aios_model_t;

/* ─────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────── */

/*
 * model_alloc(cfg)
 *
 * Allocate an aios_model_t struct and its block array via kmalloc.
 * Does NOT load weights — that is Phase 7.6 (loader.c).
 * Returns NULL on OOM.
 */
aios_model_t *model_alloc(const model_config_t *cfg);

/*
 * model_free(m)
 *
 * Free the block array and the model struct itself.
 * Does NOT free weight blob data (owned by loader).
 */
void model_free(aios_model_t *m);

/*
 * model_forward(m, cfg, token_id, pos, logits_out)
 *
 * Run one auto-regressive forward step:
 *   1. Embed token_id + positional encoding.
 *   2. Pass through all N transformer blocks (using KV-cache).
 *   3. Apply final LayerNorm / RMSNorm.
 *   4. Project through LM-head to produce logits[vocab_size].
 *
 * logits_out  — caller-supplied float[cfg->vocab_size] buffer.
 * pos         — current sequence position (0-indexed).
 *
 * Returns 0 on success, -1 on error.
 */
int model_forward(aios_model_t       *m,
                  const model_config_t *cfg,
                  uint32_t             token_id,
                  uint32_t             pos,
                  float               *logits_out);

/*
 * model_sample(logits, vocab_size, scfg)
 *
 * Given raw logits[vocab_size], apply temperature + top-k + top-p
 * (nucleus) sampling and return the sampled token id.
 *
 * If scfg->temperature == 0.0f the function is pure argmax (greedy).
 */
uint32_t model_sample(const float          *logits,
                      uint32_t              vocab_size,
                      const sample_config_t *scfg);

/*
 * model_reset_kvcache(m)
 *
 * Wipe the KV-cache (set cur_len = 0) for a new conversation.
 * Cheap — no deallocation.
 */
void model_reset_kvcache(aios_model_t *m);

#endif /* AIOS_LLM_MODEL_H */

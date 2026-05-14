#ifndef LLM_OPS_H
#define LLM_OPS_H

#include <stddef.h>
#include <stdint.h>

#include "llm/tensor.h"

/* Phase 7.2 — freestanding tensor math operations.
 *
 * Functions return 0 on success and a negative ops_status_t value on error.
 * Operations write into caller-provided output tensors; no implicit allocation
 * happens here.
 */
typedef enum ops_status {
    OPS_OK          = 0,
    OPS_ERR_INVALID = -1,
    OPS_ERR_SHAPE   = -2,
    OPS_ERR_RANGE   = -3,
} ops_status_t;

/* Matrix multiply:
 *   a:   [M, K]
 *   b:   [K, N]
 *   out: [M, N]
 */
int ops_matmul(const tensor_t *a, const tensor_t *b, tensor_t *out);

/* Elementwise add. Supports same-shape tensors, scalar add, and 1D bias
 * broadcast over the last dimension of a.
 */
int ops_add(const tensor_t *a, const tensor_t *b, tensor_t *out);

/* Elementwise scale: out = a * scale. */
int ops_scale(const tensor_t *a, float scale, tensor_t *out);

/* Softmax over the last dimension. */
int ops_softmax(const tensor_t *x, tensor_t *out);

/* LayerNorm over the last dimension. weight is required; bias may be NULL. */
int ops_layer_norm(const tensor_t *x,
                   const tensor_t *weight,
                   const tensor_t *bias,
                   tensor_t *out,
                   float eps);

/* GELU activation, elementwise. */
int ops_gelu(const tensor_t *x, tensor_t *out);

/* Embedding table lookup:
 *   weight:    [vocab_size, embed_dim]
 *   token_ids: token_count ids
 *   out:       [token_count, embed_dim]
 */
int ops_embedding_lookup(const tensor_t *weight,
                         const int32_t *token_ids,
                         size_t token_count,
                         tensor_t *out);

/* In-place rotary position embedding over the last dimension of q and k.
 * q and k must have identical shape and an even last dimension.
 */
int ops_rope(tensor_t *q, tensor_t *k, uint32_t pos);

#endif /* LLM_OPS_H */

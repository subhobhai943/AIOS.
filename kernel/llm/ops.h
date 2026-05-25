#ifndef OPS_H
#define OPS_H

/*
 * kernel/llm/ops.h — Phase 7.2: LLM math operations
 *
 * High-level tensor operations for transformer inference.
 * All ops dispatch to the SIMD kernels from kernel/simd.h
 * wherever possible; fall back to scalar otherwise.
 *
 * Freestanding C: no libc headers except <stdint.h>, <stddef.h>.
 * All tensor data buffers are assumed 32-byte aligned
 * (allocated via kmalloc_aligned(size, 32) through tensor_alloc).
 */

#include <stdint.h>
#include <stddef.h>
#include "tensor.h"

/* ─────────────────────────────────────────
 * 1. Element-wise and reduction ops
 * ───────────────────────────────────────── */

/*
 * ops_add — out[i] = a->data[i] + b->data[i]
 * a, b, out must have identical numel.
 * out may alias a or b.
 */
void ops_add(const tensor_t *a, const tensor_t *b, tensor_t *out);

/*
 * ops_scale — out[i] = t->data[i] * scalar
 * out may alias t.
 */
void ops_scale(const tensor_t *t, float scalar, tensor_t *out);

/*
 * ops_mul — element-wise multiply: out[i] = a[i] * b[i]
 * a, b, out must have identical numel.
 */
void ops_mul(const tensor_t *a, const tensor_t *b, tensor_t *out);

/*
 * ops_fill — set all elements of t to val.
 */
void ops_fill(tensor_t *t, float val);

/*
 * ops_copy — dst[i] = src[i], identical numel required.
 */
void ops_copy(tensor_t *dst, const tensor_t *src);

/* ─────────────────────────────────────────
 * 2. Matrix multiply
 * ───────────────────────────────────────── */

/*
 * ops_matmul — C = A × B
 *
 * A must be a 2-D tensor [M, K] (or a flat view of M*K floats).
 * B must be a 2-D tensor [K, N].
 * C must be pre-allocated as [M, N].
 *
 * Dispatches to simd_matmul_f32 (AVX2/AVX/SSE2/scalar).
 */
void ops_matmul(const tensor_t *A, const tensor_t *B, tensor_t *C);

/*
 * ops_matmul_add — C = A × B + bias
 * bias is a 1-D tensor of length N (broadcast across M rows).
 */
void ops_matmul_add(const tensor_t *A, const tensor_t *B,
                    const tensor_t *bias, tensor_t *C);

/* ─────────────────────────────────────────
 * 3. Activation functions
 * ───────────────────────────────────────── */

/*
 * ops_softmax — in-place softmax across the last dimension.
 * t must be 1-D [V] or 2-D [rows, V]; softmax is applied
 * independently to each row of length V (last dim).
 * For the 1-D case rows=1, V=numel.
 */
void ops_softmax(tensor_t *t);

/*
 * ops_gelu — GELU activation (GPT-2 tanh approximation).
 * out may alias t.
 */
void ops_gelu(const tensor_t *t, tensor_t *out);

/* ─────────────────────────────────────────
 * 4. Normalisation layers
 * ───────────────────────────────────────── */

/*
 * ops_layer_norm — GPT-2 LayerNorm
 *
 * y[i] = (x[i] - mean) / sqrt(var + eps) * weight[i] + bias[i]
 *
 * x, weight, bias, out must all be 1-D tensors of the same length.
 * eps is typically 1e-5.
 * out may alias x.
 */
void ops_layer_norm(const tensor_t *x,
                    const tensor_t *weight,
                    const tensor_t *bias,
                    tensor_t       *out,
                    float           eps);

/*
 * ops_rms_norm — LLaMA-style RMSNorm
 *
 * y[i] = x[i] / sqrt( mean(x^2) + eps ) * weight[i]
 *
 * x, weight, out must be 1-D tensors of the same length.
 * out may alias x.
 */
void ops_rms_norm(const tensor_t *x,
                  const tensor_t *weight,
                  tensor_t       *out,
                  float           eps);

/* ─────────────────────────────────────────
 * 5. Embedding lookup
 * ───────────────────────────────────────── */

/*
 * ops_embedding_lookup — gather token embeddings.
 *
 * table  : 2-D tensor [vocab_size, embed_dim] — the embedding weight matrix.
 * ids    : 1-D int32 array of token IDs, length seq_len.
 * out    : 2-D tensor [seq_len, embed_dim] — caller pre-allocated.
 *
 * Each row out[i] = table[ ids[i] ].  No bounds checking in release
 * builds; caller must ensure ids[i] < vocab_size.
 */
void ops_embedding_lookup(const tensor_t *table,
                          const int32_t  *ids,
                          int32_t         seq_len,
                          tensor_t       *out);

/* ─────────────────────────────────────────
 * 6. Rotary Position Embedding (RoPE)
 * ───────────────────────────────────────── */

/*
 * ops_rope — apply Rotary Position Embeddings in-place.
 *
 * RoPE rotates pairs of elements in the head dimension using the
 * position-dependent angle:
 *   theta_i = pos / (base ^ (2i / head_dim))
 * where base is typically 10000.0.
 *
 * q, k  : 3-D tensors [seq_len, n_heads, head_dim].
 *          Modified in-place (both Q and K are rotated).
 * pos   : sequence position of the first token in q/k
 *          (for incremental decoding, pos = current KV cache length).
 * base  : RoPE base frequency (10000.0 for LLaMA).
 *
 * head_dim must be even.
 */
void ops_rope(tensor_t *q, tensor_t *k,
              int32_t pos, float base);

/*
 * Freestanding scalar helpers shared by the LLM implementation.
 * These avoid pulling libc/libm into the kernel link.
 */
float ops_expf_approx(float x);
float ops_sqrtf_approx(float x);

#endif /* OPS_H */

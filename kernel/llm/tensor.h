#ifndef TENSOR_H
#define TENSOR_H

#include <stddef.h>
#include <stdint.h>

/* kernel/llm/tensor.h — Phase 7.1: minimal tensor abstraction
 *
 * Freestanding, no libc. This header defines a very small
 * tensor struct and helper functions used by the LLM engine.
 * Implementation lives in tensor.c.
 */

typedef struct tensor {
    float   *data;      /* contiguous buffer of numel floats */
    int32_t  dims[4];   /* up to 4 dimensions (e.g. [B, T, C]) */
    int32_t  ndim;      /* actual number of active dims (1..4) */
    size_t   numel;     /* product of dims[0..ndim-1] */
} tensor_t;

/* Allocate a new tensor with the given shape. All dims[i]
 * must be > 0. Returns NULL on failure. The buffer is left
 * uninitialised; callers typically follow with a fill.
 */

tensor_t *tensor_alloc(const int32_t *dims, int32_t ndim);

/* Free a tensor allocated by tensor_alloc. Safe to call with
 * NULL, in which case it is a no-op.
 */

void tensor_free(tensor_t *t);

/* Reshape tensor in-place by updating its dims / ndim. The
 * new shape must have the same numel as the existing one.
 * Returns 0 on success, -1 on invalid parameters.
 */

int tensor_reshape(tensor_t *t, const int32_t *new_dims, int32_t new_ndim);

/* Create a lightweight view (slice) of an existing tensor
 * along the first dimension. No data is copied; the returned
 * tensor_t points into the original buffer. It is the caller's
 * responsibility to ensure the lifetime of the parent tensor.
 *
 * start and count are in units of the leading dimension.
 * Returns 0 on success, -1 on invalid parameters.
 */

int tensor_slice(const tensor_t *t,
                 int32_t dim0_start,
                 int32_t dim0_count,
                 tensor_t *out_view);

/* Debug helper: print basic tensor metadata and the first few
 * elements to the serial log for inspection. Safe to call with
 * NULL (prints a brief "null tensor" line).
 */

void tensor_print(const tensor_t *t);

#endif /* TENSOR_H */

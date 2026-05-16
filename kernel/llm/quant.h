#ifndef AIOS_LLM_QUANT_H
#define AIOS_LLM_QUANT_H

/*
 * kernel/llm/quant.h — Phase 7.8: Quantization helpers
 *
 * Simple Q8_0 and Q4_K-style quantized weight layouts and
 * dequantisation / matmul helpers.  These are intentionally
 * generic so the loader can choose how to pack tensors.
 *
 * Constraints:
 *   - Freestanding C, no libc
 *   - Only <stdint.h>, <stddef.h>, <stdbool.h>
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Q8_0 layout: each weight is stored as signed int8 with a
 * per-row scale factor such that:
 *   w_real[row, col] ≈ scale[row] * q[row, col]
 */
typedef struct {
    const int8_t  *data;      /* [rows * cols] row-major */
    const float   *scales;    /* [rows] */
    uint32_t       rows;      /* output dim  */
    uint32_t       cols;      /* input  dim  */
} quant_q8_0_t;

/* Q4_K layout: 4-bit signed weights packed into bytes.
 * Two weights per byte, high nibble first.  Per-row scale
 * and zero-point are used to dequantise:
 *   w_real ≈ scale[row] * (q - zero[row])
 */
typedef struct {
    const uint8_t *data;      /* [rows * ceil(cols/2)] */
    const float   *scales;    /* [rows] */
    const float   *zeros;     /* [rows] zero-points, can be all 0 */
    uint32_t       rows;
    uint32_t       cols;
} quant_q4k_t;

/* Dequantise a single row of Q8_0 into full-precision floats. */
void quant_q8_0_dequant_row(const quant_q8_0_t *q,
                            uint32_t row,
                            float *dst);

/* Dequantise a single row of Q4_K into full-precision floats. */
void quant_q4k_dequant_row(const quant_q4k_t *q,
                           uint32_t row,
                           float *dst);

/* Mixed-precision matmul: y = W * x
 *
 * W is quantised; x and y are full-precision vectors.
 * This is a reference implementation intended for correctness
 * rather than maximum speed — SIMD kernels can be added later.
 */
void quant_q8_0_matvec(const quant_q8_0_t *w,
                       const float *x,
                       float *y);

void quant_q4k_matvec(const quant_q4k_t *w,
                      const float *x,
                      float *y);

#endif /* AIOS_LLM_QUANT_H */

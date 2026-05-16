/*
 * kernel/llm/quant.c — Phase 7.8: Quantization helpers
 *
 * Reference implementations for Q8_0 and Q4_K-style quantised
 * weight formats used by the LLM.  These are intentionally
 * simple and freestanding; higher-performance SIMD kernels can
 * be added later if needed.
 */

#include "quant.h"

/* ── Q8_0 helpers ───────────────────────────────────────────── */

void quant_q8_0_dequant_row(const quant_q8_0_t *q,
                            uint32_t row,
                            float *dst)
{
    if (!q || !dst) return;
    if (row >= q->rows) return;

    const int8_t *src   = q->data + (size_t)row * (size_t)q->cols;
    float        scale  = q->scales ? q->scales[row] : 1.0f;

    for (uint32_t c = 0; c < q->cols; c++) {
        dst[c] = (float)src[c] * scale;
    }
}

void quant_q8_0_matvec(const quant_q8_0_t *w,
                       const float *x,
                       float *y)
{
    if (!w || !x || !y) return;

    for (uint32_t r = 0; r < w->rows; r++) {
        const int8_t *row = w->data + (size_t)r * (size_t)w->cols;
        float scale = w->scales ? w->scales[r] : 1.0f;
        float acc = 0.0f;
        for (uint32_t c = 0; c < w->cols; c++) {
            acc += (float)row[c] * scale * x[c];
        }
        y[r] = acc;
    }
}

/* ── Q4_K helpers ───────────────────────────────────────────── */

static inline int8_t q4k_unpack(const uint8_t byte, uint32_t idx)
{
    /* idx=0 → high nibble, idx=1 → low nibble */
    uint8_t nibble = (idx == 0) ? (byte >> 4) : (byte & 0x0F);
    /* Interpret as signed 4-bit: range [-8, 7] */
    if (nibble & 0x8u) {
        return (int8_t)(nibble | 0xF0u); /* sign-extend */
    }
    return (int8_t)nibble;
}

void quant_q4k_dequant_row(const quant_q4k_t *q,
                           uint32_t row,
                           float *dst)
{
    if (!q || !dst) return;
    if (row >= q->rows) return;

    const uint8_t *src   = q->data + (size_t)row * ((q->cols + 1u) / 2u);
    float          scale = q->scales ? q->scales[row] : 1.0f;
    float          zero  = q->zeros  ? q->zeros[row]  : 0.0f;

    uint32_t cols = q->cols;
    uint32_t bytes = (cols + 1u) / 2u;

    uint32_t col = 0;
    for (uint32_t b = 0; b < bytes; b++) {
        uint8_t packed = src[b];
        for (uint32_t i = 0; i < 2 && col < cols; i++, col++) {
            int8_t qv = q4k_unpack(packed, i);
            float  fq = (float)qv - zero;
            dst[col] = fq * scale;
        }
    }
}

void quant_q4k_matvec(const quant_q4k_t *w,
                      const float *x,
                      float *y)
{
    if (!w || !x || !y) return;

    uint32_t cols = w->cols;
    uint32_t bytes_per_row = (cols + 1u) / 2u;

    for (uint32_t r = 0; r < w->rows; r++) {
        const uint8_t *row = w->data + (size_t)r * (size_t)bytes_per_row;
        float scale = w->scales ? w->scales[r] : 1.0f;
        float zero  = w->zeros  ? w->zeros[r]  : 0.0f;

        float acc = 0.0f;
        uint32_t col = 0;
        for (uint32_t b = 0; b < bytes_per_row; b++) {
            uint8_t packed = row[b];
            for (uint32_t i = 0; i < 2 && col < cols; i++, col++) {
                int8_t qv = q4k_unpack(packed, i);
                float  fq = ((float)qv - zero) * scale;
                acc += fq * x[col];
            }
        }
        y[r] = acc;
    }
}

#ifndef AIOS_LLM_LOADER_H
#define AIOS_LLM_LOADER_H

/* kernel/llm/loader.h — Phase 7.6
 *
 * GGUF v3 weight loader.
 *
 * Reads a GGUF file from the VFS, maps or copies the weight blob
 * into kernel memory, and fills an aios_model_t with zero-copy
 * tensor_t views so model_forward() can run immediately.
 *
 * Supported tensor types
 * ───────────────────
 *   GGML_TYPE_F32   — already float, direct view
 *   GGML_TYPE_F16   — dequantised to F32 on load
 *   GGML_TYPE_Q4_K  — dequantised to F32 on load  (4-bit K-quant)
 *   GGML_TYPE_Q8_0  — dequantised to F32 on load  (8-bit block)
 *
 * All dequantised output lives in a single kmalloc’d float arena
 * owned by the loader_ctx_t.  Freeing the context releases it all.
 *
 * Freestanding C — no libc.  Only <stdint.h>, <stddef.h>, <stdbool.h>.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "model.h"
#include "tensor.h"

/* ─────────────────────────────────────────────────────────────
 * GGML tensor types we handle
 * ───────────────────────────────────────────────────────────── */
typedef enum {
    GGML_TYPE_F32  = 0,
    GGML_TYPE_F16  = 1,
    GGML_TYPE_Q4_0 = 2,
    GGML_TYPE_Q4_1 = 3,
    GGML_TYPE_Q8_0 = 8,
    GGML_TYPE_Q4_K = 12,
    GGML_TYPE_Q6_K = 14,
} ggml_type_t;

/* ─────────────────────────────────────────────────────────────
 * Loader error codes
 * ───────────────────────────────────────────────────────────── */
typedef enum {
    LOADER_OK             =  0,
    LOADER_ERR_NOT_FOUND  = -1,   /* VFS open failed              */
    LOADER_ERR_BAD_MAGIC  = -2,   /* not a GGUF file              */
    LOADER_ERR_BAD_VER    = -3,   /* only v2 / v3 supported       */
    LOADER_ERR_OOM        = -4,   /* kmalloc returned NULL        */
    LOADER_ERR_BAD_TENSOR = -5,   /* unknown type / corrupt shape */
    LOADER_ERR_MISMATCH   = -6,   /* file dims != model_config_t  */
    LOADER_ERR_IO         = -7,   /* VFS read error               */
} loader_err_t;

/* ─────────────────────────────────────────────────────────────
 * Loader context
 *
 * Created by loader_open(), destroyed by loader_close().
 * The float_arena pointer owns all dequantised weight data;
 * the aios_model_t weight tensors are views into it.
 * ───────────────────────────────────────────────────────────── */
typedef struct {
    float    *float_arena;      /* kmalloc’d, all dequantised floats    */
    size_t    arena_bytes;      /* size of float_arena in bytes          */
    uint8_t  *raw_blob;         /* kmalloc’d raw file data (full file)   */
    size_t    raw_bytes;        /* size of raw_blob                      */
    uint32_t  n_tensors;        /* number of tensor entries in file      */
    uint32_t  gguf_version;     /* 2 or 3                                */
} loader_ctx_t;

/* ─────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────── */

/*
 * loader_load(vfs_path, cfg, model_out, ctx_out)
 *
 * High-level entry point.  Opens the GGUF file at vfs_path,
 * allocates the model via model_alloc(cfg), dequantises every
 * tensor, and fills model_out’s weight fields.
 *
 * On success: returns LOADER_OK, *model_out and *ctx_out are valid.
 * On failure: returns a negative loader_err_t; *model_out and
 *             *ctx_out are NULL.
 *
 * The caller must later call:
 *   model_free(*model_out);
 *   loader_close(*ctx_out);
 * in that order to release all memory.
 */
loader_err_t loader_load(const char          *vfs_path,
                         const model_config_t *cfg,
                         aios_model_t        **model_out,
                         loader_ctx_t        **ctx_out);

/*
 * loader_close(ctx)
 *
 * Free the float arena and raw blob owned by ctx, then free ctx.
 * Safe to call with NULL.
 */
void loader_close(loader_ctx_t *ctx);

/*
 * loader_err_str(err)
 *
 * Return a short human-readable string for a loader_err_t value.
 * Useful for error messages in the shell ‘load’ command.
 */
const char *loader_err_str(loader_err_t err);

#endif /* AIOS_LLM_LOADER_H */

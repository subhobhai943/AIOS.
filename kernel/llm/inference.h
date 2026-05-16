#ifndef AIOS_LLM_INFERENCE_H
#define AIOS_LLM_INFERENCE_H

/*
 * kernel/llm/inference.h — Phase 7.9: Inference manager
 *
 * High-level API that wraps loader/model/tokenizer into a
 * streaming text interface suitable for the shell and GUI.
 *
 * Freestanding C, no libc.  Only <stdint.h>, <stddef.h>,
 * <stdbool.h>.  All heap via kmalloc/kfree.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "model.h"
#include "tokenizer.h"

/* User callback invoked for each decoded token (piece of text). */
typedef void (*inference_token_cb_t)(const char *text,
                                     size_t      len,
                                     void       *user);

/* Simple configuration for the inference engine. */
typedef struct {
    model_config_t     model_cfg;     /* architecture + dims */
    tokenizer_config_t tok_cfg;       /* BOS/EOS/etc.        */
    sample_config_t    sample_cfg;    /* temp/top-k/top-p    */

    uint32_t           max_tokens;    /* max tokens per reply */
} inference_config_t;

/*
 * inference_init(model_path, vocab_path, merges_path, cfg)
 *
 * Load model weights and tokenizer metadata from the VFS.
 * (Exact on-disk format is defined by loader.c / tokenizer.c.)
 *
 * Returns 0 on success, negative value on error.
 */
int inference_init(const char              *model_path,
                   const char              *vocab_path,
                   const char              *merges_path,
                   const inference_config_t *cfg);

/*
 * inference_reset()
 *
 * Clear KV-cache and any internal buffers to start a new
 * conversation (chat session).  Does not reload weights.
 */
void inference_reset(void);

/*
 * inference_generate(prompt, cb, user)
 *
 * Encode prompt text to tokens, run auto-regressive generation
 * up to cfg.max_tokens or until EOS, and stream decoded text
 * pieces to the user callback.
 *
 * Returns 0 on success, negative on error.
 */
int inference_generate(const char           *prompt,
                       size_t                prompt_len,
                       inference_token_cb_t  cb,
                       void                 *user);

/*
 * inference_shutdown()
 *
 * Free all resources owned by the inference manager (model,
 * tokenizer, weight blobs, KV-cache).
 */
void inference_shutdown(void);

#endif /* AIOS_LLM_INFERENCE_H */

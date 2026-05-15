#ifndef AIOS_LLM_TOKENIZER_H
#define AIOS_LLM_TOKENIZER_H

/* kernel/llm/tokenizer.h — Phase 7.7
 *
 * BPE (Byte-Pair Encoding) tokenizer for AIOS LLM inference.
 *
 * Compatible with LLaMA-3 / LLaMA-2 / Mistral sentencepiece-style
 * GGUF vocabularies.
 *
 * Special token IDs (LLaMA-3 defaults — override via tokenizer_config_t)
 *   0   <unk>
 *   1   <s>   (BOS)
 *   2   </s>  (EOS)
 *
 * Freestanding C — no libc.
 * Only <stdint.h>, <stddef.h>, <stdbool.h>.
 * Heap: kmalloc / kfree.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ─────────────────────────────────────────────────────────────────────
 * Limits
 * ───────────────────────────────────────────────────────────────────── */
#define TOK_MAX_VOCAB       32000   /* max vocabulary size              */
#define TOK_MAX_TOKEN_LEN   64      /* max bytes in a single token str  */
#define TOK_MAX_SEQ         4096    /* max token IDs from one encode()  */
#define TOK_MAX_TEXT        65536   /* max input bytes to encode()      */

/* ─────────────────────────────────────────────────────────────────────
 * Error codes
 * ───────────────────────────────────────────────────────────────────── */
typedef enum {
    TOK_OK           =  0,
    TOK_ERR_OOM      = -1,   /* kmalloc returned NULL          */
    TOK_ERR_OVERFLOW = -2,   /* output buffer too small        */
    TOK_ERR_INVALID  = -3,   /* corrupt vocab or bad input     */
} tok_err_t;

/* ─────────────────────────────────────────────────────────────────────
 * Vocabulary entry
 *
 * token_str  — the UTF-8 string this token represents
 *              (NUL-terminated, ≤ TOK_MAX_TOKEN_LEN bytes)
 * score      — log-probability used by BPE merge ranking
 *              (higher = preferred merge, from GGUF scores array)
 * id         — index == token id (redundant but useful for reverse lookup)
 * is_special — true for <unk>, <s>, </s>, <pad>, <mask>, etc.
 * ───────────────────────────────────────────────────────────────────── */
typedef struct {
    char     token_str[TOK_MAX_TOKEN_LEN];
    float    score;
    uint32_t id;
    bool     is_special;
} vocab_entry_t;

/* ─────────────────────────────────────────────────────────────────────
 * BPE merge rule
 *
 * Stored as two token IDs (left, right) that merge into `result`.
 * The merge table is sorted by rank (index = rank, lower = higher priority).
 * ───────────────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t left;
    uint32_t right;
    uint32_t result;
} bpe_merge_t;

/* ─────────────────────────────────────────────────────────────────────
 * Tokenizer configuration
 * ───────────────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t bos_id;          /* begin-of-sequence token ID  (default 1) */
    uint32_t eos_id;          /* end-of-sequence token ID    (default 2) */
    uint32_t unk_id;          /* unknown token ID            (default 0) */
    uint32_t pad_id;          /* padding token ID            (default 0) */
    bool     add_bos;         /* prepend BOS on encode       (default true) */
    bool     add_eos;         /* append EOS on encode        (default false)*/
    bool     byte_fallback;   /* use <0xNN> tokens for unknown bytes       */
} tokenizer_config_t;

/* ─────────────────────────────────────────────────────────────────────
 * Tokenizer context (opaque to callers after init)
 * ───────────────────────────────────────────────────────────────────── */
typedef struct {
    vocab_entry_t   *vocab;        /* [vocab_size]  kmalloc'd              */
    bpe_merge_t     *merges;       /* [n_merges]    kmalloc'd              */
    uint32_t       **merge_index;  /* [vocab_size]  fast pair→rank lookup  */
    uint32_t         vocab_size;
    uint32_t         n_merges;
    tokenizer_config_t cfg;
} tokenizer_t;

/* ─────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────── */

/*
 * tokenizer_init_defaults(cfg)
 *
 * Fill a tokenizer_config_t with LLaMA-3 defaults.
 * Call before tokenizer_build() if you don't want to set fields manually.
 */
void tokenizer_init_defaults(tokenizer_config_t *cfg);

/*
 * tokenizer_build(tok, vocab_strings, scores, vocab_size,
 *                 merges_left, merges_right, n_merges, cfg)
 *
 * Build the tokenizer from raw arrays (as extracted from GGUF metadata
 * by the loader, or from a standalone vocab file).
 *
 *   vocab_strings  — array of NUL-terminated token strings (length vocab_size)
 *   scores         — log-probability per token (length vocab_size)
 *   merges_left/right — BPE merge pairs in priority order (length n_merges)
 *   cfg            — tokenizer configuration
 *
 * Returns TOK_OK or a negative tok_err_t on failure.
 * On failure, *tok is zeroed and no memory is leaked.
 */
tok_err_t tokenizer_build(tokenizer_t        *tok,
                           const char * const *vocab_strings,
                           const float        *scores,
                           uint32_t            vocab_size,
                           const uint32_t     *merges_left,
                           const uint32_t     *merges_right,
                           uint32_t            n_merges,
                           const tokenizer_config_t *cfg);

/*
 * tokenizer_free(tok)
 *
 * Release all heap memory owned by tok.
 * Safe to call on a zeroed tokenizer_t.
 */
void tokenizer_free(tokenizer_t *tok);

/*
 * tokenizer_encode(tok, text, text_len, ids_out, max_ids, n_out)
 *
 * BPE-encode a UTF-8 string into token IDs.
 *
 *   text      — input bytes (need not be NUL-terminated)
 *   text_len  — number of bytes to encode (≤ TOK_MAX_TEXT)
 *   ids_out   — caller-supplied output array
 *   max_ids   — capacity of ids_out (≤ TOK_MAX_SEQ)
 *   n_out     — set to number of token IDs written
 *
 * BOS is prepended if cfg.add_bos == true.
 * EOS is appended  if cfg.add_eos == true.
 *
 * Returns TOK_OK, TOK_ERR_OVERFLOW, or TOK_ERR_INVALID.
 */
tok_err_t tokenizer_encode(const tokenizer_t *tok,
                            const char        *text,
                            size_t             text_len,
                            uint32_t          *ids_out,
                            uint32_t           max_ids,
                            uint32_t          *n_out);

/*
 * tokenizer_decode(tok, ids, n_ids, buf_out, buf_size, len_out)
 *
 * Decode an array of token IDs into a UTF-8 string.
 *
 *   ids      — input token ID array
 *   n_ids    — number of IDs
 *   buf_out  — caller-supplied output buffer
 *   buf_size — capacity of buf_out (bytes)
 *   len_out  — set to number of bytes written (not including NUL)
 *
 * The BOS / EOS tokens are stripped from the output.
 * Byte-fallback tokens (<0xNN>) are decoded to their raw byte.
 *
 * Returns TOK_OK or TOK_ERR_OVERFLOW.
 */
tok_err_t tokenizer_decode(const tokenizer_t *tok,
                            const uint32_t    *ids,
                            uint32_t           n_ids,
                            char              *buf_out,
                            size_t             buf_size,
                            size_t            *len_out);

/*
 * tokenizer_token_to_str(tok, id)
 *
 * Return a pointer to the NUL-terminated token string for id.
 * Returns "<unk>" for out-of-range IDs.
 */
const char *tokenizer_token_to_str(const tokenizer_t *tok, uint32_t id);

/*
 * tokenizer_str_to_token(tok, str, str_len)
 *
 * Linear scan: return token ID matching str, or tok->cfg.unk_id.
 * Only used for special token lookup during init — not in the hot path.
 */
uint32_t tokenizer_str_to_token(const tokenizer_t *tok,
                                 const char        *str,
                                 size_t             str_len);

#endif /* AIOS_LLM_TOKENIZER_H */

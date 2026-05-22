/*
 * kernel/llm/inference.c — Phase 7.9: Inference manager
 *
 * High-level wrapper around loader/model/tokenizer providing
 * a streaming text interface for the shell and GUI.
 */

#include "inference.h"
#include "loader.h"
#include "tokenizer.h"
#include "model.h"

#include "../fs/vfs.h"
#include "../heap.h"
#include "../serial.h"

/* ── static engine state ──────────────────────────────────── */

static aios_model_t      *g_model      = (void*)0;
static loader_ctx_t      *g_loader_ctx = (void*)0;
static model_config_t     g_model_cfg;
static tokenizer_t        g_tok;
static inference_config_t g_cfg;
static bool               g_inited     = false;

/* Scratch buffer for logits and token IDs. */
#define INF_MAX_VOCAB   65536u
#define INF_MAX_TOKENS  4096u

static float    *g_logits = (void*)0;
static uint32_t *g_ids    = (void*)0;

/* ── public API ────────────────────────────────────────── */

int inference_init(const char               *model_path,
                   const char               *vocab_path,
                   const char               *merges_path,
                   const inference_config_t *cfg)
{
    if (!model_path || !vocab_path || !cfg) return -1;

    if (g_inited) {
        klog("[inference] already initialised, shutting down first\n");
        inference_shutdown();
    }

    g_cfg       = *cfg;
    g_model_cfg = cfg->model_cfg;

    /* 1. Load model weights via loader_load (uses VFS path directly) */
    loader_err_t lerr = loader_load(model_path, &g_model_cfg,
                                    &g_model, &g_loader_ctx);
    if (lerr != LOADER_OK) {
        klog("[inference] loader_load failed\n");
        return -1;
    }

    /* 2. Build tokenizer
     *
     * Full GGUF vocab extraction from blob is a future phase.
     * For now, initialise an empty tokenizer with defaults so the
     * rest of the pipeline compiles and runs without crashing.
     * The tokenizer will return TOK_ERR_INVALID on encode until
     * a proper vocab file parser is wired in.
     */
    tokenizer_config_t tcfg = cfg->tok_cfg;
    tok_err_t terr = tokenizer_build(&g_tok,
                                     (const char * const *)0, /* no strings */
                                     (const float *)0,        /* no scores  */
                                     0,                       /* vocab_size */
                                     (const uint32_t *)0,     /* no merges  */
                                     (const uint32_t *)0,
                                     0,
                                     &tcfg);
    (void)terr;   /* stub: errors here are non-fatal for compilation */
    (void)vocab_path;
    (void)merges_path;

    /* 3. Allocate logits + id buffers */
    uint32_t vocab = g_model_cfg.vocab_size;
    if (vocab > INF_MAX_VOCAB) vocab = INF_MAX_VOCAB;

    g_logits = (float *)kmalloc(sizeof(float) * (size_t)vocab);
    g_ids    = (uint32_t *)kmalloc(sizeof(uint32_t) * INF_MAX_TOKENS);
    if (!g_logits || !g_ids) {
        klog("[inference] OOM allocating logits/ids buffers\n");
        inference_shutdown();
        return -1;
    }

    g_inited = true;
    return 0;
}

void inference_reset(void)
{
    if (!g_inited || !g_model) return;
    model_reset_kvcache(g_model);
}

int inference_generate(const char           *prompt,
                       size_t                prompt_len,
                       inference_token_cb_t  cb,
                       void                 *user)
{
    if (!g_inited || !g_model || !cb) return -1;
    if (!prompt) { prompt = ""; prompt_len = 0; }

    /* 1. Encode prompt to token IDs */
    uint32_t n_ids = 0;
    tok_err_t terr = tokenizer_encode(&g_tok, prompt, prompt_len,
                                      g_ids, INF_MAX_TOKENS, &n_ids);
    if (terr != TOK_OK) {
        klog("[inference] tokenizer_encode failed\n");
        return -1;
    }

    /* 2. Prime KV-cache with all prompt tokens except last */
    uint32_t pos = 0;
    for (uint32_t i = 0; i + 1u < n_ids; i++) {
        if (model_forward(g_model, &g_model_cfg, g_ids[i], pos, g_logits) != 0)
            return -1;
        pos++;
    }

    /* 3. Autoregressive generation starting from last prompt token */
    uint32_t cur = (n_ids > 0) ? g_ids[n_ids - 1u] : g_cfg.tok_cfg.bos_id;
    uint32_t max_new = g_cfg.max_tokens ? g_cfg.max_tokens : 128u;

    char text_buf[256];

    for (uint32_t step = 0; step < max_new; step++) {
        if (model_forward(g_model, &g_model_cfg, cur, pos, g_logits) != 0)
            return -1;

        uint32_t next = model_sample(g_logits,
                                     g_model_cfg.vocab_size,
                                     &g_cfg.sample_cfg);

        if (next == g_cfg.tok_cfg.eos_id) break;

        /* decode single token to text */
        uint32_t one = next;
        size_t out_len = 0;
        tok_err_t d = tokenizer_decode(&g_tok, &one, 1,
                                       text_buf, sizeof(text_buf), &out_len);
        if (d != TOK_OK) {
            klog("[inference] tokenizer_decode failed\n");
            return -1;
        }

        if (out_len > 0) {
            cb(text_buf, out_len, user);
        }

        cur = next;
        pos++;
    }

    return 0;
}

void inference_shutdown(void)
{
    if (!g_inited) return;

    if (g_logits) { kfree(g_logits); g_logits = (void*)0; }
    if (g_ids)    { kfree(g_ids);    g_ids    = (void*)0; }

    tokenizer_free(&g_tok);

    if (g_model) {
        model_free(g_model);
        g_model = (void*)0;
    }

    if (g_loader_ctx) {
        loader_close(g_loader_ctx);
        g_loader_ctx = (void*)0;
    }

    g_inited = false;
}

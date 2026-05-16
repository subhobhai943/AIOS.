/*
 * kernel/llm/inference.c — Phase 7.9: Inference manager
 *
 * High-level wrapper around loader/model/tokenizer providing
 * a streaming text interface for the shell and GUI.
 */

#include "inference.h"
#include "loader.h"
#include "quant.h"

#include "../fs/vfs.h"
#include "../heap.h"
#include "../serial.h"

/* ── static engine state ───────────────────────────────────── */

static aios_model_t      *g_model      = (void*)0;
static model_config_t     g_model_cfg;
static tokenizer_t        g_tok;
static inference_config_t g_cfg;
static bool               g_inited     = false;

/* Scratch buffer for logits and token IDs. */
#define INF_MAX_VOCAB   65536u
#define INF_MAX_TOKENS  4096u

static float    *g_logits = (void*)0;
static uint32_t *g_ids    = (void*)0;

/* ── internal helpers ──────────────────────────────────────── */

static int load_entire_file(const char *path, uint8_t **out_buf, size_t *out_size)
{
    int fd = vfs_open(path);
    if (fd < 0) {
        klog("[inference] vfs_open failed for '%s'\n", path);
        return -1;
    }

    /* naive: read up to 4 MB into a kmalloc'd buffer */
    size_t cap = 4u * 1024u * 1024u;
    uint8_t *buf = (uint8_t *)kmalloc(cap);
    if (!buf) {
        klog("[inference] OOM reading '%s'\n", path);
        vfs_close(fd);
        return -1;
    }

    size_t off = 0;
    for (;;) {
        if (off >= cap) break;
        int n = vfs_read(fd, buf + off, (uint32_t)(cap - off));
        if (n <= 0) break;
        off += (size_t)n;
    }
    vfs_close(fd);

    *out_buf  = buf;
    *out_size = off;
    return 0;
}

/* ── public API ────────────────────────────────────────────── */

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

    g_cfg = *cfg;
    g_model_cfg = cfg->model_cfg;

    /* 1. Load model weights via loader.c */
    uint8_t *model_blob = 0;
    size_t   model_size = 0;
    if (load_entire_file(model_path, &model_blob, &model_size) != 0) {
        return -1;
    }

    aios_model_t *m = model_alloc(&g_model_cfg);
    if (!m) {
        klog("[inference] model_alloc failed\n");
        kfree(model_blob);
        return -1;
    }

    if (loader_load_model_from_blob(model_blob, model_size,
                                    &g_model_cfg, m) != 0) {
        klog("[inference] loader_load_model_from_blob failed\n");
        kfree(model_blob);
        model_free(m);
        return -1;
    }

    /* weights blob can be freed if loader copied views only */
    kfree(model_blob);

    g_model = m;

    /* 2. Load tokenizer metadata */
    uint8_t *vocab_blob = 0;
    size_t   vocab_size = 0;
    if (load_entire_file(vocab_path, &vocab_blob, &vocab_size) != 0) {
        klog("[inference] failed to read vocab file\n");
        return -1;
    }

    uint8_t *merges_blob = 0;
    size_t   merges_size = 0;
    if (merges_path) {
        if (load_entire_file(merges_path, &merges_blob, &merges_size) != 0) {
            klog("[inference] failed to read merges file\n");
            kfree(vocab_blob);
            return -1;
        }
    }

    tokenizer_config_t tcfg = cfg->tok_cfg;
    tokenizer_t tok;
    /* Loader helper parses GGUF or custom format into raw arrays. */
    if (loader_build_tokenizer_from_blobs(vocab_blob, vocab_size,
                                          merges_blob, merges_size,
                                          &tcfg, &tok) != 0) {
        klog("[inference] loader_build_tokenizer_from_blobs failed\n");
        kfree(vocab_blob);
        if (merges_blob) kfree(merges_blob);
        return -1;
    }

    kfree(vocab_blob);
    if (merges_blob) kfree(merges_blob);

    g_tok = tok;

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
        klog("[inference] tokenizer_encode failed (%d)\n", (int)terr);
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

    char   text_buf[256];
    size_t text_len = 0;

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
            klog("[inference] tokenizer_decode failed (%d)\n", (int)d);
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

    g_inited = false;
}

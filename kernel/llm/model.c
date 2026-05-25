/* kernel/llm/model.c — Phase 7.5
 *
 * Full transformer model: embedding lookup, N stacked transformer
 * blocks, final normalisation, LM-head projection, and a sampling
 * function (greedy / top-k / top-p) for auto-regressive generation.
 *
 * Design decisions
 * ────────────────
 * • model_alloc() allocates the block array and KV-cache via kmalloc;
 *   weight tensors (wte, wpe, ln_f_w, ln_f_b, lm_head) are filled
 *   in by the Phase 7.6 loader as zero-copy views into the weight
 *   blob — model.c never touches raw weight memory directly.
 *
 * • model_forward() uses a single static scratch buffer on the kernel
 *   stack so it never kmalloc’s inside the hot path.  The scratch
 *   buffer is MAX_EMBD floats; if n_embd > MAX_EMBD the call returns
 *   -1 immediately (loader validates dimensions at load time).
 *
 * • Sampling uses an xorshift64 PRNG seeded from sample_config_t.
 *   The PRNG state is advanced inside model_sample() and is NOT
 *   shared with anything else — thread-safe only if callers serialise.
 *
 * Constraints
 * ──────────
 * • Freestanding C: only <stdint.h>, <stddef.h>, <stdbool.h>
 * • No libm: float ops via __builtin_* / compiler intrinsics
 * • Heap: kmalloc / kfree only (heap.h)
 * • Compiler: x86_64-elf-gcc -ffreestanding -nostdlib
 *             -mno-red-zone -mcmodel=kernel
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "model.h"
#include "transformer.h"
#include "attention.h"
#include "ops.h"
#include "tensor.h"

#include "../heap.h"
#include "../serial.h"

/* ── compile-time limits ───────────────────────────────────────── */
/* Must be >= the largest n_embd you ever plan to run.              */
#define MAX_EMBD   4096
/* Maximum number of transformer layers                             */
#define MAX_LAYERS 128

/* ── tiny freestanding helpers ────────────────────────────────── */
static inline void model_memset_f(float *dst, float v, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = v;
}
static inline void model_memcpy_f(float *dst, const float *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}
static inline float model_expf(float x) { return ops_expf_approx(x); }

/* ── xorshift64 PRNG ─────────────────────────────────────────── */
static inline uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return (*state = x);
}

/* Uniform float in [0, 1) from the PRNG */
static inline float rng_float(uint64_t *state) {
    return (float)(xorshift64(state) >> 11) * (1.0f / (float)(1ULL << 53));
}

/* Helper to initialise a 1-D tensor view over `data` of length `len`. */
static inline void tensor_view_1d(tensor_t *t, float *data, int32_t len) {
    t->data  = data;
    t->ndim  = 1;
    t->dims[0] = len;
    t->dims[1] = t->dims[2] = t->dims[3] = 0;
    t->numel = (size_t)len;
}

/* ─────────────────────────────────────────────────────────────
 * model_alloc
 * ───────────────────────────────────────────────────────────── */
aios_model_t *model_alloc(const model_config_t *cfg)
{
    if (!cfg) return (void*)0;

    if (cfg->n_embd > MAX_EMBD) {
        klog("[model] n_embd exceeds MAX_EMBD\n");
        return (void*)0;
    }
    if (cfg->n_layers > MAX_LAYERS) {
        klog("[model] n_layers exceeds MAX_LAYERS\n");
        return (void*)0;
    }

    /* Allocate model struct */
    aios_model_t *m = (aios_model_t *)kmalloc(sizeof(aios_model_t));
    if (!m) { klog("[model] OOM: model struct\n"); return (void*)0; }

    /* Zero-out all fields so tensor data pointers are NULL */
    for (size_t i = 0; i < sizeof(aios_model_t); i++)
        ((uint8_t *)m)[i] = 0;

    /* Allocate block array */
    size_t block_bytes = sizeof(transformer_block_t) * cfg->n_layers;
    m->blocks = (transformer_block_t *)kmalloc(block_bytes);
    if (!m->blocks) {
        klog("[model] OOM: block array\n");
        kfree(m);
        return (void*)0;
    }
    for (size_t i = 0; i < block_bytes; i++)
        ((uint8_t *)m->blocks)[i] = 0;

    /* Allocate KV-cache */
    attn_config_t ac;
    ac.n_heads     = cfg->n_heads;
    ac.n_kv_heads  = cfg->n_kv_heads;
    ac.n_embd      = cfg->n_embd;
    ac.max_seq_len = cfg->max_seq_len;
    ac.n_layers    = cfg->n_layers;

    m->kvc = kvcache_alloc(&ac);
    if (!m->kvc) {
        klog("[model] OOM: KV-cache\n");
        kfree(m->blocks);
        kfree(m);
        return (void*)0;
    }

    klog("[model] alloc ok\n");
    return m;
}

/* ─────────────────────────────────────────────────────────────
 * model_free
 * ───────────────────────────────────────────────────────────── */
void model_free(aios_model_t *m)
{
    if (!m) return;
    if (m->kvc)    kvcache_free(m->kvc);
    if (m->blocks) kfree(m->blocks);
    kfree(m);
}

/* ─────────────────────────────────────────────────────────────
 * model_reset_kvcache
 * ───────────────────────────────────────────────────────────── */
void model_reset_kvcache(aios_model_t *m)
{
    if (m && m->kvc) kvcache_reset(m->kvc);
}

/* ─────────────────────────────────────────────────────────────
 * model_forward
 *
 * The hot path — one auto-regressive token step.
 *
 * Stack layout (static buffers, not heap):
 *   x[MAX_EMBD]  — running hidden state
 *   y[MAX_EMBD]  — scratch for block output
 * ───────────────────────────────────────────────────────────── */
int model_forward(aios_model_t        *m,
                  const model_config_t *cfg,
                  uint32_t              token_id,
                  uint32_t              pos,
                  float                *logits_out)
{
    if (!m || !cfg || !logits_out) return -1;
    if (token_id >= cfg->vocab_size) return -1;
    if (pos >= cfg->max_seq_len)     return -1;

    const uint32_t D = cfg->n_embd;
    if (D > MAX_EMBD) return -1;

    /* ── static scratch: two hidden-state vectors ───────────── */
    static float x[MAX_EMBD];
    static float y[MAX_EMBD];

    /* Build attention config for transformer blocks */
    attn_config_t ac;
    ac.n_heads     = cfg->n_heads;
    ac.n_kv_heads  = cfg->n_kv_heads;
    ac.n_embd      = cfg->n_embd;
    ac.max_seq_len = cfg->max_seq_len;
    ac.n_layers    = cfg->n_layers;

    /* ─────────────────────────────────────────────────────────
     * Step 1: Token embedding
     * wte layout: [vocab_size, n_embd] row-major
     * x = wte[token_id, :]
     * ───────────────────────────────────────────────────────── */
    {
        const float *row = m->wte.data + (size_t)token_id * D;
        model_memcpy_f(x, row, D);
    }

    /* ─────────────────────────────────────────────────────────
     * Step 2: Positional embedding (GPT-2 only)
     * LLaMA bakes position into RoPE inside the attention layer;
     * the wpe tensor is left empty (data == NULL) for LLaMA models.
     * x += wpe[pos, :]
     * ───────────────────────────────────────────────────────── */
    if (cfg->arch == MODEL_ARCH_GPT2 && m->wpe.data) {
        const float *pe = m->wpe.data + (size_t)pos * D;
        for (uint32_t i = 0; i < D; i++) x[i] += pe[i];
    }

    /* ─────────────────────────────────────────────────────────
     * Step 3: Transformer blocks
     * transformer_block_forward(block, &ac, x, y, kvc, layer, pos)
     * We pass the current hidden state x and receive updated state y,
     * then swap buffers for the next layer.
     * ───────────────────────────────────────────────────────── */
    for (uint32_t l = 0; l < cfg->n_layers; l++) {
        transformer_block_forward(&m->blocks[l], &ac, x, y, m->kvc,
                                  (int32_t)l, (int32_t)pos);
        model_memcpy_f(x, y, D);
    }

    /* ─────────────────────────────────────────────────────────
     * Step 4: Final layer norm
     * ops_layer_norm / ops_rms_norm depending on architecture.
     * Result overwrites x in-place.
     * ───────────────────────────────────────────────────────── */
    {
        tensor_t tx, tw, tb, tout;
        tensor_view_1d(&tx,   x,              (int32_t)D);
        tensor_view_1d(&tw,   m->ln_f_w.data, (int32_t)D);
        tensor_view_1d(&tout, x,              (int32_t)D);

        if (cfg->arch == MODEL_ARCH_LLAMA) {
            /* RMSNorm — bias is ignored (b is NULL/zero for LLaMA) */
            ops_rms_norm(&tx, &tw, &tout, cfg->layer_norm_eps);
        } else {
            tensor_view_1d(&tb, m->ln_f_b.data, (int32_t)D);
            ops_layer_norm(&tx, &tw, &tb, &tout, cfg->layer_norm_eps);
        }
    }

    /* ─────────────────────────────────────────────────────────
     * Step 5: LM-head projection
     * logits = lm_head @ x
     * lm_head layout: [vocab_size, n_embd] row-major
     * logits_out[v] = dot(lm_head[v,:], x)
     * ───────────────────────────────────────────────────────── */
    {
        const float *W = m->lm_head.data;
        for (uint32_t v = 0; v < cfg->vocab_size; v++) {
            float acc = 0.0f;
            const float *row = W + (size_t)v * D;
            for (uint32_t i = 0; i < D; i++) acc += row[i] * x[i];
            logits_out[v] = acc;
        }
    }

    /* Advance KV-cache position counter */
    if (m->kvc) m->kvc->cur_len++;

    return 0;
}

/* ─────────────────────────────────────────────────────────────
 * model_sample
 *
 * temperature  > 0 : divide logits, softmax, then sample
 *              == 0 : greedy argmax
 * top_k        > 0 : keep only the top-k highest-probability tokens
 * top_p        < 1 : nucleus — keep the smallest set of tokens
 *                    whose cumulative probability ≥ top_p
 *
 * When both top_k and top_p are active both filters are applied
 * (top_k first, then nucleus within the resulting set).
 * ───────────────────────────────────────────────────────────── */

/*
 * Internal: partial insertion sort to get top_k indices by logit value.
 * Operates in O(vocab * top_k) which is acceptable for typical k ≤ 40.
 */
static void topk_sort(const float *scores, uint32_t n,
                      uint32_t *out_idx, uint32_t k)
{
    /* initialise sentinel */
    for (uint32_t i = 0; i < k; i++) out_idx[i] = (uint32_t)-1;

    /* temp min-heap implemented as a flat sorted array of k elements */
    for (uint32_t v = 0; v < n; v++) {
        float sv = scores[v];
        /* If this score beats the worst in our current top-k, insert */
        float worst = (out_idx[k-1] == (uint32_t)-1)
                      ? -1e38f : scores[out_idx[k-1]];
        if (sv <= worst && out_idx[k-1] != (uint32_t)-1) continue;

        /* Shift down to find insertion point */
        int32_t ins = (int32_t)k - 1;
        while (ins > 0 && (
                out_idx[ins-1] == (uint32_t)-1 ||
                scores[out_idx[ins-1]] < sv)) {
            out_idx[ins] = out_idx[ins-1];
            ins--;
        }
        out_idx[ins] = v;
    }
}

uint32_t model_sample(const float          *logits,
                      uint32_t              vocab_size,
                      const sample_config_t *scfg)
{
    /* ── Greedy / argmax ────────────────────────────────────── */
    if (!scfg || scfg->temperature == 0.0f) {
        uint32_t best = 0;
        float    bval = logits[0];
        for (uint32_t v = 1; v < vocab_size; v++) {
            if (logits[v] > bval) { bval = logits[v]; best = v; }
        }
        return best;
    }

    /* ── Temperature scaling + softmax ────────────────────────── */
    static float probs[131072]; /* 128k tokens max — adjust if needed */
    uint32_t     n = vocab_size < 131072 ? vocab_size : 131072;

    float inv_temp = 1.0f / scfg->temperature;
    float max_l    = logits[0];
    for (uint32_t v = 1; v < n; v++)
        if (logits[v] > max_l) max_l = logits[v];

    float sum = 0.0f;
    for (uint32_t v = 0; v < n; v++) {
        probs[v] = model_expf((logits[v] - max_l) * inv_temp);
        sum += probs[v];
    }
    float inv_sum = 1.0f / sum;
    for (uint32_t v = 0; v < n; v++) probs[v] *= inv_sum;

    /* ── Top-k filter ────────────────────────────────────────── */
    static uint32_t topk_idx[128]; /* supports up to k=128           */
    uint32_t k = 0;

    if (scfg->top_k > 0 && scfg->top_k < n) {
        k = scfg->top_k < 128 ? scfg->top_k : 128;
        topk_sort(probs, n, topk_idx, k);
        /* zero out tokens not in top-k */
        static bool keep[131072];
        for (uint32_t v = 0; v < n; v++) keep[v] = false;
        for (uint32_t i = 0; i < k; i++)
            if (topk_idx[i] != (uint32_t)-1) keep[topk_idx[i]] = true;
        for (uint32_t v = 0; v < n; v++)
            if (!keep[v]) probs[v] = 0.0f;
        /* renormalise */
        sum = 0.0f;
        for (uint32_t v = 0; v < n; v++) sum += probs[v];
        if (sum > 0.0f) { inv_sum = 1.0f / sum;
            for (uint32_t v = 0; v < n; v++) probs[v] *= inv_sum; }
    }

    /* ── Top-p / nucleus filter ─────────────────────────────── */
    if (scfg->top_p > 0.0f && scfg->top_p < 1.0f) {
        uint32_t lim = n < 128 ? n : 128;
        topk_sort(probs, n, topk_idx, lim);
        float cumsum = 0.0f;
        bool  below  = true;
        for (uint32_t i = 0; i < lim && below; i++) {
            uint32_t v = topk_idx[i];
            if (v == (uint32_t)-1) break;
            cumsum += probs[v];
            if (cumsum >= scfg->top_p) below = false;
        }
        float thresh = 0.0f;
        cumsum = 0.0f;
        for (uint32_t i = 0; i < lim; i++) {
            uint32_t v = topk_idx[i];
            if (v == (uint32_t)-1) break;
            cumsum += probs[v];
            if (cumsum >= scfg->top_p) { thresh = probs[v]; break; }
        }
        for (uint32_t v = 0; v < n; v++)
            if (probs[v] < thresh) probs[v] = 0.0f;
        sum = 0.0f;
        for (uint32_t v = 0; v < n; v++) sum += probs[v];
        if (sum > 0.0f) { inv_sum = 1.0f / sum;
            for (uint32_t v = 0; v < n; v++) probs[v] *= inv_sum; }
    }

    /* ── Multinomial sample via inverse CDF ───────────────────── */
    uint64_t rng = scfg->rng_seed ^ 0xdeadbeefcafe1234ULL;
    if (rng == 0) rng = 0xc0ffee12345678ULL;
    float u = rng_float(&rng);

    float cdf = 0.0f;
    for (uint32_t v = 0; v < n; v++) {
        cdf += probs[v];
        if (u <= cdf) return v;
    }
    for (int32_t v = (int32_t)n - 1; v >= 0; v--)
        if (probs[v] > 0.0f) return (uint32_t)v;
    return 0;
}

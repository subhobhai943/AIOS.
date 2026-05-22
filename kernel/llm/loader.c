/* kernel/llm/loader.c — Phase 7.6
 *
 * GGUF v2/v3 weight loader for AIOS.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "loader.h"
#include "model.h"
#include "tensor.h"

#include "../heap.h"
#include "../serial.h"
#include "../fs/vfs.h"

/* ── GGUF magic & limits ─────────────────────────────────────── */
#define GGUF_MAGIC        0x46554747U
#define GGUF_VER_MIN      2
#define GGUF_VER_MAX      3
#define GGUF_ALIGN        32
#define MAX_TENSOR_NAME   128
#define MAX_TENSOR_DIMS   4

/* ── Q4_K block layout ───────────────────────────────────────── */
#define Q4K_BLOCK_SIZE    256
#define Q4K_BYTES_PER_BLK 144

/* ── Q8_0 block layout ───────────────────────────────────────── */
#define Q80_BLOCK_SIZE    32
#define Q80_BYTES_PER_BLK 34

/* ── tiny freestanding helpers ───────────────────────────────── */
static inline void ldr_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}
static inline int ldr_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
static inline void ldr_memset(void *dst, uint8_t v, size_t n) {
    uint8_t *d = (uint8_t *)dst; for (size_t i = 0; i < n; i++) d[i] = v;
}

/* ── FP16 → FP32 conversion ──────────────────────────────────── */
static float f16_to_f32(uint16_t h)
{
    uint32_t sign     = (uint32_t)(h >> 15) << 31;
    uint32_t exponent = (h >> 10) & 0x1F;
    uint32_t mantissa = h & 0x3FF;

    if (exponent == 0) {
        if (mantissa == 0) {
            uint32_t r = sign; float f; ldr_memcpy(&f, &r, 4); return f;
        }
        exponent = 1;
        while (!(mantissa & 0x400)) { mantissa <<= 1; exponent--; }
        mantissa &= 0x3FF;
    } else if (exponent == 31) {
        uint32_t r = sign | 0x7F800000U | (mantissa << 13);
        float f; ldr_memcpy(&f, &r, 4); return f;
    }
    uint32_t r = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float f; ldr_memcpy(&f, &r, 4); return f;
}

/* ── Cursor helpers ──────────────────────────────────────────── */
typedef struct { const uint8_t *base; size_t size; size_t pos; } cursor_t;

static inline bool cur_ok(const cursor_t *c, size_t need) {
    return c->pos + need <= c->size;
}
static inline uint8_t  cur_u8 (cursor_t *c) { return c->base[c->pos++]; }
static inline uint16_t cur_u16(cursor_t *c) {
    uint16_t v; ldr_memcpy(&v, c->base + c->pos, 2); c->pos += 2; return v;
}
static inline uint32_t cur_u32(cursor_t *c) {
    uint32_t v; ldr_memcpy(&v, c->base + c->pos, 4); c->pos += 4; return v;
}
static inline uint64_t cur_u64(cursor_t *c) {
    uint64_t v; ldr_memcpy(&v, c->base + c->pos, 8); c->pos += 8; return v;
}
static inline void cur_skip(cursor_t *c, size_t n) { c->pos += n; }
static inline void cur_align(cursor_t *c, size_t a) {
    size_t rem = c->pos % a;
    if (rem) c->pos += a - rem;
}

/* suppress unused-function warnings for cur_u8 / cur_u16 */
static inline void _ldr_suppress(void) {
    (void)cur_u8; (void)cur_u16;
}

/* ── Skip one metadata value ─────────────────────────────────── */
static bool skip_meta_value(cursor_t *c, uint32_t vtype);

static bool skip_string(cursor_t *c) {
    if (!cur_ok(c, 8)) return false;
    uint64_t len = cur_u64(c);
    if (!cur_ok(c, len)) return false;
    cur_skip(c, (size_t)len);
    return true;
}

static bool skip_meta_value(cursor_t *c, uint32_t vtype) {
    switch (vtype) {
    case 0: case 1:
        if (!cur_ok(c,1)) return false;
        cur_skip(c,1);
        return true;
    case 2: case 3:
        if (!cur_ok(c,2)) return false;
        cur_skip(c,2);
        return true;
    case 4: case 5: case 6: case 7:
        if (!cur_ok(c,4)) return false;
        cur_skip(c,4);
        return true;
    case 10: case 11: case 12:
        if (!cur_ok(c,8)) return false;
        cur_skip(c,8);
        return true;
    case 8:  return skip_string(c);
    case 9: {
        if (!cur_ok(c, 12)) return false;
        uint32_t etype = cur_u32(c);
        uint64_t cnt   = cur_u64(c);
        for (uint64_t i = 0; i < cnt; i++)
            if (!skip_meta_value(c, etype)) return false;
        return true;
    }
    default: return false;
    }
}

/* ── Dequantisation helpers ──────────────────────────────────── */
static void dequant_f16(const uint8_t *src, float *dst, size_t n_elem) {
    const uint16_t *h = (const uint16_t *)src;
    for (size_t i = 0; i < n_elem; i++) dst[i] = f16_to_f32(h[i]);
}

static void dequant_q8_0(const uint8_t *src, float *dst, size_t n_elem) {
    size_t n_blocks = n_elem / Q80_BLOCK_SIZE;
    for (size_t b = 0; b < n_blocks; b++) {
        const uint8_t *blk = src + b * Q80_BYTES_PER_BLK;
        uint16_t dh; ldr_memcpy(&dh, blk, 2);
        float d = f16_to_f32(dh);
        const int8_t *qs = (const int8_t *)(blk + 2);
        float *out = dst + b * Q80_BLOCK_SIZE;
        for (int i = 0; i < Q80_BLOCK_SIZE; i++)
            out[i] = d * (float)qs[i];
    }
}

static void dequant_q4_k(const uint8_t *src, float *dst, size_t n_elem)
{
    size_t n_blocks = n_elem / Q4K_BLOCK_SIZE;
    for (size_t b = 0; b < n_blocks; b++) {
        const uint8_t *blk = src + b * Q4K_BYTES_PER_BLK;
        uint16_t dh, dminh;
        ldr_memcpy(&dh,    blk + 0, 2);
        ldr_memcpy(&dminh, blk + 2, 2);
        float d    = f16_to_f32(dh);
        float dmin = f16_to_f32(dminh);

        const uint8_t *sc = blk + 4;
        uint8_t scales[8], mins[8];
        for (int i = 0; i < 4; i++) {
            scales[i]   =  sc[i]       & 0x3F;
            mins[i]     =  sc[i + 4]   & 0x3F;
            scales[i+4] = (sc[i+8]     & 0x0F) | ((sc[i]   >> 6) << 4);
            mins[i+4]   = (sc[i+8] >> 4)        | ((sc[i+4] >> 6) << 4);
        }

        const uint8_t *qs = blk + 16;
        float *out = dst + b * Q4K_BLOCK_SIZE;
        for (int s = 0; s < 8; s++) {
            float sc_f  = d    * (float)scales[s];
            float min_f = dmin * (float)mins[s];
            const uint8_t *q = qs + s * 16;
            float *o = out + s * 32;
            for (int j = 0; j < 16; j++) {
                o[j * 2 + 0] = sc_f * (float)(q[j] & 0x0F) - min_f;
                o[j * 2 + 1] = sc_f * (float)(q[j] >> 4)   - min_f;
            }
        }
    }
}

/* ── Map tensor name → float* slot in model ──────────────────── */
static float **find_tensor_slot(aios_model_t *m, const model_config_t *cfg,
                                const char *name, size_t name_len)
{
    if (ldr_strncmp(name, "token_embd.weight",    17) == 0) return &m->wte.data;
    if (ldr_strncmp(name, "position_embd.weight", 20) == 0) return &m->wpe.data;
    if (ldr_strncmp(name, "output_norm.weight",   18) == 0) return &m->ln_f_w.data;
    if (ldr_strncmp(name, "output_norm.bias",     16) == 0) return &m->ln_f_b.data;
    if (ldr_strncmp(name, "output.weight",        13) == 0) return &m->lm_head.data;

    if (name_len < 5) return (void*)0;
    if (name[0]!='b' || name[1]!='l' || name[2]!='k' || name[3]!='.') return (void*)0;

    uint32_t layer = 0;
    size_t   i = 4;
    while (i < name_len && name[i] >= '0' && name[i] <= '9') {
        layer = layer * 10 + (uint32_t)(name[i] - '0'); i++;
    }
    if (i >= name_len || name[i] != '.') return (void*)0;
    if (layer >= cfg->n_layers) return (void*)0;
    i++;

    const char *sub = name + i;
    transformer_block_t *blk = &m->blocks[layer];

    if (ldr_strncmp(sub, "attn_norm.weight",   16) == 0) return (float **)&blk->norm1_gamma;
    if (ldr_strncmp(sub, "attn_norm.bias",     14) == 0) return (float **)&blk->norm1_beta;
    if (ldr_strncmp(sub, "attn_q.weight",      13) == 0) return (float **)&blk->wq;
    if (ldr_strncmp(sub, "attn_k.weight",      13) == 0) return (float **)&blk->wk;
    if (ldr_strncmp(sub, "attn_v.weight",      13) == 0) return (float **)&blk->wv;
    if (ldr_strncmp(sub, "attn_output.weight", 18) == 0) return (float **)&blk->wo;
    if (ldr_strncmp(sub, "ffn_norm.weight",    15) == 0) return (float **)&blk->norm2_gamma;
    if (ldr_strncmp(sub, "ffn_norm.bias",      13) == 0) return (float **)&blk->norm2_beta;
    if (ldr_strncmp(sub, "ffn_gate.weight",    15) == 0) return (float **)&blk->w1;
    if (ldr_strncmp(sub, "ffn_up.weight",      13) == 0) return (float **)&blk->w2;
    if (ldr_strncmp(sub, "ffn_down.weight",    15) == 0) return (float **)&blk->w_gate;

    (void)name_len;
    return (void*)0;
}

/* ─────────────────────────────────────────────────────────────
 * loader_load
 * ───────────────────────────────────────────────────────────── */
loader_err_t loader_load(const char          *vfs_path,
                         const model_config_t *cfg,
                         aios_model_t        **model_out,
                         loader_ctx_t        **ctx_out)
{
    _ldr_suppress();
    *model_out = (void*)0;
    *ctx_out   = (void*)0;

    /* ── 1. Stat the file to get its size ───────────────────── */
    vfs_stat_t st;
    if (vfs_stat(vfs_path, &st) != 0) {
        klog("[loader] vfs_stat failed\n");
        return LOADER_ERR_NOT_FOUND;
    }
    if (st.size == 0) return LOADER_ERR_IO;

    /* ── 2. Open + read entire file ─────────────────────────── */
    int fd = vfs_open(vfs_path);
    if (fd < 0) {
        klog("[loader] vfs_open failed\n");
        return LOADER_ERR_NOT_FOUND;
    }

    loader_ctx_t *ctx = (loader_ctx_t *)kmalloc(sizeof(loader_ctx_t));
    if (!ctx) { vfs_close(fd); return LOADER_ERR_OOM; }
    ldr_memset(ctx, 0, sizeof(loader_ctx_t));

    ctx->raw_bytes = (size_t)st.size;
    ctx->raw_blob  = (uint8_t *)kmalloc(ctx->raw_bytes);
    if (!ctx->raw_blob) {
        kfree(ctx); vfs_close(fd); return LOADER_ERR_OOM;
    }

    int32_t nread = vfs_read(fd, ctx->raw_blob, (uint32_t)ctx->raw_bytes);
    vfs_close(fd);
    if (nread != (int32_t)ctx->raw_bytes) {
        kfree(ctx->raw_blob); kfree(ctx); return LOADER_ERR_IO;
    }
    klog("[loader] GGUF file read OK\n");

    /* ── 3. Parse GGUF header ───────────────────────────────── */
    cursor_t cur;
    cur.base = ctx->raw_blob;
    cur.size = ctx->raw_bytes;
    cur.pos  = 0;

    if (!cur_ok(&cur, 4)) { goto bad_magic; }
    uint32_t magic = cur_u32(&cur);
    if (magic != GGUF_MAGIC) { goto bad_magic; }

    uint32_t version = cur_u32(&cur);
    if (version < GGUF_VER_MIN || version > GGUF_VER_MAX) {
        klog("[loader] unsupported GGUF version\n");
        goto bad_ver;
    }
    ctx->gguf_version = version;

    uint64_t n_tensors = cur_u64(&cur);
    uint64_t n_kv      = cur_u64(&cur);
    ctx->n_tensors = (uint32_t)n_tensors;

    /* ── 4. Skip metadata KV pairs ──────────────────────────── */
    for (uint64_t k = 0; k < n_kv; k++) {
        if (!skip_string(&cur)) goto bad_kv;
        if (!cur_ok(&cur, 4)) goto bad_kv;
        uint32_t vtype = cur_u32(&cur);
        if (!skip_meta_value(&cur, vtype)) goto bad_kv;
    }

    /* ── 5. Read tensor info entries ────────────────────────── */
#define MAX_TENSORS 512
    typedef struct {
        char      name[MAX_TENSOR_NAME];
        uint32_t  name_len;
        uint64_t  n_elem;
        uint32_t  type;
        uint64_t  raw_offset;
        size_t    raw_bytes;
    } tinfo_t;

    tinfo_t *tinfos = (tinfo_t *)kmalloc(sizeof(tinfo_t) * MAX_TENSORS);
    if (!tinfos) goto oom;
    ldr_memset(tinfos, 0, sizeof(tinfo_t) * MAX_TENSORS);

    size_t arena_floats = 0;

    for (uint64_t t = 0; t < n_tensors && t < MAX_TENSORS; t++) {
        tinfo_t *ti = &tinfos[t];

        if (!cur_ok(&cur, 8)) goto bad_tensor;
        uint64_t nlen = cur_u64(&cur);
        if (nlen >= MAX_TENSOR_NAME || !cur_ok(&cur, nlen)) goto bad_tensor;
        ldr_memcpy(ti->name, cur.base + cur.pos, (size_t)nlen);
        ti->name[(size_t)nlen] = '\0';
        ti->name_len = (uint32_t)nlen;
        cur_skip(&cur, (size_t)nlen);

        if (!cur_ok(&cur, 4)) goto bad_tensor;
        uint32_t n_dims = cur_u32(&cur);
        if (n_dims > MAX_TENSOR_DIMS) goto bad_tensor;
        uint64_t ne = 1;
        for (uint32_t d = 0; d < n_dims; d++) {
            if (!cur_ok(&cur, 8)) goto bad_tensor;
            ne *= cur_u64(&cur);
        }
        ti->n_elem = ne;

        if (!cur_ok(&cur, 4)) goto bad_tensor;
        ti->type = cur_u32(&cur);

        if (!cur_ok(&cur, 8)) goto bad_tensor;
        ti->raw_offset = cur_u64(&cur);

        switch ((ggml_type_t)ti->type) {
        case GGML_TYPE_F32:  ti->raw_bytes = (size_t)ne * 4; break;
        case GGML_TYPE_F16:  ti->raw_bytes = (size_t)ne * 2; break;
        case GGML_TYPE_Q8_0:
            ti->raw_bytes = (ne / Q80_BLOCK_SIZE) * Q80_BYTES_PER_BLK; break;
        case GGML_TYPE_Q4_K:
            ti->raw_bytes = (ne / Q4K_BLOCK_SIZE) * Q4K_BYTES_PER_BLK; break;
        default:
            ti->raw_bytes = 0; break;
        }

        arena_floats += (size_t)ne;
    }

    /* ── 6. Locate data section start ───────────────────────── */
    cur_align(&cur, GGUF_ALIGN);
    size_t data_section_start = cur.pos;

    /* ── 7. Allocate float arena ────────────────────────────── */
    ctx->arena_bytes = arena_floats * sizeof(float);
    ctx->float_arena = (float *)kmalloc_aligned(ctx->arena_bytes, 32);
    if (!ctx->float_arena) {
        kfree(tinfos); goto oom;
    }

    /* ── 8. Allocate model struct ───────────────────────────── */
    aios_model_t *m = model_alloc(cfg);
    if (!m) { kfree(tinfos); goto oom; }

    /* ── 9. Dequantise + assign tensor pointers ─────────────── */
    float *arena_ptr = ctx->float_arena;

    for (uint32_t t = 0; t < ctx->n_tensors && t < MAX_TENSORS; t++) {
        tinfo_t *ti = &tinfos[t];
        if (ti->raw_bytes == 0) continue;

        const uint8_t *src = ctx->raw_blob + data_section_start + ti->raw_offset;
        if (data_section_start + ti->raw_offset + ti->raw_bytes > ctx->raw_bytes)
            continue;

        float *dst = arena_ptr;

        switch ((ggml_type_t)ti->type) {
        case GGML_TYPE_F32:  ldr_memcpy(dst, src, ti->raw_bytes); break;
        case GGML_TYPE_F16:  dequant_f16(src, dst, (size_t)ti->n_elem); break;
        case GGML_TYPE_Q8_0: dequant_q8_0(src, dst, (size_t)ti->n_elem); break;
        case GGML_TYPE_Q4_K: dequant_q4_k(src, dst, (size_t)ti->n_elem); break;
        default: continue;
        }

        float **slot_ptr = find_tensor_slot(m, cfg, ti->name, ti->name_len);
        if (slot_ptr) *slot_ptr = dst;

        arena_ptr += (size_t)ti->n_elem;
    }

    if (cfg->tie_weights && m->wte.data && !m->lm_head.data)
        m->lm_head = m->wte;

    kfree(tinfos);
    *model_out = m;
    *ctx_out   = ctx;
    return LOADER_OK;

    /* ── error labels ──────────────────────────────────────── */
bad_magic:
    kfree(ctx->raw_blob); kfree(ctx);
    return LOADER_ERR_BAD_MAGIC;
bad_ver:
    kfree(ctx->raw_blob); kfree(ctx);
    return LOADER_ERR_BAD_VER;
bad_kv:
    kfree(ctx->raw_blob); kfree(ctx);
    return LOADER_ERR_IO;
bad_tensor:
    kfree(ctx->raw_blob); kfree(ctx);
    return LOADER_ERR_BAD_TENSOR;
oom:
    if (ctx->float_arena) kfree_aligned(ctx->float_arena);
    kfree(ctx->raw_blob); kfree(ctx);
    return LOADER_ERR_OOM;
}

/* ─────────────────────────────────────────────────────────────
 * loader_close
 * ───────────────────────────────────────────────────────────── */
void loader_close(loader_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->float_arena) kfree_aligned(ctx->float_arena);
    if (ctx->raw_blob)    kfree(ctx->raw_blob);
    kfree(ctx);
}

/* ─────────────────────────────────────────────────────────────
 * loader_err_str
 * ───────────────────────────────────────────────────────────── */
const char *loader_err_str(loader_err_t err)
{
    switch (err) {
    case LOADER_OK:             return "ok";
    case LOADER_ERR_NOT_FOUND:  return "file not found";
    case LOADER_ERR_BAD_MAGIC:  return "not a GGUF file";
    case LOADER_ERR_BAD_VER:    return "unsupported GGUF version (need v2/v3)";
    case LOADER_ERR_OOM:        return "out of memory";
    case LOADER_ERR_BAD_TENSOR: return "corrupt or unsupported tensor";
    case LOADER_ERR_MISMATCH:   return "tensor dims mismatch model config";
    case LOADER_ERR_IO:         return "VFS read error";
    default:                    return "unknown error";
    }
}

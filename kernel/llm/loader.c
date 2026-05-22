/* kernel/llm/loader.c — Phase 7.6
 *
 * GGUF v2/v3 weight loader for AIOS.
 *
 * What it does
 * ──────────
 * 1. Opens the GGUF file via vfs_open / vfs_read (VFS, Phase 3.3).
 * 2. Reads the full file into a raw_blob (kmalloc’d).
 * 3. Parses the GGUF header: magic, version, tensor count,
 *    metadata key-value pairs (skipped — dimensions come from
 *    model_config_t which the caller already validated).
 * 4. For each tensor entry:
 *    a. Reads name, n_dims, shape, ggml_type, offset.
 *    b. Maps the tensor name to the corresponding field in
 *       aios_model_t (wte, wpe, ln_f_w, lm_head, block[l].*).
 *    c. Dequantises the raw bytes into a freshly-carved slice of
 *       the float_arena, then stores a tensor_t view in the model.
 * 5. Returns LOADER_OK; the model is ready for model_forward().
 *
 * GGUF binary format (v3, little-endian)
 * ───────────────────────────────
 *   [0]  uint32  magic        0x46554747 (‘GGUF’)
 *   [4]  uint32  version      2 or 3
 *   [8]  uint64  n_tensors
 *  [16]  uint64  n_kv         metadata key-value count
 *   … metadata KV pairs …
 *   … tensor info entries …
 *   … tensor data (aligned to 32 bytes in v3) …
 *
 * Each tensor info entry:
 *   uint64  name_len
 *   char    name[name_len]
 *   uint32  n_dims
 *   uint64  ne[n_dims]      element counts per dimension
 *   uint32  type            ggml_type_t
 *   uint64  offset          byte offset into data section
 *
 * Constraints
 * ──────────
 *   Freestanding C: only <stdint.h>, <stddef.h>, <stdbool.h>
 *   No libm: floats via __builtin_* only
 *   Heap: kmalloc / kfree
 *   Compiler: x86_64-elf-gcc -ffreestanding -nostdlib
 *             -mno-red-zone -mcmodel=kernel
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

/* ── GGUF magic & limits ────────────────────────────────────── */
#define GGUF_MAGIC        0x46554747U   /* 'GGUF' little-endian */
#define GGUF_VER_MIN      2
#define GGUF_VER_MAX      3
#define GGUF_ALIGN        32            /* data section alignment (v3) */
#define MAX_TENSOR_NAME   128
#define MAX_TENSOR_DIMS   4

/* ── Q4_K block layout (256 elements per block) ──────────────── */
/*
 * Q4_K super-block: 256 elements
 *   2  × float16  d, dmin         (super-block scales)
 *   12 × uint8    scales          (6-bit sub-block scales, packed)
 *   128 × uint8   qs              (nibble-packed 4-bit weights)
 * Total: 4 + 12 + 128 = 144 bytes per 256 elements
 */
#define Q4K_BLOCK_SIZE    256
#define Q4K_BYTES_PER_BLK 144

/* ── Q8_0 block layout (32 elements per block) ──────────────── */
/*
 * Q8_0 block:
 *   1 × float16  d     (block scale)
 *   32 × int8    qs    (quantised values)
 * Total: 2 + 32 = 34 bytes per 32 elements
 */
#define Q80_BLOCK_SIZE    32
#define Q80_BYTES_PER_BLK 34

/* ── tiny freestanding helpers ────────────────────────────────── */
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
static inline size_t ldr_strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}
/* memset for zeroing */
static inline void ldr_memset(void *dst, uint8_t v, size_t n) {
    uint8_t *d = (uint8_t *)dst; for (size_t i = 0; i < n; i++) d[i] = v;
}

/* ── FP16 → FP32 conversion (IEEE 754 half-precision) ─────────── */
static float f16_to_f32(uint16_t h)
{
    uint32_t sign     = (uint32_t)(h >> 15) << 31;
    uint32_t exponent = (h >> 10) & 0x1F;
    uint32_t mantissa = h & 0x3FF;

    if (exponent == 0) {
        /* subnormal / zero */
        if (mantissa == 0) {
            uint32_t r = sign; float f; ldr_memcpy(&f, &r, 4); return f;
        }
        /* normalise subnormal */
        exponent = 1;
        while (!(mantissa & 0x400)) { mantissa <<= 1; exponent--; }
        mantissa &= 0x3FF;
    } else if (exponent == 31) {
        /* inf / NaN */
        uint32_t r = sign | 0x7F800000U | (mantissa << 13);
        float f; ldr_memcpy(&f, &r, 4); return f;
    }
    uint32_t r = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float f; ldr_memcpy(&f, &r, 4); return f;
}

/* ── Cursor: safe read helpers over raw_blob ──────────────────── */
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

/* ── Skip one metadata value (type-tagged) ────────────────────── */
/*
 * GGUF metadata value types:
 *   0=u8, 1=i8, 2=u16, 3=i16, 4=u32, 5=i32,
 *   6=f32, 7=bool, 8=string, 9=array, 10=u64, 11=i64, 12=f64
 */
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
    case 0: case 1:  if (!cur_ok(c,1)) return false; cur_skip(c,1); return true;
    case 2: case 3:  if (!cur_ok(c,2)) return false; cur_skip(c,2); return true;
    case 4: case 5: case 6: case 7:
                     if (!cur_ok(c,4)) return false; cur_skip(c,4); return true;
    case 10: case 11: case 12:
                     if (!cur_ok(c,8)) return false; cur_skip(c,8); return true;
    case 8:          return skip_string(c);
    case 9: {
        /* array: elem_type (u32) + count (u64) + elements */
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

/* ── Dequantisation helpers ────────────────────────────────────── */

/* F16 → F32 (bulk) */
static void dequant_f16(const uint8_t *src, float *dst, size_t n_elem) {
    const uint16_t *h = (const uint16_t *)src;
    for (size_t i = 0; i < n_elem; i++) dst[i] = f16_to_f32(h[i]);
}

/*
 * Q8_0 dequant:
 *   Each 34-byte block: [f16 d][32 × i8 qs]
 *   output[i] = d * qs[i]
 */
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

/*
 * Q4_K dequant (super-block = 256 elements = 8 sub-blocks of 32):
 *
 * Super-block layout (144 bytes):
 *   [0..1]   f16  d         super-block scale
 *   [2..3]   f16  dmin      super-block min
 *   [4..15]  12 bytes       6-bit sub-block scales (packed 2 per byte,
 *                           lower 6 bits = scale, upper 6 bits = min_scale)
 *   [16..143] 128 bytes     nibble-packed 4-bit weights
 *             (each byte holds two 4-bit values: low nibble first)
 *
 * Reconstruction:
 *   For sub-block s (0..7), element j (0..31):
 *     scale_s = d    * (scales[s] & 63)
 *     min_s   = dmin * (scales[s] >> 6)   <- packed differently; see below
 *     x[j] = scale_s * nibble - min_s
 *
 * The 6-bit packing used by llama.cpp Q4_K:
 *   bytes [4..7]  hold lower 4 bits of scales[0..7]
 *   bytes [8..11] hold lower 4 bits of mins[0..7]
 *   bytes [12..15] hold upper 2 bits of scales and mins interleaved
 */
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

        /* Unpack 8 sub-block scales and 8 mins from 12 bytes */
        const uint8_t *sc = blk + 4;
        uint8_t scales[8], mins[8];
        for (int i = 0; i < 4; i++) {
            scales[i]   =  sc[i]       & 0x3F;
            mins[i]     =  sc[i + 4]   & 0x3F;
            scales[i+4] = (sc[i+8]     & 0x0F) | ((sc[i]   >> 6) << 4);
            mins[i+4]   = (sc[i+8] >> 4)        | ((sc[i+4] >> 6) << 4);
        }

        const uint8_t *qs = blk + 16;  /* 128 bytes of nibble data */
        float *out = dst + b * Q4K_BLOCK_SIZE;

        /* 8 sub-blocks of 32 elements each */
        for (int s = 0; s < 8; s++) {
            float sc_f  = d    * (float)scales[s];
            float min_f = dmin * (float)mins[s];
            const uint8_t *q = qs + s * 16;  /* 16 bytes = 32 nibbles */
            float *o = out + s * 32;
            for (int j = 0; j < 16; j++) {
                o[j * 2 + 0] = sc_f * (float)(q[j] & 0x0F) - min_f;
                o[j * 2 + 1] = sc_f * (float)(q[j] >> 4)   - min_f;
            }
        }
    }
}

/* ── Number of elements given ggml_type and byte count ───────────── */
static size_t ggml_n_elem(ggml_type_t t, size_t raw_bytes) {
    switch (t) {
    case GGML_TYPE_F32:  return raw_bytes / 4;
    case GGML_TYPE_F16:  return raw_bytes / 2;
    case GGML_TYPE_Q8_0: return (raw_bytes / Q80_BYTES_PER_BLK) * Q80_BLOCK_SIZE;
    case GGML_TYPE_Q4_K: return (raw_bytes / Q4K_BYTES_PER_BLK) * Q4K_BLOCK_SIZE;
    default:             return 0;
    }
}

/* ── Map a tensor name to the correct tensor_t* in aios_model_t ─── */
/*
 * GGUF tensor naming conventions (llama.cpp):
 *   token_embd.weight          → wte
 *   position_embd.weight       → wpe  (GPT-2 only)
 *   output_norm.weight         → ln_f_w
 *   output_norm.bias           → ln_f_b
 *   output.weight              → lm_head
 *   blk.N.attn_norm.weight     → blocks[N].norm1_gamma
 *   blk.N.attn_norm.bias       → blocks[N].norm1_beta
 *   blk.N.attn_q.weight        → blocks[N].wq
 *   blk.N.attn_k.weight        → blocks[N].wk
 *   blk.N.attn_v.weight        → blocks[N].wv
 *   blk.N.attn_output.weight   → blocks[N].wo
 *   blk.N.ffn_norm.weight      → blocks[N].norm2_gamma
 *   blk.N.ffn_norm.bias        → blocks[N].norm2_beta
 *   blk.N.ffn_gate.weight      → blocks[N].w1  (SwiGLU gate)
 *   blk.N.ffn_up.weight        → blocks[N].w2  (SwiGLU up / GELU)
 *   blk.N.ffn_down.weight      → blocks[N].w_gate  (projection or extra gate)
 */
static float **find_tensor_slot(aios_model_t *m, const model_config_t *cfg,
                                const char *name, size_t name_len)
{
    /* Global tensors */
    if (ldr_strncmp(name, "token_embd.weight",    17) == 0) return &m->wte.data;
    if (ldr_strncmp(name, "position_embd.weight", 20) == 0) return &m->wpe.data;
    if (ldr_strncmp(name, "output_norm.weight",   18) == 0) return &m->ln_f_w.data;
    if (ldr_strncmp(name, "output_norm.bias",     16) == 0) return &m->ln_f_b.data;
    if (ldr_strncmp(name, "output.weight",        13) == 0) return &m->lm_head.data;

    /* Block tensors: "blk.N.xxx" */
    if (name_len < 5) return (void*)0;
    if (name[0]!='b' || name[1]!='l' || name[2]!='k' || name[3]!='.') return (void*)0;

    /* Parse layer index */
    uint32_t layer = 0;
    size_t   i = 4;
    while (i < name_len && name[i] >= '0' && name[i] <= '9') {
        layer = layer * 10 + (uint32_t)(name[i] - '0'); i++;
    }
    if (i >= name_len || name[i] != '.') return (void*)0;
    if (layer >= cfg->n_layers) return (void*)0;
    i++;  /* skip '.' */

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
    return (void*)0;  /* unknown tensor — skip */
}

/* ─────────────────────────────────────────────────────────────
 * loader_load
 * ───────────────────────────────────────────────────────────── */
loader_err_t loader_load(const char          *vfs_path,
                         const model_config_t *cfg,
                         aios_model_t        **model_out,
                         loader_ctx_t        **ctx_out)
{
    *model_out = (void*)0;
    *ctx_out   = (void*)0;

    /* ── 1. Open file ───────────────────────────────────────── */
    int fd = vfs_open(vfs_path);
    if (fd < 0) {
        klog("[loader] vfs_open failed\n");
        return LOADER_ERR_NOT_FOUND;
    }

    /* Get file size via seek-to-end trick on VFS */
    int64_t file_size = vfs_size(fd);
    if (file_size <= 0) { vfs_close(fd); return LOADER_ERR_IO; }

    /* ── 2. Read entire file into raw_blob ────────────────────── */
    loader_ctx_t *ctx = (loader_ctx_t *)kmalloc(sizeof(loader_ctx_t));
    if (!ctx) { vfs_close(fd); return LOADER_ERR_OOM; }
    ldr_memset(ctx, 0, sizeof(loader_ctx_t));

    ctx->raw_bytes = (size_t)file_size;
    ctx->raw_blob  = (uint8_t *)kmalloc(ctx->raw_bytes);
    if (!ctx->raw_blob) {
        kfree(ctx); vfs_close(fd); return LOADER_ERR_OOM;
    }

    int32_t nread = vfs_read(fd, ctx->raw_blob, ctx->raw_bytes);
    vfs_close(fd);
    if (nread != (int32_t)ctx->raw_bytes) {
        kfree(ctx->raw_blob); kfree(ctx); return LOADER_ERR_IO;
    }
    klog("[loader] read GGUF file\n");

    /* ── 3. Parse GGUF header ─────────────────────────────────── */
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

    /* ── 4. Skip metadata KV pairs ───────────────────────────── */
    for (uint64_t k = 0; k < n_kv; k++) {
        /* key string */
        if (!skip_string(&cur)) goto bad_kv;
        /* value type */
        if (!cur_ok(&cur, 4)) goto bad_kv;
        uint32_t vtype = cur_u32(&cur);
        /* value */
        if (!skip_meta_value(&cur, vtype)) goto bad_kv;
    }

    /* ── 5. Read tensor info entries ───────────────────────────── */
    /*
     * We need two passes:
     *   Pass A: calculate total float_arena size needed.
     *   Pass B: dequantise each tensor into the arena.
     * To avoid re-parsing, save tensor info in a small stack array.
     */
#define MAX_TENSORS 512
    typedef struct {
        char      name[MAX_TENSOR_NAME];
        uint32_t  name_len;
        uint64_t  n_elem;      /* total number of elements */
        uint32_t  type;        /* ggml_type_t */
        uint64_t  raw_offset;  /* offset in data section */
        size_t    raw_bytes;   /* size of raw data */
    } tinfo_t;

    /* Allocate tinfo array on heap (can be up to 512 entries) */
    tinfo_t *tinfos = (tinfo_t *)kmalloc(sizeof(tinfo_t) * MAX_TENSORS);
    if (!tinfos) goto oom;
    ldr_memset(tinfos, 0, sizeof(tinfo_t) * MAX_TENSORS);

    size_t arena_floats = 0;   /* running count of floats needed */

    for (uint64_t t = 0; t < n_tensors && t < MAX_TENSORS; t++) {
        tinfo_t *ti = &tinfos[t];

        /* name */
        if (!cur_ok(&cur, 8)) goto bad_tensor;
        uint64_t nlen = cur_u64(&cur);
        if (nlen >= MAX_TENSOR_NAME || !cur_ok(&cur, nlen)) goto bad_tensor;
        ldr_memcpy(ti->name, cur.base + cur.pos, (size_t)nlen);
        ti->name[(size_t)nlen] = '\0';
        ti->name_len = (uint32_t)nlen;
        cur_skip(&cur, (size_t)nlen);

        /* n_dims + shape */
        if (!cur_ok(&cur, 4)) goto bad_tensor;
        uint32_t n_dims = cur_u32(&cur);
        if (n_dims > MAX_TENSOR_DIMS) goto bad_tensor;
        uint64_t ne = 1;
        for (uint32_t d = 0; d < n_dims; d++) {
            if (!cur_ok(&cur, 8)) goto bad_tensor;
            ne *= cur_u64(&cur);
        }
        ti->n_elem = ne;

        /* type */
        if (!cur_ok(&cur, 4)) goto bad_tensor;
        ti->type = cur_u32(&cur);

        /* data offset (relative to data section start — resolved below) */
        if (!cur_ok(&cur, 8)) goto bad_tensor;
        ti->raw_offset = cur_u64(&cur);

        /* compute raw byte count */
        switch ((ggml_type_t)ti->type) {
        case GGML_TYPE_F32:  ti->raw_bytes = (size_t)ne * 4; break;
        case GGML_TYPE_F16:  ti->raw_bytes = (size_t)ne * 2; break;
        case GGML_TYPE_Q8_0:
            ti->raw_bytes = (ne / Q80_BLOCK_SIZE) * Q80_BYTES_PER_BLK; break;
        case GGML_TYPE_Q4_K:
            ti->raw_bytes = (ne / Q4K_BLOCK_SIZE) * Q4K_BYTES_PER_BLK; break;
        default:
            ti->raw_bytes = 0; break;  /* skip on deq pass */
        }

        arena_floats += (size_t)ne;
    }

    /* ── 6. Locate data section start ───────────────────────────── */
    /* GGUF v3: data section starts at next GGUF_ALIGN boundary after
     * the last tensor info entry. */
    cur_align(&cur, GGUF_ALIGN);
    size_t data_section_start = cur.pos;

    /* ── 7. Allocate float arena ───────────────────────────────── */
    ctx->arena_bytes = arena_floats * sizeof(float);
    ctx->float_arena = (float *)kmalloc_aligned(ctx->arena_bytes, 32);
    if (!ctx->float_arena) {
        kfree(tinfos); goto oom;
    }

    /* ── 8. Allocate model struct ──────────────────────────────── */
    aios_model_t *m = model_alloc(cfg);
    if (!m) { kfree(tinfos); goto oom; }

    /* ── 9. Dequantise each tensor into arena, assign tensor views ─ */
    float *arena_ptr = ctx->float_arena;

    for (uint32_t t = 0; t < ctx->n_tensors && t < MAX_TENSORS; t++) {
        tinfo_t *ti = &tinfos[t];
        if (ti->raw_bytes == 0) continue;   /* unsupported type, skip */

        const uint8_t *src = ctx->raw_blob + data_section_start + ti->raw_offset;

        /* bounds check */
        if (data_section_start + ti->raw_offset + ti->raw_bytes > ctx->raw_bytes) {
            continue;
        }

        float *dst = arena_ptr;

        switch ((ggml_type_t)ti->type) {
        case GGML_TYPE_F32:
            ldr_memcpy(dst, src, ti->raw_bytes);
            break;
        case GGML_TYPE_F16:
            dequant_f16(src, dst, (size_t)ti->n_elem);
            break;
        case GGML_TYPE_Q8_0:
            dequant_q8_0(src, dst, (size_t)ti->n_elem);
            break;
        case GGML_TYPE_Q4_K:
            dequant_q4_k(src, dst, (size_t)ti->n_elem);
            break;
        default: continue;
        }

        /* Find the tensor slot in the model and assign the pointer */
        float **slot_ptr = find_tensor_slot(m, cfg, ti->name, ti->name_len);
        if (slot_ptr) {
            *slot_ptr = dst;
        }

        arena_ptr += (size_t)ti->n_elem;
    }

    /* Tie lm_head to wte if configured */
    if (cfg->tie_weights && m->wte.data && !m->lm_head.data) {
        m->lm_head = m->wte;
    }

    kfree(tinfos);
    *model_out = m;
    *ctx_out   = ctx;
    return LOADER_OK;

    /* ── error labels ────────────────────────────────────────── */
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

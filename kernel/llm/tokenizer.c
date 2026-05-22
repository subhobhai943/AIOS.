/* kernel/llm/tokenizer.c — Phase 7.7
 *
 * BPE tokenizer for AIOS LLM inference.
 *
 * Algorithm overview
 * ──────────────────
 * Encode (tokenizer_encode):
 *  1. Pre-tokenise: split text into individual UTF-8 bytes, mapping
 *     each to its corresponding single-byte token ID (byte_fallback vocab).
 *     Whitespace prefix handling: prepend '▁' (U+2581, 3 UTF-8 bytes)
 *     before the first non-space byte of each whitespace-separated word,
 *     matching SentencePiece behaviour used by LLaMA/Mistral.
 *  2. BPE merge loop:
 *     While any adjacent pair (left, right) has a merge rule:
 *       a. Find the lowest-rank merge among all adjacent pairs.
 *       b. Apply it: replace (left, right) with result everywhere.
 *       c. Repeat until no more merges apply.
 *     Implemented with a doubly-linked node array (static, stack-allocated
 *     for the sequence, O(n) per merge step, no heap in hot path).
 *  3. Walk the remaining nodes, output their token IDs.
 *  4. Optionally prepend BOS / append EOS.
 *
 * Decode (tokenizer_decode):
 *  1. For each token ID, look up the token string.
 *  2. Convert '▁' → ' ' (SentencePiece space marker).
 *  3. Decode <0xNN> byte-fallback tokens to raw bytes.
 *  4. Strip BOS/EOS token strings.
 *
 * Constraints
 * ───────────
 *  Freestanding C: <stdint.h>, <stddef.h>, <stdbool.h> only.
 *  No libm. No libc. Heap: kmalloc / kfree (kernel/heap.h).
 *  Compiler: x86_64-elf-gcc -ffreestanding -nostdlib
 *            -mno-red-zone -mcmodel=kernel
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "tokenizer.h"
#include "../heap.h"
#include "../serial.h"

/* ─────────────────────────────────────────────────────────────────────
 * Freestanding helpers (no libc)
 * ───────────────────────────────────────────────────────────────────── */
static inline size_t tok_strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}
static inline void tok_memcpy(void *d, const void *s, size_t n) {
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
}
static inline void tok_memset(void *d, uint8_t v, size_t n) {
    uint8_t *dd = (uint8_t *)d;
    for (size_t i = 0; i < n; i++) dd[i] = v;
}
static inline int tok_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)a[i] != (unsigned char)b[i])
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
static inline bool tok_isspace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Append bytes to a buffer, return false on overflow */
static inline bool buf_append(char *buf, size_t *pos, size_t cap,
                               const char *src, size_t len)
{
    if (*pos + len >= cap) return false;   /* leave room for NUL */
    tok_memcpy(buf + *pos, src, len);
    *pos += len;
    return true;
}

/* ─────────────────────────────────────────────────────────────────────
 * Minimal freestanding snprintf for klog formatting
 * ───────────────────────────────────────────────────────────────────── */
static void tok_u32_to_str(char *buf, size_t bufsz, uint32_t val) {
    if (bufsz == 0) return;
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[12]; int i = 0;
    while (val > 0 && i < 11) { tmp[i++] = (char)('0' + val % 10); val /= 10; }
    int j = 0;
    while (i > 0 && (size_t)j < bufsz - 1) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static size_t tok_klog_fmt(char *out, size_t outsz,
                            const char *fmt, uint32_t a, uint32_t b)
{
    size_t pos = 0;
    int arg = 0;
    for (const char *p = fmt; *p && pos + 1 < outsz; p++) {
        if (p[0] == '%' && p[1] == 'u') {
            char num[12];
            tok_u32_to_str(num, sizeof(num), arg == 0 ? a : b);
            for (int k = 0; num[k] && pos + 1 < outsz; k++)
                out[pos++] = num[k];
            p++; arg++;
        } else {
            out[pos++] = *p;
        }
    }
    out[pos] = '\0';
    return pos;
}

/* ─────────────────────────────────────────────────────────────────────
 * UTF-8 utilities
 * ───────────────────────────────────────────────────────────────────── */

/* Return number of bytes in the UTF-8 sequence starting at *b */
static inline int utf8_seq_len(uint8_t b) {
    if ((b & 0x80) == 0x00) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;  /* treat as single byte (byte-fallback) */
}

/* ▁ = U+2581 = 0xE2 0x96 0x81  (SentencePiece word boundary marker) */
#define SP_MARKER      "\xe2\x96\x81"
#define SP_MARKER_LEN  3

/* ─────────────────────────────────────────────────────────────────────
 * Merge index
 *
 * merge_index is a flat hash table: key = (left << 16 | right) % HASH_SIZE
 * Value stored = merge rank (index into merges[]).
 * Collisions resolved by open addressing (linear probe).
 * Built once in tokenizer_build(), used in O(1) per pair lookup.
 * ───────────────────────────────────────────────────────────────────── */
#define MERGE_HASH_SIZE  65536U   /* power of 2, must be > n_merges */
#define MERGE_EMPTY      0xFFFFFFFFU

typedef struct {
    uint64_t key;     /* packed (left << 32 | right) */
    uint32_t rank;    /* index into merges[] */
} merge_slot_t;

/* The hash table lives in the tokenizer_t as an opaque pointer */
/* We store it as merge_index[0] array of MERGE_HASH_SIZE slots */
/* Reusing the merge_index field (declared as uint32_t**) as merge_slot_t* */

static inline uint32_t merge_hash(uint32_t left, uint32_t right) {
    uint64_t k = ((uint64_t)left << 32) | right;
    /* FNV-1a inspired mix */
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    return (uint32_t)(k & (MERGE_HASH_SIZE - 1));
}

/* Look up rank for pair (left, right). Returns MERGE_EMPTY if not found. */
static uint32_t merge_lookup(const merge_slot_t *ht,
                              uint32_t left, uint32_t right)
{
    uint64_t key = ((uint64_t)left << 32) | right;
    uint32_t idx = merge_hash(left, right);
    for (uint32_t probe = 0; probe < MERGE_HASH_SIZE; probe++) {
        uint32_t i = (idx + probe) & (MERGE_HASH_SIZE - 1);
        if (ht[i].rank == MERGE_EMPTY) return MERGE_EMPTY;
        if (ht[i].key  == key)         return ht[i].rank;
    }
    return MERGE_EMPTY;
}

/* ─────────────────────────────────────────────────────────────────────
 * tokenizer_init_defaults
 * ───────────────────────────────────────────────────────────────────── */
void tokenizer_init_defaults(tokenizer_config_t *cfg)
{
    cfg->bos_id       = 1;
    cfg->eos_id       = 2;
    cfg->unk_id       = 0;
    cfg->pad_id       = 0;
    cfg->add_bos      = true;
    cfg->add_eos      = false;
    cfg->byte_fallback = true;
}

/* ─────────────────────────────────────────────────────────────────────
 * tokenizer_build
 * ───────────────────────────────────────────────────────────────────── */
tok_err_t tokenizer_build(tokenizer_t        *tok,
                           const char * const *vocab_strings,
                           const float        *scores,
                           uint32_t            vocab_size,
                           const uint32_t     *merges_left,
                           const uint32_t     *merges_right,
                           uint32_t            n_merges,
                           const tokenizer_config_t *cfg)
{
    tok_memset(tok, 0, sizeof(tokenizer_t));

    if (vocab_size == 0 || vocab_size > TOK_MAX_VOCAB)
        return TOK_ERR_INVALID;

    /* ── Allocate vocab array ── */
    tok->vocab = (vocab_entry_t *)kmalloc(sizeof(vocab_entry_t) * vocab_size);
    if (!tok->vocab) return TOK_ERR_OOM;
    tok_memset(tok->vocab, 0, sizeof(vocab_entry_t) * vocab_size);

    /* ── Fill vocab entries ── */
    for (uint32_t i = 0; i < vocab_size; i++) {
        vocab_entry_t *ve = &tok->vocab[i];
        ve->id    = i;
        ve->score = scores ? scores[i] : 0.0f;

        const char *s = vocab_strings[i];
        size_t slen = tok_strlen(s);
        if (slen >= TOK_MAX_TOKEN_LEN) slen = TOK_MAX_TOKEN_LEN - 1;
        tok_memcpy(ve->token_str, s, slen);
        ve->token_str[slen] = '\0';

        /* Mark special tokens */
        if (i == cfg->bos_id || i == cfg->eos_id ||
            i == cfg->unk_id || i == cfg->pad_id)
            ve->is_special = true;
    }

    tok->vocab_size = vocab_size;
    tok->cfg = *cfg;

    /* ── Allocate merge table ── */
    if (n_merges > 0) {
        tok->merges = (bpe_merge_t *)kmalloc(sizeof(bpe_merge_t) * n_merges);
        if (!tok->merges) { tokenizer_free(tok); return TOK_ERR_OOM; }

        for (uint32_t i = 0; i < n_merges; i++) {
            tok->merges[i].left   = merges_left[i];
            tok->merges[i].right  = merges_right[i];
            /* result: look up the combined string in vocab */
            /* Build combined string */
            char combined[TOK_MAX_TOKEN_LEN * 2];
            const char *ls = tok->vocab[merges_left[i]].token_str;
            const char *rs = tok->vocab[merges_right[i]].token_str;
            size_t ll = tok_strlen(ls);
            size_t rl = tok_strlen(rs);
            size_t cl = ll + rl;
            if (cl >= sizeof(combined)) cl = sizeof(combined) - 1;
            tok_memcpy(combined, ls, ll);
            tok_memcpy(combined + ll, rs, rl);
            combined[cl] = '\0';
            tok->merges[i].result = tokenizer_str_to_token(tok, combined, cl);
        }
        tok->n_merges = n_merges;
    }

    /* ── Build merge hash table ── */
    /*
     * We reuse merge_index field (uint32_t**) to store a merge_slot_t*
     * by casting.  The caller never dereferences merge_index directly.
     */
    merge_slot_t *ht = (merge_slot_t *)kmalloc(
        sizeof(merge_slot_t) * MERGE_HASH_SIZE);
    if (!ht) { tokenizer_free(tok); return TOK_ERR_OOM; }
    /* Initialise all slots to empty */
    for (uint32_t i = 0; i < MERGE_HASH_SIZE; i++) {
        ht[i].key  = 0xFFFFFFFFFFFFFFFFULL;
        ht[i].rank = MERGE_EMPTY;
    }
    /* Insert all merge rules */
    for (uint32_t r = 0; r < n_merges; r++) {
        uint32_t left  = tok->merges[r].left;
        uint32_t right = tok->merges[r].right;
        uint64_t key   = ((uint64_t)left << 32) | right;
        uint32_t idx   = merge_hash(left, right);
        for (uint32_t probe = 0; probe < MERGE_HASH_SIZE; probe++) {
            uint32_t i = (idx + probe) & (MERGE_HASH_SIZE - 1);
            if (ht[i].rank == MERGE_EMPTY) {
                ht[i].key  = key;
                ht[i].rank = r;
                break;
            }
        }
    }
    tok->merge_index = (uint32_t **)ht;  /* type-punned storage */

    {
        char _klog_buf[128];
        tok_klog_fmt(_klog_buf, sizeof(_klog_buf),
                     "[tokenizer] built: vocab=%u merges=%u\n",
                     vocab_size, n_merges);
        klog(_klog_buf);
    }
    return TOK_OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * tokenizer_free
 * ───────────────────────────────────────────────────────────────────── */
void tokenizer_free(tokenizer_t *tok)
{
    if (!tok) return;
    if (tok->vocab)        kfree(tok->vocab);
    if (tok->merges)       kfree(tok->merges);
    if (tok->merge_index)  kfree(tok->merge_index);  /* actually merge_slot_t* */
    tok_memset(tok, 0, sizeof(tokenizer_t));
}

/* ─────────────────────────────────────────────────────────────────────
 * tokenizer_str_to_token
 * Linear scan — only used during build / special token lookup.
 * ───────────────────────────────────────────────────────────────────── */
uint32_t tokenizer_str_to_token(const tokenizer_t *tok,
                                 const char        *str,
                                 size_t             str_len)
{
    for (uint32_t i = 0; i < tok->vocab_size; i++) {
        const char *vs = tok->vocab[i].token_str;
        size_t vl = tok_strlen(vs);
        if (vl == str_len && tok_strncmp(vs, str, str_len) == 0)
            return i;
    }
    return tok->cfg.unk_id;
}

/* ─────────────────────────────────────────────────────────────────────
 * tokenizer_token_to_str
 * ───────────────────────────────────────────────────────────────────── */
const char *tokenizer_token_to_str(const tokenizer_t *tok, uint32_t id)
{
    if (id >= tok->vocab_size) return "<unk>";
    return tok->vocab[id].token_str;
}

/* ─────────────────────────────────────────────────────────────────────
 * BPE encode — internal node list
 *
 * We use a fixed-size doubly-linked list of nodes on the stack.
 * Each node holds one token ID and links to prev/next.
 * Merges update the links in O(1); deleted nodes are marked with
 * id = UINT32_MAX.
 * ───────────────────────────────────────────────────────────────────── */
#define NODE_NONE  0xFFFFFFFFU

typedef struct bpe_node {
    uint32_t id;    /* current token ID          */
    uint32_t prev;  /* index of previous node    */
    uint32_t next;  /* index of next node        */
} bpe_node_t;

/* Maximum nodes: one per input byte + SP marker + BOS/EOS headroom */
#define MAX_NODES  (TOK_MAX_TEXT + 8)

/* Static buffer — lives in BSS, not on the stack (avoids stack overflow
 * since MAX_NODES × 12 bytes = ~768 KB)
 */
static bpe_node_t s_nodes[MAX_NODES];

/* ─────────────────────────────────────────────────────────────────────
 * byte_to_token_id
 *
 * Map a raw byte value to its token ID in the vocab.
 * LLaMA-3 byte-fallback format: "<0xNN>" where NN is uppercase hex.
 * If the exact token string is found, return its ID.
 * Otherwise fall back to unk_id.
 * ───────────────────────────────────────────────────────────────────── */
static uint32_t byte_to_token_id(const tokenizer_t *tok, uint8_t byte)
{
    /* Try single-char match first (ASCII printable byte tokens) */
    char single[2] = { (char)byte, '\0' };
    uint32_t id = tokenizer_str_to_token(tok, single, 1);
    if (id != tok->cfg.unk_id) return id;

    /* Try <0xNN> fallback format */
    char fb[8];
    fb[0] = '<'; fb[1] = '0'; fb[2] = 'x';
    uint8_t hi = byte >> 4;
    uint8_t lo = byte & 0x0F;
    fb[3] = hi < 10 ? ('0' + hi) : ('A' + hi - 10);
    fb[4] = lo < 10 ? ('0' + lo) : ('A' + lo - 10);
    fb[5] = '>';
    fb[6] = '\0';
    return tokenizer_str_to_token(tok, fb, 6);
}

/* ─────────────────────────────────────────────────────────────────────
 * pre_tokenize
 *
 * Split text into initial token IDs:
 *   - For each whitespace-separated word, prepend '▁' (SP_MARKER)
 *     to the first character.
 *   - Map each resulting UTF-8 sequence to a vocab token via
 *     tokenizer_str_to_token; fall back to byte-by-byte.
 *
 * Fills node_ids[0..n-1], returns n, or 0 on error.
 * ───────────────────────────────────────────────────────────────────── */
static uint32_t pre_tokenize(const tokenizer_t *tok,
                              const char        *text,
                              size_t             text_len,
                              uint32_t          *node_ids,
                              uint32_t           max_nodes)
{
    uint32_t n = 0;
    size_t   i = 0;

    while (i < text_len) {
        /* Check if we're at the start of a word (after whitespace or at pos 0) */
        bool word_start = (i == 0) || tok_isspace(text[i - 1]);

        /* Skip leading whitespace — SentencePiece encodes spaces as ▁ prefix */
        if (tok_isspace(text[i])) { i++; continue; }

        if (word_start) {
            /* Try to match '▁' + next UTF-8 char as a single token */
            int clen = utf8_seq_len((uint8_t)text[i]);
            char with_marker[SP_MARKER_LEN + 4 + 1];
            tok_memcpy(with_marker, SP_MARKER, SP_MARKER_LEN);
            if (i + (size_t)clen <= text_len) {
                tok_memcpy(with_marker + SP_MARKER_LEN, text + i, (size_t)clen);
                with_marker[SP_MARKER_LEN + clen] = '\0';
                uint32_t tid = tokenizer_str_to_token(
                    tok, with_marker, SP_MARKER_LEN + clen);
                if (tid != tok->cfg.unk_id) {
                    if (n >= max_nodes) return 0;
                    node_ids[n++] = tid;
                    i += (size_t)clen;
                    goto next_char;
                }
            }
            /* Emit ▁ marker token separately */
            uint32_t marker_id = tokenizer_str_to_token(
                tok, SP_MARKER, SP_MARKER_LEN);
            if (n >= max_nodes) return 0;
            node_ids[n++] = marker_id;
        }

        {
            /* Try to match the UTF-8 character as a vocab token */
            int clen = utf8_seq_len((uint8_t)text[i]);
            if (i + (size_t)clen <= text_len) {
                uint32_t tid = tokenizer_str_to_token(tok, text + i, (size_t)clen);
                if (tid != tok->cfg.unk_id) {
                    if (n >= max_nodes) return 0;
                    node_ids[n++] = tid;
                    i += (size_t)clen;
                    goto next_char;
                }
            }
            /* Byte fallback: emit one byte at a time */
            if (n >= max_nodes) return 0;
            node_ids[n++] = byte_to_token_id(tok, (uint8_t)text[i]);
            i++;
        }

    next_char:;
    }
    return n;
}

/* ─────────────────────────────────────────────────────────────────────
 * tokenizer_encode
 * ───────────────────────────────────────────────────────────────────── */
tok_err_t tokenizer_encode(const tokenizer_t *tok,
                            const char        *text,
                            size_t             text_len,
                            uint32_t          *ids_out,
                            uint32_t           max_ids,
                            uint32_t          *n_out)
{
    *n_out = 0;
    if (text_len == 0) {
        if (tok->cfg.add_bos) { ids_out[(*n_out)++] = tok->cfg.bos_id; }
        return TOK_OK;
    }
    if (text_len > TOK_MAX_TEXT) return TOK_ERR_OVERFLOW;

    /* ── Step 1: Pre-tokenise into node IDs ── */
    static uint32_t s_init_ids[MAX_NODES];
    uint32_t n_nodes = pre_tokenize(tok, text, text_len,
                                     s_init_ids, MAX_NODES);
    if (n_nodes == 0) return TOK_ERR_INVALID;

    /* ── Step 2: Build doubly-linked node list ── */
    for (uint32_t i = 0; i < n_nodes; i++) {
        s_nodes[i].id   = s_init_ids[i];
        s_nodes[i].prev = (i == 0)          ? NODE_NONE : i - 1;
        s_nodes[i].next = (i == n_nodes - 1)? NODE_NONE : i + 1;
    }
    uint32_t head = 0;
    uint32_t live = n_nodes;   /* number of live nodes */

    /* ── Step 3: BPE merge loop ── */
    const merge_slot_t *ht = (const merge_slot_t *)tok->merge_index;
    if (ht) {
        bool changed = true;
        while (changed && live > 1) {
            changed = false;
            uint32_t best_rank = MERGE_EMPTY;
            uint32_t best_node = NODE_NONE;

            /* Find the lowest-rank mergeable pair */
            for (uint32_t i = head; i != NODE_NONE; i = s_nodes[i].next) {
                uint32_t j = s_nodes[i].next;
                if (j == NODE_NONE) break;
                uint32_t rank = merge_lookup(ht,
                                             s_nodes[i].id, s_nodes[j].id);
                if (rank < best_rank) {
                    best_rank = rank;
                    best_node = i;
                }
            }
            if (best_node == NODE_NONE) break;

            /* Apply merge: replace node best_node with result,
             * unlink the next node */
            uint32_t i = best_node;
            uint32_t j = s_nodes[i].next;
            s_nodes[i].id   = tok->merges[best_rank].result;
            /* Unlink j */
            uint32_t jnext = s_nodes[j].next;
            s_nodes[i].next = jnext;
            if (jnext != NODE_NONE) s_nodes[jnext].prev = i;
            s_nodes[j].id = NODE_NONE;  /* mark deleted */
            live--;
            changed = true;
        }
    }

    /* ── Step 4: Collect output IDs ── */
    uint32_t out = 0;
    if (tok->cfg.add_bos) {
        if (out >= max_ids) return TOK_ERR_OVERFLOW;
        ids_out[out++] = tok->cfg.bos_id;
    }
    for (uint32_t i = head; i != NODE_NONE; i = s_nodes[i].next) {
        if (s_nodes[i].id == NODE_NONE) continue;
        if (out >= max_ids) return TOK_ERR_OVERFLOW;
        ids_out[out++] = s_nodes[i].id;
    }
    if (tok->cfg.add_eos) {
        if (out >= max_ids) return TOK_ERR_OVERFLOW;
        ids_out[out++] = tok->cfg.eos_id;
    }

    *n_out = out;
    return TOK_OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * decode_hex_byte
 *
 * Parse <0xNN> → byte value.  Returns -1 if not a valid fallback token.
 * ───────────────────────────────────────────────────────────────────── */
static int decode_hex_byte(const char *s)
{
    /* Expect exactly "<0xNN>" = 6 chars */
    if (s[0]!='<' || s[1]!='0' || s[2]!='x') return -1;
    if (s[5]!='>' || s[6]!='\0') return -1;
    uint8_t hi, lo;
    char c = s[3];
    if      (c >= '0' && c <= '9') hi = (uint8_t)(c - '0');
    else if (c >= 'A' && c <= 'F') hi = (uint8_t)(c - 'A' + 10);
    else if (c >= 'a' && c <= 'f') hi = (uint8_t)(c - 'a' + 10);
    else return -1;
    c = s[4];
    if      (c >= '0' && c <= '9') lo = (uint8_t)(c - '0');
    else if (c >= 'A' && c <= 'F') lo = (uint8_t)(c - 'A' + 10);
    else if (c >= 'a' && c <= 'f') lo = (uint8_t)(c - 'a' + 10);
    else return -1;
    return (int)((hi << 4) | lo);
}

/* ─────────────────────────────────────────────────────────────────────
 * tokenizer_decode
 * ───────────────────────────────────────────────────────────────────── */
tok_err_t tokenizer_decode(const tokenizer_t *tok,
                            const uint32_t    *ids,
                            uint32_t           n_ids,
                            char              *buf_out,
                            size_t             buf_size,
                            size_t            *len_out)
{
    size_t pos = 0;
    *len_out = 0;

    for (uint32_t t = 0; t < n_ids; t++) {
        uint32_t id = ids[t];

        /* Skip BOS / EOS */
        if (id == tok->cfg.bos_id || id == tok->cfg.eos_id) continue;

        if (id >= tok->vocab_size) {
            /* Unknown: emit nothing */
            continue;
        }

        const char *s = tok->vocab[id].token_str;
        size_t slen   = tok_strlen(s);

        /* Byte-fallback token? */
        if (slen == 6 && s[0] == '<' && s[1] == '0' && s[2] == 'x') {
            int byte_val = decode_hex_byte(s);
            if (byte_val >= 0) {
                if (pos + 1 >= buf_size) return TOK_ERR_OVERFLOW;
                buf_out[pos++] = (char)(uint8_t)byte_val;
                continue;
            }
        }

        /* SentencePiece ▁ → space (0xE2 0x96 0x81 → 0x20) */
        if (slen >= SP_MARKER_LEN &&
            (uint8_t)s[0] == 0xE2 &&
            (uint8_t)s[1] == 0x96 &&
            (uint8_t)s[2] == 0x81)
        {
            /* Emit a space then the rest of the token */
            if (t != 0) {  /* don't emit leading space for first token */
                if (pos + 1 >= buf_size) return TOK_ERR_OVERFLOW;
                buf_out[pos++] = ' ';
            }
            s    += SP_MARKER_LEN;
            slen -= SP_MARKER_LEN;
        }

        if (slen > 0) {
            if (!buf_append(buf_out, &pos, buf_size, s, slen))
                return TOK_ERR_OVERFLOW;
        }
    }

    buf_out[pos] = '\0';
    *len_out = pos;
    return TOK_OK;
}

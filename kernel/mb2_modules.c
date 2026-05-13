#include "mb2_modules.h"
#include "serial.h"

#ifndef PHYS_TO_VIRT
#  define PHYS_TO_VIRT(p) ((void *)(uintptr_t)(p))
#endif

/* Multiboot2 tag types we care about */
#define MB2_TAG_TYPE_END      0
#define MB2_TAG_TYPE_MODULE   3

/* Fixed-size header at the start of the MB2 info block */
typedef struct __attribute__((packed)) {
    uint32_t total_size;
    uint32_t _reserved;
} mb2_info_hdr_t;

/* Generic tag header */
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;   /* includes this header */
} mb2_tag_hdr_t;

/* Module tag (type 3) */
typedef struct __attribute__((packed)) {
    uint32_t type;         /* 3 */
    uint32_t size;
    uint32_t mod_start;    /* physical address */
    uint32_t mod_end;      /* physical address (exclusive) */
    char     string[1];    /* variable-length, null-terminated */
} mb2_module_tag_t;

/* ---- tiny no-libc helpers ----------------------------------------- */

static int m_strstr(const char *haystack, const char *needle) {
    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return 1;
        haystack++;
    }
    return 0;
}

/* ---- internal walker ---------------------------------------------- */

typedef void (*module_visitor_t)(const mb2_module_tag_t *tag, void *ctx);

static void mb2_walk_modules(uint64_t mb2_phys, module_visitor_t fn, void *ctx) {
    const uint8_t *info = (const uint8_t *)PHYS_TO_VIRT(mb2_phys);
    const mb2_info_hdr_t *hdr = (const mb2_info_hdr_t *)info;

    uint32_t offset = 8;  /* skip the 8-byte fixed header */
    while (offset < hdr->total_size) {
        const mb2_tag_hdr_t *tag =
            (const mb2_tag_hdr_t *)(info + offset);

        if (tag->type == MB2_TAG_TYPE_END)
            break;

        if (tag->type == MB2_TAG_TYPE_MODULE)
            fn((const mb2_module_tag_t *)tag, ctx);

        /* Tags are 8-byte aligned */
        uint32_t padded = (tag->size + 7u) & ~7u;
        offset += padded;
    }
}

/* ---- visitor contexts --------------------------------------------- */

typedef struct {
    uint64_t  phys;
    uint32_t  size;
    bool      found;
} find_ctx_t;

static void visitor_find_initrd(const mb2_module_tag_t *tag, void *ctx) {
    find_ctx_t *fc = (find_ctx_t *)ctx;
    if (fc->found) return;  /* keep first match */

    /* Accept any module whose string is empty OR contains "initrd" */
    const char *s = tag->string;
    if (s[0] == '\0' || m_strstr(s, "initrd")) {
        fc->phys  = tag->mod_start;
        fc->size  = tag->mod_end - tag->mod_start;
        fc->found = true;
    }
}

typedef struct {
    uint32_t count;
} count_ctx_t;

static void visitor_count(const mb2_module_tag_t *tag, void *ctx) {
    (void)tag;
    count_ctx_t *cc = (count_ctx_t *)ctx;
    cc->count++;
}

/* ---- public API ---------------------------------------------------- */

bool mb2_find_initrd(uint64_t mb2_phys, uint64_t *out_phys, uint32_t *out_size) {
    find_ctx_t fc = { 0, 0, false };
    mb2_walk_modules(mb2_phys, visitor_find_initrd, &fc);
    if (fc.found) {
        *out_phys = fc.phys;
        *out_size = fc.size;
        serial_puts(SERIAL_COM1, "[mb2] initrd module found\n");
    } else {
        serial_puts(SERIAL_COM1, "[mb2] no initrd module in MB2 tags\n");
    }
    return fc.found;
}

uint32_t mb2_count_modules(uint64_t mb2_phys) {
    count_ctx_t cc = { 0 };
    mb2_walk_modules(mb2_phys, visitor_count, &cc);
    return cc.count;
}

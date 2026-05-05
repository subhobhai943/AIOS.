#include "heap.h"
#include "vga.h"
#include <stddef.h>
#include <stdint.h>

typedef struct block_header {
    size_t              size;      /* usable bytes (excludes header) */
    int                 free;
    uint32_t            magic;
    struct block_header *next;
} __attribute__((aligned(16))) block_header_t;

#define HEADER_SIZE  sizeof(block_header_t)
#define MIN_SPLIT    (HEADER_SIZE + 16)

static block_header_t *heap_head = NULL;
static uint64_t heap_base  = 0;
static size_t   heap_total = 0;

/* ─── helpers ─────────────────────────────────────── */
void *kmemset(void *dst, int val, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)val;
    return dst;
}

void *kmemcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

/* ─── init ────────────────────────────────────────── */
void heap_init(uint64_t heap_start, size_t heap_size)
{
    heap_base  = heap_start;
    heap_total = heap_size;

    heap_head        = (block_header_t *)heap_start;
    heap_head->size  = heap_size - HEADER_SIZE;
    heap_head->free  = 1;
    heap_head->magic = HEAP_MAGIC;
    heap_head->next  = NULL;

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  [ OK ] Kernel heap initialised (");
    vga_putdec(heap_size / 1024);
    vga_puts(" KB)\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ─── kmalloc ─────────────────────────────────────── */
void *kmalloc(size_t size)
{
    if (!size) return NULL;
    /* Round up to 16-byte alignment */
    size = (size + 15) & ~15ULL;

    block_header_t *cur = heap_head;
    while (cur) {
        if (cur->free && cur->magic == HEAP_MAGIC && cur->size >= size) {
            /* Split block if enough room */
            if (cur->size >= size + MIN_SPLIT) {
                block_header_t *newblk = (block_header_t *)((uint8_t *)cur + HEADER_SIZE + size);
                newblk->size  = cur->size - size - HEADER_SIZE;
                newblk->free  = 1;
                newblk->magic = HEAP_MAGIC;
                newblk->next  = cur->next;
                cur->next = newblk;
                cur->size = size;
            }
            cur->free = 0;
            return (void *)((uint8_t *)cur + HEADER_SIZE);
        }
        cur = cur->next;
    }
    return NULL;   /* OOM */
}

void *kmalloc_aligned(size_t size, size_t alignment)
{
    /* Over-allocate to guarantee aligned pointer */
    void *raw = kmalloc(size + alignment);
    if (!raw) return NULL;
    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = (addr + alignment - 1) & ~(alignment - 1);
    return (void *)aligned;
}

/* ─── kfree ───────────────────────────────────────── */
void kfree(void *ptr)
{
    if (!ptr) return;
    block_header_t *blk = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    if (blk->magic != HEAP_MAGIC) return;   /* corruption guard */
    blk->free = 1;

    /* Coalesce adjacent free blocks */
    block_header_t *cur = heap_head;
    while (cur && cur->next) {
        if (cur->free && cur->next->free && cur->next->magic == HEAP_MAGIC) {
            cur->size += HEADER_SIZE + cur->next->size;
            cur->next  = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

void *krealloc(void *ptr, size_t new_size)
{
    if (!ptr) return kmalloc(new_size);
    block_header_t *blk = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    if (new_size <= blk->size) return ptr;
    void *newptr = kmalloc(new_size);
    if (!newptr) return NULL;
    kmemcpy(newptr, ptr, blk->size);
    kfree(ptr);
    return newptr;
}

void heap_dump_stats(void)
{
    size_t total_free = 0, total_used = 0, blocks = 0;
    block_header_t *cur = heap_head;
    while (cur) {
        blocks++;
        if (cur->free) total_free += cur->size;
        else           total_used += cur->size;
        cur = cur->next;
    }
    vga_puts("Heap: blocks=");
    vga_putdec(blocks);
    vga_puts(" used=");
    vga_putdec(total_used);
    vga_puts(" free=");
    vga_putdec(total_free);
    vga_puts("\n");
}

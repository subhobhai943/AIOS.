/* ============================================================
 * AIOS — Kernel Heap  (kmalloc / kfree)
 *
 * Free-list allocator.  A block_header_t sits immediately before
 * every allocation.  Headers are 16-byte aligned throughout.
 *
 * kmalloc_aligned() bug fix
 * -------------------------
 * The old version over-allocated, bumped the pointer to the
 * aligned address, and returned it — but kfree() recovers the
 * header by subtracting HEADER_SIZE, so it would compute the
 * wrong address and either corrupt memory or panic on the magic
 * check.
 *
 * Fix: we store the original (unaligned) kmalloc pointer in the
 * 8 bytes immediately before the aligned return address.
 * kfree_aligned() reads it back and calls kfree() on the raw ptr.
 * A PUBLIC aligned-free function (kfree_aligned) is added so
 * callers know which flavour of free to use.
 *
 * Coalesce fix
 * ------------
 * Old kfree() merged only one forward step per call, so a long
 * free sequence left many small blocks.  The new version runs a
 * full forward-merge pass over the entire list after every free.
 * ============================================================ */

#include <stddef.h>
#include <stdint.h>
#include "include/heap.h"
#include "include/vga.h"

/* ---------------------------------------------------------------
 * Block header
 * --------------------------------------------------------------- */
typedef struct block_header {
    size_t               size;   /* usable bytes (excludes header) */
    uint32_t             free;
    uint32_t             magic;
    struct block_header *next;
} __attribute__((aligned(16))) block_header_t;

#define HEADER_SIZE   sizeof(block_header_t)
#define MIN_SPLIT     (HEADER_SIZE + 16u)   /* don't create a crumb block */

static block_header_t *heap_head  = (block_header_t *)0;
static size_t          heap_total = 0;

/* ---------------------------------------------------------------
 * Memory utilities (no libc)
 * --------------------------------------------------------------- */
void *kmemset(void *dst, int val, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)val;
    return dst;
}

void *kmemcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

int kmemcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

/* ---------------------------------------------------------------
 * heap_init
 * --------------------------------------------------------------- */
void heap_init(uint64_t heap_start, size_t heap_size)
{
    heap_total = heap_size;
    heap_head  = (block_header_t *)(uintptr_t)heap_start;

    heap_head->size  = heap_size - HEADER_SIZE;
    heap_head->free  = 1u;
    heap_head->magic = HEAP_MAGIC;
    heap_head->next  = (block_header_t *)0;

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  [ OK ] Heap init — ");
    vga_putdec((uint64_t)(heap_size / 1024));
    vga_puts(" KB at 0x");
    vga_puthex(heap_start);
    vga_puts("\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ---------------------------------------------------------------
 * coalesce — merge all adjacent free blocks (full-pass).
 * Called after every kfree().
 * --------------------------------------------------------------- */
static void coalesce(void)
{
    block_header_t *cur = heap_head;
    while (cur && cur->next) {
        if (cur->free
            && cur->next->free
            && cur->next->magic == HEAP_MAGIC)
        {
            cur->size += HEADER_SIZE + cur->next->size;
            cur->next  = cur->next->next;
            /* don't advance cur — try merging again from same node */
        } else {
            cur = cur->next;
        }
    }
}

/* ---------------------------------------------------------------
 * kmalloc — first-fit allocator, 16-byte aligned.
 * --------------------------------------------------------------- */
void *kmalloc(size_t size)
{
    if (!size) return (void *)0;
    size = (size + 15u) & ~15u;   /* round up to 16-byte multiple */

    block_header_t *cur = heap_head;
    while (cur) {
        if (cur->free && cur->magic == HEAP_MAGIC && cur->size >= size) {
            /* Split if remaining space after allocation is big enough */
            if (cur->size >= size + MIN_SPLIT) {
                block_header_t *nb = (block_header_t *)(
                    (uint8_t *)cur + HEADER_SIZE + size);
                nb->size  = cur->size - size - HEADER_SIZE;
                nb->free  = 1u;
                nb->magic = HEAP_MAGIC;
                nb->next  = cur->next;
                cur->next = nb;
                cur->size = size;
            }
            cur->free = 0u;
            return (void *)((uint8_t *)cur + HEADER_SIZE);
        }
        cur = cur->next;
    }
    return (void *)0;   /* OOM */
}

/* ---------------------------------------------------------------
 * kcalloc — kmalloc + zero-fill.
 * --------------------------------------------------------------- */
void *kcalloc(size_t count, size_t elem_size)
{
    size_t total = count * elem_size;
    void  *ptr   = kmalloc(total);
    if (ptr) kmemset(ptr, 0, total);
    return ptr;
}

/* ---------------------------------------------------------------
 * kmalloc_aligned — return a pointer aligned to `alignment` bytes
 * (must be a power of two, >= 8).
 *
 * Layout of the raw allocation:
 *
 *   [ kmalloc raw block ]
 *   ....padding....
 *   [ uint64_t: raw_ptr stored here  ]  ← always 8 bytes before
 *   [ aligned user data              ]  ← returned to caller
 *
 * kfree_aligned() reads raw_ptr and calls kfree() on it.
 * --------------------------------------------------------------- */
void *kmalloc_aligned(size_t size, size_t alignment)
{
    if (!alignment || (alignment & (alignment - 1)))
        return (void *)0;   /* alignment must be power-of-two */
    if (alignment < 8) alignment = 8;

    /* Over-allocate: worst-case we need (alignment - 1) extra bytes
     * plus 8 bytes to store the raw pointer.                       */
    void *raw = kmalloc(size + alignment + 8u);
    if (!raw) return (void *)0;

    uintptr_t raw_addr     = (uintptr_t)raw;
    uintptr_t aligned_addr = (raw_addr + 8u + alignment - 1u)
                             & ~(alignment - 1u);

    /* Store raw pointer in the 8 bytes immediately before aligned */
    *((uintptr_t *)(aligned_addr - sizeof(uintptr_t))) = raw_addr;

    return (void *)aligned_addr;
}

void kfree_aligned(void *aligned_ptr)
{
    if (!aligned_ptr) return;
    uintptr_t raw_addr = *((uintptr_t *)((uint8_t *)aligned_ptr
                                         - sizeof(uintptr_t)));
    kfree((void *)raw_addr);
}

/* ---------------------------------------------------------------
 * kfree
 * --------------------------------------------------------------- */
void kfree(void *ptr)
{
    if (!ptr) return;
    block_header_t *blk = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    if (blk->magic != HEAP_MAGIC) return;   /* corruption guard */
    if (blk->free)                return;   /* double-free guard  */
    blk->free = 1u;
    coalesce();
}

/* ---------------------------------------------------------------
 * krealloc
 * --------------------------------------------------------------- */
void *krealloc(void *ptr, size_t new_size)
{
    if (!ptr)     return kmalloc(new_size);
    if (!new_size) { kfree(ptr); return (void *)0; }

    block_header_t *blk = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    if (blk->magic != HEAP_MAGIC) return (void *)0;

    /* Already big enough — return in-place */
    if (new_size <= blk->size) return ptr;

    void *np = kmalloc(new_size);
    if (!np) return (void *)0;
    kmemcpy(np, ptr, blk->size);
    kfree(ptr);
    return np;
}

/* ---------------------------------------------------------------
 * heap_dump_stats
 * --------------------------------------------------------------- */
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
    vga_putdec((uint64_t)blocks);
    vga_puts(" used=");
    vga_putdec((uint64_t)total_used);
    vga_puts(" free=");
    vga_putdec((uint64_t)total_free);
    vga_puts("\n");
}

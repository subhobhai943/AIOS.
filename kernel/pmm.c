/* ============================================================
 * AIOS — Physical Memory Manager (PMM)
 * Bitmap allocator: 1 bit per 4 KB frame.
 * Supports up to 4 GB (1 M frames → 128 KB bitmap in BSS).
 * ============================================================ */

#include <stdint.h>
#include <stddef.h>
#include "include/pmm.h"
#include "include/vga.h"

/* ---------------------------------------------------------------
 * Storage
 * --------------------------------------------------------------- */
#define MAX_FRAMES   (1024u * 1024u)   /* 4 GB / 4 KB = 1 M frames */

static uint8_t  bitmap[MAX_FRAMES / 8];  /* 128 KB in BSS           */
static uint64_t total_frames = 0;
static uint64_t free_frames  = 0;

/* ---------------------------------------------------------------
 * Bit helpers  (inline so the compiler can optimise them away)
 * --------------------------------------------------------------- */
static inline void  bm_set  (uint64_t f) { bitmap[f >> 3] |=  (uint8_t)(1u << (f & 7)); }
static inline void  bm_clear(uint64_t f) { bitmap[f >> 3] &= (uint8_t)~(1u << (f & 7)); }
static inline int   bm_test (uint64_t f) { return (bitmap[f >> 3] >> (f & 7)) & 1; }

/* ---------------------------------------------------------------
 * pmm_init
 *
 * mmap_addr : virtual address of the Multiboot2 memory-map tag
 *             (the first entry begins at mmap_addr + 16, after
 *              the tag header:  type[4] size[4] entry_size[4]
 *              entry_version[4]).
 * mmap_len  : tag->size field (entire tag, header included).
 * ---------------------------------------------------------------
 *
 * Multiboot2 memory-map tag layout (spec §3.6.8):
 *   uint32_t  type          = 6
 *   uint32_t  size          (total tag size in bytes)
 *   uint32_t  entry_size    (size of each entry, >= 24)
 *   uint32_t  entry_version (currently 0)
 *   entry[0 .. n]
 *
 * Each entry:
 *   uint64_t  base_addr
 *   uint64_t  length
 *   uint32_t  type          (1 = available)
 *   uint32_t  reserved
 * --------------------------------------------------------------- */
void pmm_init(uint64_t mmap_tag_addr,
              uint32_t mmap_tag_size,
              uint64_t kernel_start,
              uint64_t kernel_end)
{
    /* 1. Mark everything USED (safe default) */
    for (uint32_t i = 0; i < MAX_FRAMES / 8; i++)
        bitmap[i] = 0xFF;
    free_frames  = 0;
    total_frames = 0;

    /* 2. Parse Multiboot2 mmap tag --------------------------------
     * Tag header is 16 bytes: type(4) + size(4) + entry_size(4) +
     * entry_version(4).  Entries start immediately after.         */
    uint32_t  entry_size    = *(uint32_t *)(mmap_tag_addr + 8);
    uint32_t  tag_data_size = mmap_tag_size - 16u;  /* bytes of entries */
    uint64_t  entry_base    = mmap_tag_addr + 16u;  /* first entry addr  */

    for (uint32_t off = 0; off < tag_data_size; off += entry_size) {
        uint64_t base   = *(uint64_t *)(entry_base + off);
        uint64_t length = *(uint64_t *)(entry_base + off + 8);
        uint32_t type   = *(uint32_t *)(entry_base + off + 16);

        if (type != 1) continue;   /* skip reserved / ACPI / NVS */

        /* Round base UP to page boundary, end DOWN */
        uint64_t frame_start = (base + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t frame_end   = (base + length)        / PAGE_SIZE;

        if (frame_end > MAX_FRAMES) frame_end = MAX_FRAMES;
        if (frame_start >= frame_end) continue;

        for (uint64_t f = frame_start; f < frame_end; f++) {
            bm_clear(f);
            free_frames++;
        }
        if (frame_end > total_frames) total_frames = frame_end;
    }

    /* 3. Re-mark low 1 MB as USED (BIOS data, VGA, bootloader) */
    pmm_mark_used(0, 256);           /* 256 * 4 KB = 1 MB */

    /* 4. Re-mark kernel image pages as USED */
    uint64_t ks = kernel_start / PAGE_SIZE;
    uint64_t ke = (kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;
    pmm_mark_used(ks * PAGE_SIZE, (size_t)(ke - ks));

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  [ OK ] PMM init — ");
    vga_putdec(free_frames * 4 / 1024);
    vga_puts(" MB free (");
    vga_putdec(free_frames);
    vga_puts(" frames)\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ---------------------------------------------------------------
 * pmm_mark_used / pmm_mark_free
 * --------------------------------------------------------------- */
void pmm_mark_used(uint64_t phys_addr, size_t page_count)
{
    uint64_t frame = phys_addr / PAGE_SIZE;
    for (size_t i = 0; i < page_count; i++) {
        uint64_t f = frame + i;
        if (f >= MAX_FRAMES) break;
        if (!bm_test(f)) { bm_set(f); free_frames--; }
    }
}

void pmm_mark_free(uint64_t phys_addr, size_t page_count)
{
    uint64_t frame = phys_addr / PAGE_SIZE;
    for (size_t i = 0; i < page_count; i++) {
        uint64_t f = frame + i;
        if (f >= MAX_FRAMES) break;
        if (bm_test(f)) { bm_clear(f); free_frames++; }
    }
}

/* ---------------------------------------------------------------
 * pmm_alloc_page
 * Returns physical address of a free 4 KB frame, or
 * PMM_ALLOC_FAIL (0xFFFF...FFFF) on out-of-memory.
 *
 * Note: physical address 0 is always marked USED (low 1 MB),
 * so 0 can never be returned as a valid allocation.
 * --------------------------------------------------------------- */
uint64_t pmm_alloc_page(void)
{
    for (uint64_t i = 1; i < total_frames; i++) {  /* skip frame 0 */
        if (!bm_test(i)) {
            bm_set(i);
            free_frames--;
            return i * PAGE_SIZE;
        }
    }
    return PMM_ALLOC_FAIL;
}

void pmm_free_page(uint64_t phys_addr)
{
    if (phys_addr == PMM_ALLOC_FAIL) return;
    uint64_t frame = phys_addr / PAGE_SIZE;
    if (frame >= total_frames) return;
    if (bm_test(frame)) { bm_clear(frame); free_frames++; }
}

/* ---------------------------------------------------------------
 * pmm_alloc_contiguous  — allocate N physically contiguous pages
 * Used by the VMM and DMA-capable drivers.
 * --------------------------------------------------------------- */
uint64_t pmm_alloc_contiguous(size_t page_count)
{
    if (!page_count) return PMM_ALLOC_FAIL;

    uint64_t run_start = 0;
    size_t   run_len   = 0;

    for (uint64_t i = 1; i < total_frames; i++) {
        if (!bm_test(i)) {
            if (run_len == 0) run_start = i;
            if (++run_len == page_count) {
                for (size_t j = 0; j < page_count; j++)
                    bm_set(run_start + j);
                free_frames -= page_count;
                return run_start * PAGE_SIZE;
            }
        } else {
            run_len = 0;
        }
    }
    return PMM_ALLOC_FAIL;
}

/* ---------------------------------------------------------------
 * Accessors / diagnostics
 * --------------------------------------------------------------- */
uint64_t pmm_get_total_pages(void) { return total_frames; }
uint64_t pmm_get_free_pages (void) { return free_frames;  }

void pmm_dump_stats(void)
{
    vga_puts("PMM: total=");
    vga_putdec(total_frames);
    vga_puts(" free=");
    vga_putdec(free_frames);
    vga_puts(" used=");
    vga_putdec(total_frames - free_frames);
    vga_puts("\n");
}

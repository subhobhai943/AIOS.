#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

/* Physical Memory Manager — Bitmap Allocator
 * Bit = 1 → frame USED;  Bit = 0 → frame FREE.
 *
 * Supports up to 4 GB RAM (1 M frames, 128 KB static bitmap).
 */

#define PAGE_SIZE       4096u
#define PAGES_PER_BYTE  8u

/* Sentinel returned by pmm_alloc_page() / pmm_alloc_contiguous()
 * on out-of-memory.  Never a valid physical address.             */
#define PMM_ALLOC_FAIL  (~0ULL)

/*
 * pmm_init — parse Multiboot2 memory-map tag, build bitmap.
 *
 * mmap_tag_addr : virtual address of the MB2 type-6 tag
 * mmap_tag_size : tag->size field (whole tag, header included)
 * kernel_start  : physical start of kernel image (_kernel_start)
 * kernel_end    : physical end   of kernel image (_kernel_end)
 */
void     pmm_init(uint64_t mmap_tag_addr, uint32_t mmap_tag_size,
                  uint64_t kernel_start,  uint64_t kernel_end);

/* Mark a range of pages as explicitly used or free.
 * phys_addr must be page-aligned; page_count is the number of 4 KB pages. */
void     pmm_mark_used(uint64_t phys_addr, size_t page_count);
void     pmm_mark_free(uint64_t phys_addr, size_t page_count);

/* Single-page allocator.  Returns physical address or PMM_ALLOC_FAIL. */
uint64_t pmm_alloc_page(void);
void     pmm_free_page(uint64_t phys_addr);

/* Contiguous allocator — needed for DMA buffers and LLM weight loading. */
uint64_t pmm_alloc_contiguous(size_t page_count);

/* Diagnostics */
uint64_t pmm_get_total_pages(void);
uint64_t pmm_get_free_pages(void);
void     pmm_dump_stats(void);

#endif /* PMM_H */

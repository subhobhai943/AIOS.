#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

/* Physical Memory Manager — Bitmap Allocator
 * Bit = 1 → frame USED;  Bit = 0 → frame FREE.
 */

#define PAGE_SIZE       4096u
#define PAGES_PER_BYTE  8u

/* Sentinel returned by pmm_alloc_page() / pmm_alloc_contiguous()
 * on out-of-memory.  Never a valid physical address.             */
#define PMM_ALLOC_FAIL  (~0ULL)

void     pmm_init(uint64_t mmap_tag_addr, uint32_t mmap_tag_size,
                  uint64_t kernel_start,  uint64_t kernel_end);
void     pmm_mark_used(uint64_t phys_addr, size_t page_count);
void     pmm_mark_free(uint64_t phys_addr, size_t page_count);
uint64_t pmm_alloc_page(void);
void     pmm_free_page(uint64_t phys_addr);
uint64_t pmm_alloc_contiguous(size_t page_count);
uint64_t pmm_get_total_pages(void);
uint64_t pmm_get_free_pages(void);
void     pmm_dump_stats(void);

#endif /* PMM_H */

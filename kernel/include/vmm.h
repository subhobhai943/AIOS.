#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

/* Virtual Memory Manager — 4-level paging (x86-64)
 *
 * Virtual address breakdown (48-bit canonical):
 *   [47:39] PML4 index   (9 bits)
 *   [38:30] PDPT  index  (9 bits)
 *   [29:21] PD    index  (9 bits)
 *   [20:12] PT    index  (9 bits)
 *   [11:0 ] Page  offset (12 bits)
 *
 * Early boot: first 64 MB is identity-mapped (virt == phys).
 * Higher-half kernel map at 0xFFFFFFFF80000000 is set up in Phase 4.
 */

/* PTE flag bits */
#define PAGE_PRESENT    (1ULL << 0)
#define PAGE_WRITE      (1ULL << 1)
#define PAGE_USER       (1ULL << 2)
#define PAGE_HUGE       (1ULL << 7)   /* 2 MB pages when set in a PD entry */
#define PAGE_NX         (1ULL << 63)  /* No-execute (needs EFER.NXE) */

/* Future higher-half offset (kernel linked at this virtual base) */
#define VIRT_TO_PHYS_OFFSET  0xFFFFFFFF80000000ULL

typedef uint64_t page_entry_t;

/*
 * vmm_init — allocate PML4, identity-map first 64 MB, load CR3.
 * Must be called AFTER pmm_init() so the PMM bitmap is ready.
 */
void     vmm_init(void);

/*
 * Map / unmap individual pages.
 * flags should be a combination of PAGE_PRESENT, PAGE_WRITE,
 * PAGE_USER, PAGE_NX etc.  PAGE_PRESENT is always OR-ed in by
 * vmm_map_page() regardless of the flags argument.
 */
void     vmm_map_page   (uint64_t virt, uint64_t phys, uint64_t flags);
void     vmm_unmap_page (uint64_t virt);

/* Walk the page tables and return the physical address that backs
 * `virt`, or PMM_ALLOC_FAIL if any level is not present. */
uint64_t vmm_virt_to_phys(uint64_t virt);

/* Map a contiguous range of physical pages to a contiguous virtual range. */
void     vmm_map_range(uint64_t virt_start, uint64_t phys_start,
                       size_t page_count, uint64_t flags);

/* Switch CR3 to a different address space. */
void     vmm_switch_directory(uint64_t pml4_phys);
uint64_t vmm_get_current_pml4(void);

#endif /* VMM_H */

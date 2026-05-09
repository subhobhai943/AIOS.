/* ============================================================
 * AIOS — Virtual Memory Manager (VMM)
 * 4-level paging: PML4 → PDPT → PD → PT
 *
 * Key design decisions
 * --------------------
 * 1. IDENTITY MAP the first 64 MB at boot.
 *    pmm_alloc_page() returns physical addresses inside that
 *    window, so page-table pages are always directly addressable
 *    as virtual pointers before a higher-half mapping is set up.
 *
 * 2. PHYS_TO_VIRT / VIRT_TO_PHYS macros.
 *    Once paging is live, every pointer dereference of a page-
 *    table physical address goes through PHYS_TO_VIRT().
 *    In the early identity-mapped phase phys == virt, so the
 *    macro is a no-op — but the call sites are future-proof for
 *    when we move to a proper higher-half kernel map.
 *
 * 3. Safe guard on all walk helpers.
 *    vmm_virt_to_phys() and vmm_unmap_page() check PAGE_PRESENT
 *    at every level so they never dereference a zero/garbage PTE.
 * ============================================================ */

#include <stdint.h>
#include <stddef.h>
#include "include/vmm.h"
#include "include/pmm.h"
#include "include/vga.h"

/* ---------------------------------------------------------------
 * In the early boot identity-mapped region phys == virt.
 * Replace these macros when moving to a higher-half layout.
 * --------------------------------------------------------------- */
#define PHYS_TO_VIRT(p)  ((void *)(uintptr_t)(p))
#define VIRT_TO_PHYS(v)  ((uint64_t)(uintptr_t)(v))

/* ---------------------------------------------------------------
 * Page-index extractors
 * --------------------------------------------------------------- */
#define PML4_IDX(v)  (((uint64_t)(v) >> 39) & 0x1FFu)
#define PDPT_IDX(v)  (((uint64_t)(v) >> 30) & 0x1FFu)
#define PD_IDX(v)    (((uint64_t)(v) >> 21) & 0x1FFu)
#define PT_IDX(v)    (((uint64_t)(v) >> 12) & 0x1FFu)

#define ENTRIES_PER_TABLE  512u
#define PHYS_ADDR_MASK     (~0xFFFULL)

/* ---------------------------------------------------------------
 * State
 * --------------------------------------------------------------- */
static uint64_t current_pml4_phys = 0;

/* ---------------------------------------------------------------
 * alloc_table — allocate and zero one 4 KB page-table page.
 *
 * Critical: we obtain a PHYSICAL address from the PMM, then
 * convert it to a virtual pointer via PHYS_TO_VIRT() for
 * zeroing.  In the identity-mapped phase these are equal.
 * --------------------------------------------------------------- */
static page_entry_t *alloc_table(void)
{
    uint64_t phys = pmm_alloc_page();
    if (phys == PMM_ALLOC_FAIL) return (page_entry_t *)0;  /* OOM */

    page_entry_t *tbl = (page_entry_t *)PHYS_TO_VIRT(phys);
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; i++)
        tbl[i] = 0;
    return tbl;
}

/* ---------------------------------------------------------------
 * get_or_create — return (virtual) pointer to child table,
 * allocating it if the entry is not yet present.
 *
 * flags is ORed into the new entry (in addition to PRESENT|WRITE).
 * Kernel-only tables must NOT have PAGE_USER set.
 * --------------------------------------------------------------- */
static page_entry_t *get_or_create(page_entry_t *table, uint32_t idx,
                                   uint64_t flags)
{
    if (!(table[idx] & PAGE_PRESENT)) {
        page_entry_t *child = alloc_table();
        if (!child) return (page_entry_t *)0;
        table[idx] = VIRT_TO_PHYS(child) | flags | PAGE_PRESENT | PAGE_WRITE;
    }
    uint64_t child_phys = table[idx] & PHYS_ADDR_MASK;
    return (page_entry_t *)PHYS_TO_VIRT(child_phys);
}

/* ---------------------------------------------------------------
 * vmm_map_page — map one virtual page → physical frame.
 * --------------------------------------------------------------- */
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    page_entry_t *pml4 = (page_entry_t *)PHYS_TO_VIRT(current_pml4_phys);
    page_entry_t *pdpt = get_or_create(pml4, PML4_IDX(virt), 0);
    if (!pdpt) return;
    page_entry_t *pd   = get_or_create(pdpt, PDPT_IDX(virt), 0);
    if (!pd)   return;
    page_entry_t *pt   = get_or_create(pd,   PD_IDX(virt),   0);
    if (!pt)   return;

    pt[PT_IDX(virt)] = (phys & PHYS_ADDR_MASK) | flags | PAGE_PRESENT;

    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

/* ---------------------------------------------------------------
 * vmm_unmap_page — zero the PTE and flush TLB.
 * Guards every level with a PAGE_PRESENT check.
 * --------------------------------------------------------------- */
void vmm_unmap_page(uint64_t virt)
{
    page_entry_t *pml4 = (page_entry_t *)PHYS_TO_VIRT(current_pml4_phys);

    if (!(pml4[PML4_IDX(virt)] & PAGE_PRESENT)) return;
    page_entry_t *pdpt = (page_entry_t *)PHYS_TO_VIRT(
                          pml4[PML4_IDX(virt)] & PHYS_ADDR_MASK);

    if (!(pdpt[PDPT_IDX(virt)] & PAGE_PRESENT)) return;
    page_entry_t *pd   = (page_entry_t *)PHYS_TO_VIRT(
                          pdpt[PDPT_IDX(virt)] & PHYS_ADDR_MASK);

    if (!(pd[PD_IDX(virt)] & PAGE_PRESENT)) return;
    page_entry_t *pt   = (page_entry_t *)PHYS_TO_VIRT(
                          pd[PD_IDX(virt)] & PHYS_ADDR_MASK);

    pt[PT_IDX(virt)] = 0;
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

/* ---------------------------------------------------------------
 * vmm_virt_to_phys — walk page tables safely; return
 * PMM_ALLOC_FAIL if any level is not present.
 * --------------------------------------------------------------- */
uint64_t vmm_virt_to_phys(uint64_t virt)
{
    page_entry_t *pml4 = (page_entry_t *)PHYS_TO_VIRT(current_pml4_phys);
    if (!(pml4[PML4_IDX(virt)] & PAGE_PRESENT)) return PMM_ALLOC_FAIL;
    page_entry_t *pdpt = (page_entry_t *)PHYS_TO_VIRT(
                          pml4[PML4_IDX(virt)] & PHYS_ADDR_MASK);

    if (!(pdpt[PDPT_IDX(virt)] & PAGE_PRESENT)) return PMM_ALLOC_FAIL;
    page_entry_t *pd   = (page_entry_t *)PHYS_TO_VIRT(
                          pdpt[PDPT_IDX(virt)] & PHYS_ADDR_MASK);

    if (!(pd[PD_IDX(virt)] & PAGE_PRESENT)) return PMM_ALLOC_FAIL;
    page_entry_t *pt   = (page_entry_t *)PHYS_TO_VIRT(
                          pd[PD_IDX(virt)] & PHYS_ADDR_MASK);

    if (!(pt[PT_IDX(virt)] & PAGE_PRESENT)) return PMM_ALLOC_FAIL;
    return (pt[PT_IDX(virt)] & PHYS_ADDR_MASK) | (virt & 0xFFF);
}

/* ---------------------------------------------------------------
 * vmm_map_range — map [virt, virt + count*PAGE) → [phys, ...]
 * --------------------------------------------------------------- */
void vmm_map_range(uint64_t virt_start, uint64_t phys_start,
                   size_t count, uint64_t flags)
{
    for (size_t i = 0; i < count; i++) {
        vmm_map_page(virt_start + (uint64_t)i * PAGE_SIZE,
                     phys_start + (uint64_t)i * PAGE_SIZE,
                     flags);
    }
}

/* ---------------------------------------------------------------
 * vmm_switch_directory — load CR3.
 * --------------------------------------------------------------- */
void vmm_switch_directory(uint64_t pml4_phys)
{
    current_pml4_phys = pml4_phys;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
}

uint64_t vmm_get_current_pml4(void) { return current_pml4_phys; }

/* ---------------------------------------------------------------
 * vmm_init — set up identity map for the first IDENTITY_MAP_MB MB.
 *
 * Why 64 MB?
 *   pmm_alloc_page() scans from frame 1 upward; page-table pages
 *   could land anywhere in available RAM.  64 MB gives plenty of
 *   headroom for the early kernel + initial heap before we set up
 *   a proper higher-half map.
 * --------------------------------------------------------------- */
#define IDENTITY_MAP_MB   64u
#define IDENTITY_MAP_PAGES  ((IDENTITY_MAP_MB * 1024u * 1024u) / PAGE_SIZE)

void vmm_init(void)
{
    page_entry_t *pml4 = alloc_table();
    if (!pml4) {
        vga_puts("[FAIL] VMM: out of memory for PML4\n");
        for (;;) __asm__ volatile ("hlt");
    }
    current_pml4_phys = VIRT_TO_PHYS(pml4);

    /* Identity-map the first 64 MB: virt 0 → phys 0 */
    vmm_map_range(0, 0, IDENTITY_MAP_PAGES, PAGE_PRESENT | PAGE_WRITE);

    vmm_switch_directory(current_pml4_phys);

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  [ OK ] VMM init — identity mapped first ");
    vga_putdec(IDENTITY_MAP_MB);
    vga_puts(" MB\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

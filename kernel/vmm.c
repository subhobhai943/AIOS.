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
 *    In the early identity-mapped phase phys == virt, so these
 *    are no-ops — but call sites are future-proof.
 *
 * 3. Safe guard on all walk helpers.
 *    vmm_virt_to_phys() and vmm_unmap_page() check PAGE_PRESENT
 *    at every level.
 *
 * 4. STATIC BUMP FALLBACK.
 *    vmm_init() may be called before pmm_init() (no MB2 mmap).
 *    A 640 KB BSS pool covers the ~19 PT pages needed for the
 *    64 MB identity map with large headroom.
 *
 * 5. vmm_map_mmio().
 *    Maps MMIO regions (LAPIC 0xFEE00000, IOAPIC 0xFEC00000,
 *    PCI BARs …) with PAGE_NOCACHE before any device access.
 *    The identity map only covers 0–64 MB; MMIO is at 4 GB-range
 *    addresses and MUST be explicitly mapped or a #PF fires
 *    on the very first register read/write.
 * ============================================================ */

#include <stdint.h>
#include <stddef.h>
#include "include/vmm.h"
#include "include/pmm.h"
#include "include/vga.h"

#define PHYS_TO_VIRT(p)  ((void *)(uintptr_t)(p))
#define VIRT_TO_PHYS(v)  ((uint64_t)(uintptr_t)(v))

#define PML4_IDX(v)  (((uint64_t)(v) >> 39) & 0x1FFu)
#define PDPT_IDX(v)  (((uint64_t)(v) >> 30) & 0x1FFu)
#define PD_IDX(v)    (((uint64_t)(v) >> 21) & 0x1FFu)
#define PT_IDX(v)    (((uint64_t)(v) >> 12) & 0x1FFu)

#define ENTRIES_PER_TABLE  512u
#define PHYS_ADDR_MASK     (~0xFFFULL)

/* ---------------------------------------------------------------
 * Static page-table page pool (640 KB, BSS = zeroed at boot)
 * 160 pages → covers 64 MB identity map (19 pages) + large headroom
 * for MMIO mappings added later (LAPIC, IOAPIC, …).
 * Used ONLY when pmm_alloc_page() returns PMM_ALLOC_FAIL.
 * --------------------------------------------------------------- */
#define STATIC_PT_PAGES  160u

static uint8_t static_pt_pool[STATIC_PT_PAGES * 4096]
    __attribute__((aligned(4096)));
static uint32_t static_pt_used = 0;

static page_entry_t *alloc_from_static_pool(void)
{
    if (static_pt_used >= STATIC_PT_PAGES) {
        vga_puts_color(
            "[FAIL] VMM: static PT pool exhausted\n",
            VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK
        );
        for (;;) __asm__ volatile ("hlt");
    }
    page_entry_t *p =
        (page_entry_t *)(static_pt_pool + static_pt_used * 4096u);
    static_pt_used++;
    return p;   /* BSS → already zero, no memset needed */
}

/* ---------------------------------------------------------------
 * State
 * --------------------------------------------------------------- */
static uint64_t current_pml4_phys = 0;

/* ---------------------------------------------------------------
 * alloc_table — try PMM first, fall back to static pool.
 * --------------------------------------------------------------- */
static page_entry_t *alloc_table(void)
{
    uint64_t phys = pmm_alloc_page();
    if (phys != PMM_ALLOC_FAIL) {
        page_entry_t *tbl = (page_entry_t *)PHYS_TO_VIRT(phys);
        for (uint32_t i = 0; i < ENTRIES_PER_TABLE; i++)
            tbl[i] = 0;
        return tbl;
    }
    return alloc_from_static_pool();
}

/* ---------------------------------------------------------------
 * get_or_create — return child table pointer, creating if absent.
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
 * vmm_map_page
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
 * vmm_unmap_page
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
 * vmm_virt_to_phys
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
 * vmm_map_range — map `count` pages virt_start → phys_start.
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
 * vmm_map_mmio — identity-map an MMIO region with cache-disable.
 *
 * MMIO addresses (LAPIC @ 0xFEE00000, IOAPIC @ 0xFEC00000, etc.)
 * live far above the 64 MB identity window.  They MUST be mapped
 * before any driver reads or writes a device register, otherwise
 * the first access triggers a #PF (error code 0x2 = write to a
 * non-present page).
 *
 * PAGE_NOCACHE (PCD, bit 4) ensures the CPU never serves a read
 * from its cache and never coalesces writes — both critical for
 * memory-mapped device registers.
 * --------------------------------------------------------------- */
void vmm_map_mmio(uint64_t phys_base, size_t page_count)
{
    vmm_map_range(
        phys_base, phys_base, page_count,
        PAGE_PRESENT | PAGE_WRITE | PAGE_NOCACHE
    );
}

/* ---------------------------------------------------------------
 * vmm_switch_directory
 * --------------------------------------------------------------- */
void vmm_switch_directory(uint64_t pml4_phys)
{
    current_pml4_phys = pml4_phys;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
}

uint64_t vmm_get_current_pml4(void) { return current_pml4_phys; }

/* ---------------------------------------------------------------
 * vmm_init — identity-map first 64 MB and install CR3.
 *
 * MMIO regions (LAPIC, IOAPIC) are NOT mapped here — each driver
 * calls vmm_map_mmio() before its first register access.
 * --------------------------------------------------------------- */
#define IDENTITY_MAP_MB     64u
#define IDENTITY_MAP_PAGES  ((IDENTITY_MAP_MB * 1024u * 1024u) / PAGE_SIZE)

void vmm_init(void)
{
    page_entry_t *pml4 = alloc_table();
    current_pml4_phys  = VIRT_TO_PHYS(pml4);

    vmm_map_range(0, 0, IDENTITY_MAP_PAGES, PAGE_PRESENT | PAGE_WRITE);

    vmm_switch_directory(current_pml4_phys);

    vga_puts_color(
        "  [ OK ] VMM: identity mapped first 64 MB (static pool used: ",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK
    );
    vga_putdec(static_pt_used);
    vga_puts_color(" pages)\n", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
}

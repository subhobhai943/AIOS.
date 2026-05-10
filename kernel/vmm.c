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
 *
 * 4. STATIC BUMP FALLBACK (added May 2026).
 *    vmm_init() is called BEFORE pmm_init() when Multiboot2 is
 *    not available (no mmap tag).  In that case pmm_alloc_page()
 *    returns PMM_ALLOC_FAIL immediately because total_frames==0.
 *    We now provide a small static arena of pre-zeroed 4 KB pages
 *    (STATIC_PT_PAGES × 4 KB = enough for PML4 + the ~130 page-
 *    table pages needed to cover 64 MB).  The static arena is
 *    used ONLY when the PMM has no free pages to give.
 * ============================================================ */

#include <stdint.h>
#include <stddef.h>
#include "include/vmm.h"
#include "include/pmm.h"
#include "include/vga.h"

/* ---------------------------------------------------------------
 * In the early boot identity-mapped region phys == virt.
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
 * Static page-table page pool
 *
 * Identity-mapping 64 MB requires:
 *   1 PML4 + 1 PDPT + 1 PD + 16 PT pages  = 19 pages minimum.
 *   We allocate 160 pages (640 KB) to give comfortable headroom
 *   (allows up to ~320 MB identity mapping plus overhead).
 *
 * This pool lives in BSS (zeroed by the bootloader), so each
 * page is already clean — no explicit zeroing needed.
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
    /* BSS is zero-initialised, so no memset needed */
    page_entry_t *p =
        (page_entry_t *)(static_pt_pool + static_pt_used * 4096u);
    static_pt_used++;
    return p;
}

/* ---------------------------------------------------------------
 * State
 * --------------------------------------------------------------- */
static uint64_t current_pml4_phys = 0;

/* ---------------------------------------------------------------
 * alloc_table — allocate one 4 KB page-table page.
 *
 * Priority:
 *   1. Try PMM (normal path — after pmm_init() with MB2 mmap).
 *   2. Fall back to static pool (no PMM / early boot).
 * --------------------------------------------------------------- */
static page_entry_t *alloc_table(void)
{
    uint64_t phys = pmm_alloc_page();
    if (phys != PMM_ALLOC_FAIL) {
        /* PMM page: zero it (PMM doesn't guarantee zeroed pages) */
        page_entry_t *tbl = (page_entry_t *)PHYS_TO_VIRT(phys);
        for (uint32_t i = 0; i < ENTRIES_PER_TABLE; i++)
            tbl[i] = 0;
        return tbl;
    }
    /* PMM unavailable — use static pool (BSS, already zero) */
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
 * vmm_map_range
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
 * vmm_switch_directory
 * --------------------------------------------------------------- */
void vmm_switch_directory(uint64_t pml4_phys)
{
    current_pml4_phys = pml4_phys;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
}

uint64_t vmm_get_current_pml4(void) { return current_pml4_phys; }

/* ---------------------------------------------------------------
 * vmm_init — set up identity map for the first 64 MB.
 *
 * Called BEFORE pmm_init() when no MB2 mmap is available.
 * In that case alloc_table() automatically falls back to the
 * static BSS pool — no PMM needed for the initial page tables.
 *
 * After pmm_init() runs (if MB2 mmap is present), subsequent
 * vmm_map_page() calls use the PMM normally.
 * --------------------------------------------------------------- */
#define IDENTITY_MAP_MB     64u
#define IDENTITY_MAP_PAGES  ((IDENTITY_MAP_MB * 1024u * 1024u) / PAGE_SIZE)

void vmm_init(void)
{
    /* Allocate PML4 — uses static pool if PMM not yet ready */
    page_entry_t *pml4 = alloc_table();
    /* alloc_from_static_pool() halts on exhaustion, so pml4 != NULL */
    current_pml4_phys = VIRT_TO_PHYS(pml4);

    /* Identity-map 0 → 64 MB */
    vmm_map_range(0, 0, IDENTITY_MAP_PAGES, PAGE_PRESENT | PAGE_WRITE);

    vmm_switch_directory(current_pml4_phys);

    vga_puts_color(
        "  [ OK ] VMM: identity mapped first 64 MB (static pool used: ",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK
    );
    vga_putdec(static_pt_used);
    vga_puts_color(" pages)\n", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
}

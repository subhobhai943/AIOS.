#include "vmm.h"
#include "pmm.h"
#include "vga.h"
#include <stddef.h>
#include <stdint.h>

/* ─── Page table helpers ──────────────────────────── */
#define ENTRIES_PER_TABLE   512
#define PML4_IDX(v)   (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v)   (((v) >> 30) & 0x1FF)
#define PD_IDX(v)     (((v) >> 21) & 0x1FF)
#define PT_IDX(v)     (((v) >> 12) & 0x1FF)
#define ALIGN_PAGE(x) (((x) + 0xFFF) & ~0xFFFULL)

static uint64_t current_pml4_phys = 0;

/* ─── table allocation ────────────────────────────── */
static page_entry_t *alloc_table(void)
{
    uint64_t phys = pmm_alloc_page();
    page_entry_t *tbl = (page_entry_t *)phys;

    for (int i = 0; i < ENTRIES_PER_TABLE; i++)
        tbl[i] = 0;

    return tbl;
}

static page_entry_t *get_or_create(page_entry_t *table, int idx, uint64_t flags)
{
    if (!(table[idx] & PAGE_PRESENT)) {
        page_entry_t *child = alloc_table();
        table[idx] = ((uint64_t)child) | flags | PAGE_PRESENT | PAGE_WRITE;
    }
    return (page_entry_t *)(table[idx] & ~0xFFFULL);
}

/* ─── init ────────────────────────────────────────── */
void vmm_init(void)
{
    page_entry_t *pml4 = alloc_table();
    current_pml4_phys = (uint64_t)pml4;

    /* identity map first 4MB */
    vmm_map_range(0, 0, 1024, PAGE_PRESENT | PAGE_WRITE);

    vmm_switch_directory(current_pml4_phys);

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  [ OK ] VMM initialised\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ─── mapping ─────────────────────────────────────── */
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    page_entry_t *pml4 = (page_entry_t *)current_pml4_phys;
    page_entry_t *pdpt = get_or_create(pml4, PML4_IDX(virt), PAGE_USER);
    page_entry_t *pd   = get_or_create(pdpt, PDPT_IDX(virt), PAGE_USER);
    page_entry_t *pt   = get_or_create(pd,   PD_IDX(virt),   PAGE_USER);

    pt[PT_IDX(virt)] = (phys & ~0xFFFULL) | flags | PAGE_PRESENT;

    /* flush TLB */
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

void vmm_unmap_page(uint64_t virt)
{
    page_entry_t *pml4 = (page_entry_t *)current_pml4_phys;
    page_entry_t *pdpt = (page_entry_t *)(pml4[PML4_IDX(virt)] & ~0xFFFULL);
    page_entry_t *pd   = (page_entry_t *)(pdpt[PDPT_IDX(virt)] & ~0xFFFULL);
    page_entry_t *pt   = (page_entry_t *)(pd[PD_IDX(virt)]     & ~0xFFFULL);

    pt[PT_IDX(virt)] = 0;

    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

/* ─── translation ─────────────────────────────────── */
uint64_t vmm_virt_to_phys(uint64_t virt)
{
    page_entry_t *pml4 = (page_entry_t *)current_pml4_phys;
    page_entry_t *pdpt = (page_entry_t *)(pml4[PML4_IDX(virt)] & ~0xFFFULL);
    page_entry_t *pd   = (page_entry_t *)(pdpt[PDPT_IDX(virt)] & ~0xFFFULL);
    page_entry_t *pt   = (page_entry_t *)(pd[PD_IDX(virt)]     & ~0xFFFULL);

    return (pt[PT_IDX(virt)] & ~0xFFFULL) | (virt & 0xFFF);
}

/* ─── bulk mapping ────────────────────────────────── */
void vmm_map_range(uint64_t virt_start, uint64_t phys_start, size_t count, uint64_t flags)
{
    for (size_t i = 0; i < count; i++) {
        vmm_map_page(
            virt_start + i * PAGE_SIZE,
            phys_start + i * PAGE_SIZE,
            flags
        );
    }
}

/* ─── CR3 switch (FIXED) ──────────────────────────── */
void vmm_switch_directory(uint64_t pml4_phys)
{
    current_pml4_phys = pml4_phys;

    __asm__ volatile(
        "mov %0, %%cr3"
        :
        : "r"(pml4_phys)
        : "memory"
    );
}

/* ─── getter ─────────────────────────────────────── */
uint64_t vmm_get_current_pml4(void)
{
    return current_pml4_phys;
}

/* ============================================================
 * AIOS — Global Descriptor Table (GDT)
 * ============================================================ */

#include "include/gdt.h"
#include <stdint.h>

/* ───────────────────────────────────────────── */
/* GDT Entry                                     */
/* ───────────────────────────────────────────── */

typedef struct __attribute__((packed))
{
    uint16_t limit_low;
    uint16_t base_low;

    uint8_t  base_mid;
    uint8_t  access;

    uint8_t  flags_limit_hi;
    uint8_t  base_high;

} gdt_entry_t;

/* ───────────────────────────────────────────── */
/* GDTR                                          */
/* ───────────────────────────────────────────── */

typedef struct __attribute__((packed))
{
    uint16_t limit;
    uint64_t base;

} gdt_ptr_t;

/* ───────────────────────────────────────────── */
/* Access flags                                  */
/* ───────────────────────────────────────────── */

#define GDT_PRESENT   (1 << 7)
#define GDT_DPL(x)    (((x) & 0x3) << 5)

#define GDT_SYSTEM    (1 << 4)

#define GDT_EXEC      (1 << 3)
#define GDT_DC        (1 << 2)
#define GDT_RW        (1 << 1)
#define GDT_ACCESSED  (1 << 0)

/* ───────────────────────────────────────────── */
/* Flags                                          */
/* ───────────────────────────────────────────── */

#define GDT_GRAN_4K   (1 << 7)
#define GDT_32BIT     (1 << 6)
#define GDT_64BIT     (1 << 5)

/* ───────────────────────────────────────────── */

static gdt_entry_t gdt[GDT_ENTRY_COUNT];
static gdt_ptr_t   gdt_ptr;

/* ───────────────────────────────────────────── */

static void gdt_set_entry(
    int idx,
    uint32_t base,
    uint32_t limit,
    uint8_t access,
    uint8_t flags
)
{
    gdt[idx].base_low  = (base & 0xFFFF);
    gdt[idx].base_mid  = (base >> 16) & 0xFF;
    gdt[idx].base_high = (base >> 24) & 0xFF;

    gdt[idx].limit_low = (limit & 0xFFFF);

    gdt[idx].flags_limit_hi =
        ((limit >> 16) & 0x0F) |
        (flags & 0xF0);

    gdt[idx].access = access;
}

/* ───────────────────────────────────────────── */

void gdt_init(void)
{
    /* NULL descriptor */
    gdt_set_entry(
        GDT_NULL_SEG,
        0,
        0,
        0,
        0
    );

    /* Kernel code */
    gdt_set_entry(
        GDT_KERNEL_CODE_SEG,
        0,
        0xFFFFF,
        GDT_PRESENT |
        GDT_DPL(0) |
        GDT_SYSTEM |
        GDT_EXEC |
        GDT_RW,

        GDT_GRAN_4K |
        GDT_64BIT
    );

    /* Kernel data */
    gdt_set_entry(
        GDT_KERNEL_DATA_SEG,
        0,
        0xFFFFF,
        GDT_PRESENT |
        GDT_DPL(0) |
        GDT_SYSTEM |
        GDT_RW,

        GDT_GRAN_4K |
        GDT_32BIT
    );

    /* User code */
    gdt_set_entry(
        GDT_USER_CODE_SEG,
        0,
        0xFFFFF,
        GDT_PRESENT |
        GDT_DPL(3) |
        GDT_SYSTEM |
        GDT_EXEC |
        GDT_RW,

        GDT_GRAN_4K |
        GDT_64BIT
    );

    /* User data */
    gdt_set_entry(
        GDT_USER_DATA_SEG,
        0,
        0xFFFFF,
        GDT_PRESENT |
        GDT_DPL(3) |
        GDT_SYSTEM |
        GDT_RW,

        GDT_GRAN_4K |
        GDT_32BIT
    );

    /* Dummy TSS */
    gdt_set_entry(
        GDT_TSS_SEG,
        0,
        0,
        0,
        0
    );

    /* GDTR */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (uint64_t)&gdt;

    /* Load GDT */
    __asm__ volatile (
        "lgdt %0\n\t"

        "mov $0x10, %%ax\n\t"

        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"

        :
        : "m"(gdt_ptr)
        : "ax", "memory"
    );
}

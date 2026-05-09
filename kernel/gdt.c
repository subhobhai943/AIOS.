/* ============================================================
 * AIOS — Global Descriptor Table (GDT)
 *
 * Phase 0.3 checklist
 *   [x] null descriptor
 *   [x] kernel code  (64-bit, ring 0)
 *   [x] kernel data  (ring 0)
 *   [x] user code    (64-bit, ring 3)
 *   [x] user data    (ring 3)
 *   [x] TSS          (16-byte system segment, type 0x9 = 64-bit available TSS)
 *   [x] gdt_flush()  — lgdt + lretq far-jump to reload CS
 *   [x] tss_load()   — ltr
 * ============================================================ */

#include "include/gdt.h"
#include <stdint.h>

/* ---------------------------------------------------------------
 * Internal types
 * --------------------------------------------------------------- */

/* Standard 8-byte GDT entry (code / data segments) */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;          /* P | DPL[2] | S | Type[4]  */
    uint8_t  flags_limit_hi;  /* G | DB | L | AVL | limit[19:16] */
    uint8_t  base_high;
} gdt_entry_t;

/* 16-byte GDT system descriptor (TSS).
 * We overlay two consecutive gdt_entry_t slots.
 * Lower 8 bytes: same layout as gdt_entry_t.
 * Upper 8 bytes: base[63:32] + reserved.              */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_00_15;
    uint8_t  base_16_23;
    uint8_t  type;        /* P=1 | DPL=0 | 0 | 0x9 (64-bit avail TSS) */
    uint8_t  limit_hi;    /* G=0 | 0 | AVL | limit[19:16]              */
    uint8_t  base_24_31;
    uint32_t base_32_63;
    uint32_t reserved;
} tss_descriptor_t;

/* GDTR */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdt_ptr_t;

/* ---------------------------------------------------------------
 * Access-byte bit helpers
 * --------------------------------------------------------------- */
#define ACCESS_PRESENT  (1u << 7)
#define ACCESS_DPL(x)   (((x) & 0x3u) << 5)
#define ACCESS_S        (1u << 4)   /* 1 = code/data, 0 = system */
#define ACCESS_EXEC     (1u << 3)
#define ACCESS_RW       (1u << 1)

/* Flags nibble (top 4 bits of flags_limit_hi) */
#define FLAG_GRAN_4K    (1u << 7)
#define FLAG_32BIT      (1u << 6)   /* DB bit */
#define FLAG_64BIT      (1u << 5)   /* L  bit */

/* 64-bit available TSS: P=1, DPL=0, S=0, type=9 */
#define TSS_TYPE_AVAIL  (ACCESS_PRESENT | 0x9u)

/* ---------------------------------------------------------------
 * Storage
 * --------------------------------------------------------------- */
static gdt_entry_t gdt[GDT_ENTRY_COUNT];
static gdt_ptr_t   gdt_ptr;

tss_t g_tss;   /* zero-initialised by BSS */

/* ---------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------- */
static void gdt_set_entry(int idx,
                          uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags)
{
    gdt[idx].base_low       = (uint16_t)( base        & 0xFFFF);
    gdt[idx].base_mid       = (uint8_t) ((base >> 16) & 0xFF);
    gdt[idx].base_high      = (uint8_t) ((base >> 24) & 0xFF);
    gdt[idx].limit_low      = (uint16_t)( limit       & 0xFFFF);
    gdt[idx].flags_limit_hi = (uint8_t)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    gdt[idx].access         = access;
}

static void gdt_set_tss(uint64_t base, uint32_t limit)
{
    /* Overlay the two 8-byte slots at GDT_TSS_SEG_LOW / _HIGH
     * with the 16-byte TSS system descriptor.                  */
    tss_descriptor_t *d = (tss_descriptor_t *)&gdt[GDT_TSS_SEG_LOW];

    d->limit_low  = (uint16_t)( limit        & 0xFFFF);
    d->base_00_15 = (uint16_t)( base         & 0xFFFF);
    d->base_16_23 = (uint8_t) ((base >> 16)  & 0xFF);
    d->type       = TSS_TYPE_AVAIL;
    d->limit_hi   = (uint8_t) ((limit >> 16) & 0x0F);
    d->base_24_31 = (uint8_t) ((base >> 24)  & 0xFF);
    d->base_32_63 = (uint32_t)((base >> 32)  & 0xFFFFFFFF);
    d->reserved   = 0;
}

/* ---------------------------------------------------------------
 * gdt_flush — reload GDTR and far-return to flush CS cache
 *
 * Root cause of the build error:
 *   A named label like ".flush_cs" inside asm volatile is a real
 *   assembler symbol.  With -O2, GCC may clone or outline the
 *   function, emitting the same label twice → "already defined".
 *
 * Fix: numeric local label "1:".  The GNU assembler treats these
 * as purely local; "1f" (forward) and "1b" (backward) references
 * work within the same asm block and are never globally unique,
 * so cloning is safe.  __attribute__((noinline)) is belt-and-
 * suspenders: prevents inlining so the asm block is emitted once.
 * --------------------------------------------------------------- */
void __attribute__((noinline)) gdt_flush(void)
{
    __asm__ volatile (
        /* 1. Load new GDTR */
        "lgdt %[ptr]            \n\t"

        /* 2. Reload data-segment registers (kernel DS = 0x10) */
        "mov  $0x10, %%ax       \n\t"
        "mov  %%ax,  %%ds       \n\t"
        "mov  %%ax,  %%es       \n\t"
        "mov  %%ax,  %%fs       \n\t"
        "mov  %%ax,  %%gs       \n\t"
        "mov  %%ax,  %%ss       \n\t"

        /* 3. Far-return trick to flush the CS descriptor cache:
         *      push new CS (0x08 = GDT_KERNEL_CS)
         *      push address of label 1f  (next instruction)
         *      lretq  -> CPU pops RIP then CS atomically        */
        "push $0x08             \n\t"
        "lea  1f(%%rip), %%rax  \n\t"
        "push %%rax             \n\t"
        "lretq                  \n\t"
        "1:                     \n\t"
        :
        : [ptr] "m" (gdt_ptr)
        : "rax", "memory"
    );
}

/* ---------------------------------------------------------------
 * tss_load — load TSS selector into Task Register
 * --------------------------------------------------------------- */
void tss_load(void)
{
    __asm__ volatile (
        "ltr %[sel]\n\t"
        :
        : [sel] "r" ((uint16_t)GDT_TSS_SEL)
    );
}

/* ---------------------------------------------------------------
 * gdt_init — public entry point
 * --------------------------------------------------------------- */
void gdt_init(void)
{
    /* 0 — Null descriptor (mandatory) */
    gdt_set_entry(GDT_NULL_SEG, 0, 0, 0, 0);

    /* 1 — Kernel code, 64-bit (ring 0): L=1, DB=0 */
    gdt_set_entry(
        GDT_KERNEL_CODE_SEG, 0, 0xFFFFF,
        ACCESS_PRESENT | ACCESS_DPL(0) | ACCESS_S | ACCESS_EXEC | ACCESS_RW,
        FLAG_GRAN_4K | FLAG_64BIT
    );

    /* 2 — Kernel data (ring 0) */
    gdt_set_entry(
        GDT_KERNEL_DATA_SEG, 0, 0xFFFFF,
        ACCESS_PRESENT | ACCESS_DPL(0) | ACCESS_S | ACCESS_RW,
        FLAG_GRAN_4K | FLAG_32BIT
    );

    /* 3 — User code, 64-bit (ring 3) */
    gdt_set_entry(
        GDT_USER_CODE_SEG, 0, 0xFFFFF,
        ACCESS_PRESENT | ACCESS_DPL(3) | ACCESS_S | ACCESS_EXEC | ACCESS_RW,
        FLAG_GRAN_4K | FLAG_64BIT
    );

    /* 4 — User data (ring 3) */
    gdt_set_entry(
        GDT_USER_DATA_SEG, 0, 0xFFFFF,
        ACCESS_PRESENT | ACCESS_DPL(3) | ACCESS_S | ACCESS_RW,
        FLAG_GRAN_4K | FLAG_32BIT
    );

    /* 5+6 — TSS (16-byte system descriptor)
     *
     * iopb_offset past end of TSS = no I/O permission bitmap.
     * rsp0 stays 0; scheduler sets it per-thread before ring 3. */
    g_tss.iopb_offset = (uint16_t)sizeof(tss_t);
    gdt_set_tss((uint64_t)&g_tss, (uint32_t)(sizeof(tss_t) - 1));

    /* Build GDTR */
    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_ptr.base  = (uint64_t)&gdt;

    /* Reload GDTR + CS */
    gdt_flush();

    /* Load TSS into TR */
    tss_load();
}

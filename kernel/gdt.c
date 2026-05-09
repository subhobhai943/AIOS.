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

/* 16-byte GDT system descriptor (TSS, LDT, …)
 * Lower 8 bytes share the same layout as gdt_entry_t for base[23:0] / limit.
 * Upper 8 bytes hold base[63:32] + reserved.
 * We store them as two consecutive gdt_entry_t slots and alias the upper
 * one via a plain uint64_t overlay.                                        */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;   /* limit[15:0]                      */
    uint16_t base_00_15;  /* base[15:0]                       */
    uint8_t  base_16_23;  /* base[23:16]                      */
    uint8_t  type;        /* P=1 | DPL | 0 | type(0x9=avail)  */
    uint8_t  limit_hi;    /* G=0 | 0 | AVL | limit[19:16]     */
    uint8_t  base_24_31;  /* base[31:24]                      */
    uint32_t base_32_63;  /* base[63:32]                      */
    uint32_t reserved;    /* must be zero                     */
} tss_descriptor_t;

/* GDTR */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdt_ptr_t;

/* ---------------------------------------------------------------
 * Access-byte bit helpers
 * S=1 (code/data), S=0 (system descriptor like TSS)
 * --------------------------------------------------------------- */
#define ACCESS_PRESENT   (1u << 7)
#define ACCESS_DPL(x)    (((x) & 0x3u) << 5)
#define ACCESS_S         (1u << 4)   /* must be SET   for code/data */
                                     /* must be CLEAR for system    */
#define ACCESS_EXEC      (1u << 3)
#define ACCESS_DC        (1u << 2)
#define ACCESS_RW        (1u << 1)
#define ACCESS_ACCESSED  (1u << 0)

/* Flags nibble (stored in bits[7:4] of flags_limit_hi) */
#define FLAG_GRAN_4K     (1u << 7)
#define FLAG_32BIT       (1u << 6)   /* DB bit  */
#define FLAG_64BIT       (1u << 5)   /* L  bit  */

/* TSS type: 64-bit available TSS = 0x9 */
#define TSS_TYPE_AVAIL   (ACCESS_PRESENT | 0x9u)  /* DPL=0, S=0 */

/* ---------------------------------------------------------------
 * Storage
 * --------------------------------------------------------------- */

/* The GDT array.  Slots 5+6 are the low/high halves of the 16-byte
 * TSS descriptor — we write them by casting to tss_descriptor_t*.  */
static gdt_entry_t gdt[GDT_ENTRY_COUNT];
static gdt_ptr_t   gdt_ptr;

/* The actual hardware TSS (zeroed by BSS) */
tss_t g_tss;

/* ---------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------- */

static void gdt_set_entry(
    int     idx,
    uint32_t base,
    uint32_t limit,
    uint8_t  access,
    uint8_t  flags
)
{
    gdt[idx].base_low       = (uint16_t)( base        & 0xFFFF);
    gdt[idx].base_mid       = (uint8_t) ((base >> 16) & 0xFF);
    gdt[idx].base_high      = (uint8_t) ((base >> 24) & 0xFF);

    gdt[idx].limit_low      = (uint16_t)( limit       & 0xFFFF);
    gdt[idx].flags_limit_hi = (uint8_t) (((limit >> 16) & 0x0F) | (flags & 0xF0));

    gdt[idx].access = access;
}

static void gdt_set_tss(uint64_t base, uint32_t limit)
{
    /* Write the 16-byte TSS descriptor by overlaying the two 8-byte
     * GDT slots at indices GDT_TSS_SEG_LOW and GDT_TSS_SEG_HIGH.   */
    tss_descriptor_t *d = (tss_descriptor_t *)&gdt[GDT_TSS_SEG_LOW];

    d->limit_low  = (uint16_t)( limit        & 0xFFFF);
    d->base_00_15 = (uint16_t)( base         & 0xFFFF);
    d->base_16_23 = (uint8_t) ((base >> 16)  & 0xFF);
    d->type       = TSS_TYPE_AVAIL;
    d->limit_hi   = (uint8_t) ((limit >> 16) & 0x0F);  /* G=0, AVL=0 */
    d->base_24_31 = (uint8_t) ((base >> 24)  & 0xFF);
    d->base_32_63 = (uint32_t)((base >> 32)  & 0xFFFFFFFF);
    d->reserved   = 0;
}

/* ---------------------------------------------------------------
 * gdt_flush — reload GDTR and far-jump to reload CS
 *
 * After lgdt the CPU still has the old CS cached.  We must execute
 * a far jump / far return to a 64-bit code descriptor to flush it.
 *
 * Technique: push the new CS selector + a label address onto the
 * stack, then execute LRETQ (far return) which pops RIP then CS.
 * This is the standard approach in 64-bit mode where far JMP with
 * an immediate selector is not encodeable.
 * --------------------------------------------------------------- */
void gdt_flush(void)
{
    __asm__ volatile (
        /* 1. Load the new GDTR */
        "lgdt %[ptr]            \n\t"

        /* 2. Reload data-segment registers with kernel DS (0x10) */
        "mov  $0x10, %%ax       \n\t"
        "mov  %%ax,  %%ds       \n\t"
        "mov  %%ax,  %%es       \n\t"
        "mov  %%ax,  %%fs       \n\t"
        "mov  %%ax,  %%gs       \n\t"
        "mov  %%ax,  %%ss       \n\t"

        /* 3. Far-return trick: push CS selector then RIP of .flush_cs,
         *    then LRETQ — CPU pops RIP, then CS, flushing the code cache. */
        "push $0x08             \n\t"  /* new CS = GDT_KERNEL_CS */
        "lea  .flush_cs(%%rip), %%rax \n\t"
        "push %%rax             \n\t"
        "lretq                  \n\t"
        ".flush_cs:             \n\t"
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
    gdt_set_entry(
        GDT_NULL_SEG,
        0, 0, 0, 0
    );

    /* 1 — Kernel code, 64-bit (ring 0)
     *     L=1, DB=0  →  64-bit code segment.
     *     Limit is ignored in 64-bit mode but set to 0xFFFFF for convention. */
    gdt_set_entry(
        GDT_KERNEL_CODE_SEG,
        0,
        0xFFFFF,
        ACCESS_PRESENT | ACCESS_DPL(0) | ACCESS_S | ACCESS_EXEC | ACCESS_RW,
        FLAG_GRAN_4K | FLAG_64BIT
    );

    /* 2 — Kernel data (ring 0)
     *     DB=1 (32-bit stack default; harmless in 64-bit mode).
     *     In 64-bit mode data descriptors are mostly ignored but
     *     SS must reference a valid Present descriptor.           */
    gdt_set_entry(
        GDT_KERNEL_DATA_SEG,
        0,
        0xFFFFF,
        ACCESS_PRESENT | ACCESS_DPL(0) | ACCESS_S | ACCESS_RW,
        FLAG_GRAN_4K | FLAG_32BIT
    );

    /* 3 — User code, 64-bit (ring 3) */
    gdt_set_entry(
        GDT_USER_CODE_SEG,
        0,
        0xFFFFF,
        ACCESS_PRESENT | ACCESS_DPL(3) | ACCESS_S | ACCESS_EXEC | ACCESS_RW,
        FLAG_GRAN_4K | FLAG_64BIT
    );

    /* 4 — User data (ring 3) */
    gdt_set_entry(
        GDT_USER_DATA_SEG,
        0,
        0xFFFFF,
        ACCESS_PRESENT | ACCESS_DPL(3) | ACCESS_S | ACCESS_RW,
        FLAG_GRAN_4K | FLAG_32BIT
    );

    /* 5+6 — TSS (16-byte system descriptor)
     *
     *   g_tss.iopb_offset must point past the end of the TSS so
     *   the I/O permission bitmap is "empty" (all I/O ports denied
     *   in user mode, which is fine — we have no user processes yet).
     *
     *   rsp0 is left as 0 here; the scheduler will fill it in with
     *   the per-thread kernel-stack top before switching to ring 3. */
    g_tss.iopb_offset = (uint16_t)sizeof(tss_t);

    gdt_set_tss(
        (uint64_t)&g_tss,
        (uint32_t)(sizeof(tss_t) - 1)
    );

    /* Set up GDTR */
    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_ptr.base  = (uint64_t)&gdt;

    /* Reload GDTR + CS */
    gdt_flush();

    /* Load TSS into TR */
    tss_load();
}

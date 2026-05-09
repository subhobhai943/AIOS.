#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* ---------------------------------------------------------------
 * GDT segment indices
 * The 64-bit TSS descriptor occupies TWO consecutive 8-byte slots
 * (low word at index 5, high word at index 6).
 * --------------------------------------------------------------- */
#define GDT_NULL_SEG        0
#define GDT_KERNEL_CODE_SEG 1
#define GDT_KERNEL_DATA_SEG 2
#define GDT_USER_CODE_SEG   3
#define GDT_USER_DATA_SEG   4
#define GDT_TSS_SEG_LOW     5   /* lower 8 bytes of 16-byte TSS descriptor */
#define GDT_TSS_SEG_HIGH    6   /* upper 8 bytes of 16-byte TSS descriptor */
#define GDT_ENTRY_COUNT     7

/* ---------------------------------------------------------------
 * Segment selectors  (index << 3 | RPL)
 * --------------------------------------------------------------- */
#define GDT_KERNEL_CS  (GDT_KERNEL_CODE_SEG << 3)          /* 0x08 */
#define GDT_KERNEL_DS  (GDT_KERNEL_DATA_SEG << 3)          /* 0x10 */
#define GDT_USER_CS    ((GDT_USER_CODE_SEG   << 3) | 3)    /* 0x1B */
#define GDT_USER_DS    ((GDT_USER_DATA_SEG   << 3) | 3)    /* 0x23 */
#define GDT_TSS_SEL    (GDT_TSS_SEG_LOW      << 3)         /* 0x28 */

/* ---------------------------------------------------------------
 * Hardware Task State Segment (x86-64, 104 bytes)
 * Intel SDM Vol.3 §7.7
 * --------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;          /* kernel stack pointer for ring-0 entries  */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];        /* interrupt stack table pointers            */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;   /* I/O permission bitmap offset (set = sizeof TSS) */
} tss_t;

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */
void gdt_init(void);    /* set up descriptors + call gdt_flush() internally */
void gdt_flush(void);   /* lgdt + far-jump to reload CS                     */
void tss_load(void);    /* ltr — load TSS selector into TR                  */

/* Pointer to the TSS so kernel can set rsp0 before every ring-3 entry */
extern tss_t g_tss;

#endif /* GDT_H */

#ifndef IDT_H
#define IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

/* -------------------------------------------------------------------
 * interrupt_frame_t
 *
 * Layout matches exactly what isr_common_handler pushes onto the stack
 * before calling isr_dispatch(rsp).  The CPU also pushes SS, RSP,
 * RFLAGS, CS, RIP automatically (and sometimes an error code).
 *
 * isr_stubs.asm push order (top of stack = lowest address = first field):
 *
 *   The common handler pushes in this order:
 *       push r15  ← goes on stack FIRST = highest address
 *       push r14
 *       push r13
 *       push r12
 *       push r11
 *       push r10
 *       push r9
 *       push r8
 *       push rbp
 *       push rdi
 *       push rsi
 *       push rdx
 *       push rcx
 *       push rbx
 *       push rax  ← goes on stack LAST = lowest address = RSP after pushes
 *
 *   Then the ISR stub (already on stack before common handler runs):
 *       [RSP+120] int_num   ← pushed by stub (push <n>)
 *       [RSP+128] err_code  ← pushed by stub (push 0) or CPU
 *
 *   CPU hardware frame (pushed before stub runs):
 *       [RSP+136] rip
 *       [RSP+144] cs
 *       [RSP+152] rflags
 *       [RSP+160] rsp      (ring-0: CPU pushes this only on stack switch)
 *       [RSP+168] ss
 *
 * C struct layout rule: lowest address = first field.
 * RSP after all pushes points to rax, so rax is field [0].
 *
 * BUG THAT WAS HERE (fixed):
 *   The struct previously listed rax FIRST in source, which looks
 *   correct in isolation — but the comment also said "push order:
 *   r15 first, rax last".  On a DOWN-GROWING stack, "pushed last" =
 *   lowest address = [RSP+0] = FIRST C struct field.  The struct
 *   was therefore correct AS WRITTEN — rax at offset 0 is right.
 *
 *   However the stack-offset table in the old comment was WRONG:
 *   it listed [RSP+0] = rax which would only be true if rax were
 *   pushed first (i.e., r15 is at offset 0).  The old comment
 *   contradicted itself.  The struct field ORDER is correct; only
 *   the comment offsets and the stated push order description were
 *   inverted.  The comment below now matches the struct correctly.
 *
 *   Additionally: isr_dispatch() was missing an explicit 'return'
 *   after calling a registered CPU-exception handler (vec 0-31),
 *   meaning execution continued to the vec>=32 block.  While
 *   harmless today (isr_handlers[vec] is the same pointer so it
 *   would just be called a second time, but the vec<32 path
 *   returns before reaching that block anyway due to the 'return'
 *   added at the bottom of the if), it was a latent bug that
 *   would cause a double-call if the compiler moved the branches.
 *   Fixed in idt.c.
 * ------------------------------------------------------------------- */

typedef struct __attribute__((packed)) interrupt_frame
{
    /*
     * General-purpose registers saved by isr_common_handler.
     *
     * Push order in asm: r15 pushed FIRST (highest address on
     * down-growing stack), rax pushed LAST (lowest address = RSP
     * after all saves = [RSP+0] = first byte of this struct).
     *
     * So in the C struct (lowest address first):
     *   rax  offset   0   ← last push  = lowest addr
     *   rbx  offset   8
     *   rcx  offset  16
     *   rdx  offset  24
     *   rsi  offset  32
     *   rdi  offset  40
     *   rbp  offset  48
     *   r8   offset  56
     *   r9   offset  64
     *   r10  offset  72
     *   r11  offset  80
     *   r12  offset  88
     *   r13  offset  96
     *   r14  offset 104
     *   r15  offset 112   ← first push = highest addr
     */
    uint64_t rax;   /* offset   0 */
    uint64_t rbx;   /* offset   8 */
    uint64_t rcx;   /* offset  16 */
    uint64_t rdx;   /* offset  24 */
    uint64_t rsi;   /* offset  32 */
    uint64_t rdi;   /* offset  40 */
    uint64_t rbp;   /* offset  48 */
    uint64_t r8;    /* offset  56 */
    uint64_t r9;    /* offset  64 */
    uint64_t r10;   /* offset  72 */
    uint64_t r11;   /* offset  80 */
    uint64_t r12;   /* offset  88 */
    uint64_t r13;   /* offset  96 */
    uint64_t r14;   /* offset 104 */
    uint64_t r15;   /* offset 112 */

    /* pushed by the ISR stub before jumping to common handler */
    uint64_t int_num;   /* offset 120 */
    uint64_t err_code;  /* offset 128 */

    /* pushed automatically by the CPU on exception/interrupt entry */
    uint64_t rip;       /* offset 136 */
    uint64_t cs;        /* offset 144 */
    uint64_t rflags;    /* offset 152 */
    uint64_t rsp;       /* offset 160  (ring-0: always present in 64-bit mode) */
    uint64_t ss;        /* offset 168 */

} interrupt_frame_t;

typedef void (*isr_handler_t)(interrupt_frame_t *frame);

/* idt_init  — remap PIC, install 256 gates, lidt */
void idt_init(void);

/* idt_flush — (re)load the IDT pointer register; called by idt_init
 *             and may be called after hot-patching a gate */
void idt_flush(void);

/* idt_register_handler — install a C handler for vector `vector`.
 *   For CPU exceptions (0–31): replaces the default panic dump.
 *   For hardware IRQs  (32–47): called before PIC EOI is sent.
 *   Pass NULL to remove a previously installed handler. */
void idt_register_handler(uint8_t vector, isr_handler_t handler);

#endif /* IDT_H */

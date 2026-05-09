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
 * Stack at the point RDI = RSP in isr_common_handler (top → bottom):
 *
 *   [RSP+0]   rax           ← first pushed by common handler
 *   [RSP+8]   rbx
 *   [RSP+16]  rcx
 *   [RSP+24]  rdx
 *   [RSP+32]  rsi
 *   [RSP+40]  rdi
 *   [RSP+48]  rbp
 *   [RSP+56]  r8
 *   [RSP+64]  r9
 *   [RSP+72]  r10
 *   [RSP+80]  r11
 *   [RSP+88]  r12
 *   [RSP+96]  r13
 *   [RSP+104] r14
 *   [RSP+112] r15
 *   [RSP+120] int_num       ← pushed by ISR stub (push <n>)
 *   [RSP+128] err_code      ← pushed by stub (push 0) or CPU
 *   [RSP+136] rip           ← CPU-pushed exception frame
 *   [RSP+144] cs
 *   [RSP+152] rflags
 *   [RSP+160] rsp           ← ring-0 fault: CPU may or may not push
 *   [RSP+168] ss
 *
 * NOTE: isr_stubs.asm pushes int_num FIRST then err_code (or 0).
 * The struct below reflects the layout as seen in C (lowest address first).
 * ------------------------------------------------------------------- */

typedef struct __attribute__((packed)) interrupt_frame
{
    /* saved by isr_common_handler (push order: r15 first, rax last) */
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;

    /* pushed by the ISR stub */
    uint64_t int_num;
    uint64_t err_code;

    /* pushed automatically by the CPU */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;

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

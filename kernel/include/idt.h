#ifndef IDT_H
#define IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

typedef struct interrupt_frame
{
    /* pushed manually in asm */
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;

    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;

    /* pushed by stub */
    uint64_t int_num;
    uint64_t err_code;

    /* pushed automatically by CPU */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;

} interrupt_frame_t;

typedef void (*isr_handler_t)(interrupt_frame_t *frame);

void idt_init(void);

void idt_register_handler(
    uint8_t vector,
    isr_handler_t handler
);

#endif

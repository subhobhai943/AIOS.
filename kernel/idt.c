/* ============================================================
 * AIOS — Interrupt Descriptor Table (IDT)
 * Phase 1.1 — All 256 gates, exception register dump + halt,
 *             idt_flush(), PIC remap (IRQs → vectors 32–47)
 * ============================================================ */

#include "include/idt.h"
#include "include/vga.h"

#include <stdint.h>

/* ============================================================
 * ISR STUB TABLE (defined in isr_stubs.asm)
 * ============================================================ */

extern void *isr_stub_table[];

/* ============================================================
 * IDT STRUCTURES
 * ============================================================ */

typedef struct __attribute__((packed))
{
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} idt_entry_t;

typedef struct __attribute__((packed))
{
    uint16_t limit;
    uint64_t base;
} idt_ptr_t;

/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define IDT_INTERRUPT_GATE  0x8E   /* present, DPL=0, 64-bit interrupt gate */
#define IDT_TRAP_GATE       0x8F   /* present, DPL=0, 64-bit trap gate      */

/* ============================================================
 * STORAGE
 * ============================================================ */

static idt_entry_t idt[IDT_ENTRIES];
static idt_ptr_t   idt_ptr;

static isr_handler_t isr_handlers[IDT_ENTRIES];

/* ============================================================
 * PORT I/O
 * ============================================================ */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ============================================================
 * PIC
 * ============================================================ */

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20

static void pic_remap(int offset1, int offset2)
{
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD,  0x11);
    outb(PIC2_CMD,  0x11);
    outb(PIC1_DATA, (uint8_t)offset1);
    outb(PIC2_DATA, (uint8_t)offset2);
    outb(PIC1_DATA, 0x04);   /* slave on IRQ2 */
    outb(PIC2_DATA, 0x02);   /* cascade identity */
    outb(PIC1_DATA, 0x01);   /* 8086 mode */
    outb(PIC2_DATA, 0x01);
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

/* ============================================================
 * EXCEPTION NAME TABLE
 * ============================================================ */

static const char *exception_names[32] = {
    "#DE  Divide-by-Zero",
    "#DB  Debug",
    "#NMI Non-Maskable Interrupt",
    "#BP  Breakpoint",
    "#OF  Overflow",
    "#BR  Bound Range Exceeded",
    "#UD  Invalid Opcode",
    "#NM  Device Not Available",
    "#DF  Double Fault",
    "     Coprocessor Segment Overrun (reserved)",
    "#TS  Invalid TSS",
    "#NP  Segment Not Present",
    "#SS  Stack-Segment Fault",
    "#GP  General Protection Fault",
    "#PF  Page Fault",
    "     Reserved (vector 15)",
    "#MF  x87 Floating-Point Exception",
    "#AC  Alignment Check",
    "#MC  Machine Check",
    "#XF  SIMD Floating-Point Exception",
    "#VE  Virtualization Exception",
    "#CP  Control-Protection Exception",
    "     Reserved (vector 22)",
    "     Reserved (vector 23)",
    "     Reserved (vector 24)",
    "     Reserved (vector 25)",
    "     Reserved (vector 26)",
    "     Reserved (vector 27)",
    "#HV  Hypervisor Injection Exception",
    "#VC  VMM Communication Exception",
    "#SX  Security Exception",
    "     Reserved (vector 31)",
};

/* ============================================================
 * REGISTER DUMP — called for CPU exceptions (vectors 0–31)
 * Prints all saved registers in a panic-style box, then halts.
 * ============================================================ */

static void exception_dump(interrupt_frame_t *f)
{
    /* Red title bar */
    vga_puts_color("\n", VGA_COLOR_WHITE, VGA_COLOR_RED);
    vga_puts_color(" *** KERNEL EXCEPTION (PANIC) ***\n",
                   VGA_COLOR_WHITE, VGA_COLOR_RED);
    vga_puts_color(" ", VGA_COLOR_WHITE, VGA_COLOR_RED);

    /* Exception name */
    vga_puts_color(" Vector 0x", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_puthex(f->int_num);
    vga_puts_color("  ", VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    if (f->int_num < 32) {
        vga_puts_color(exception_names[f->int_num],
                       VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    } else {
        vga_puts_color("(unknown exception)",
                       VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    }
    vga_puts_color("\n", VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* Error code (only meaningful for some exceptions) */
    vga_puts_color(" Error code : 0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puthex(f->err_code);
    vga_putchar('\n');

    /* Separator */
    vga_puts_color("---------- Registers ----------\n",
                   VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    /* General-purpose registers */
    vga_puts_color(" RAX=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->rax);
    vga_puts_color("  RBX=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->rbx); vga_putchar('\n');

    vga_puts_color(" RCX=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->rcx);
    vga_puts_color("  RDX=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->rdx); vga_putchar('\n');

    vga_puts_color(" RSI=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->rsi);
    vga_puts_color("  RDI=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->rdi); vga_putchar('\n');

    vga_puts_color(" RBP=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->rbp);
    vga_puts_color("  RSP=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->rsp); vga_putchar('\n');

    vga_puts_color("  R8=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->r8);
    vga_puts_color("   R9=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->r9);  vga_putchar('\n');

    vga_puts_color(" R10=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->r10);
    vga_puts_color("  R11=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->r11); vga_putchar('\n');

    vga_puts_color(" R12=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->r12);
    vga_puts_color("  R13=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->r13); vga_putchar('\n');

    vga_puts_color(" R14=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->r14);
    vga_puts_color("  R15=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK); vga_puthex(f->r15); vga_putchar('\n');

    /* Control / instruction registers */
    vga_puts_color("---------- Control/IP ---------\n",
                   VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    vga_puts_color(" RIP=0x", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK); vga_puthex(f->rip);
    vga_puts_color("   CS=0x", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK); vga_puthex(f->cs);  vga_putchar('\n');

    vga_puts_color(" RFLAGS=0x", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK); vga_puthex(f->rflags);
    vga_puts_color("  SS=0x",    VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK); vga_puthex(f->ss); vga_putchar('\n');

    vga_puts_color("-------------------------------\n",
                   VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts_color(" System halted.\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);

    /* Disable interrupts and halt forever */
    __asm__ volatile (
        "cli\n"
        "1: hlt\n"
        "   jmp 1b\n"
    );
    __builtin_unreachable();
}

/* ============================================================
 * IDT GATE SETTER
 * ============================================================ */

static void idt_set_gate(
    uint8_t  vec,
    void    *isr,
    uint8_t  flags
)
{
    uint64_t addr = (uint64_t)isr;

    idt[vec].offset_low  = (uint16_t)( addr        & 0xFFFF);
    idt[vec].selector    = 0x08;               /* kernel code segment */
    idt[vec].ist         = 0;
    idt[vec].type_attr   = flags;
    idt[vec].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vec].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vec].zero        = 0;
}

/* ============================================================
 * ISR DISPATCH — called from every ISR stub via isr_common_handler
 * ============================================================ */

void isr_dispatch(interrupt_frame_t *frame)
{
    uint64_t vec = frame->int_num;

    /* CPU exceptions (vectors 0–31): if no custom handler is
     * registered, fall through to the default register dump. */
    if (vec < 32) {
        if (isr_handlers[vec]) {
            isr_handlers[vec](frame);
        } else {
            /* Default: print full register dump and halt */
            exception_dump(frame);
        }
        return;
    }

    /* Hardware IRQs (vectors 32–47, remapped PIC) */
    if (vec >= 32 && vec < 48) {
        if (isr_handlers[vec]) {
            isr_handlers[vec](frame);
        }
        /* Send End-Of-Interrupt to PIC(s) */
        if (vec >= 40) {
            outb(PIC2_CMD, PIC_EOI);
        }
        outb(PIC1_CMD, PIC_EOI);
        return;
    }

    /* Vectors 48–255: call registered handler if any */
    if (isr_handlers[vec]) {
        isr_handlers[vec](frame);
    }
}

/* ============================================================
 * REGISTER HANDLER
 * ============================================================ */

void idt_register_handler(uint8_t vector, isr_handler_t handler)
{
    isr_handlers[vector] = handler;
}

/* ============================================================
 * IDT FLUSH — load the IDT pointer register
 * Called by idt_init(); may also be called after hot-patching gates.
 * ============================================================ */

void idt_flush(void)
{
    __asm__ volatile (
        "lidt %0"
        :
        : "m"(idt_ptr)
    );
}

/* ============================================================
 * IDT INIT
 * 1. Remap PIC: IRQ0-7  → vectors 32-39
 *               IRQ8-15 → vectors 40-47
 * 2. Install all 256 ISR stubs (CPU exceptions use TRAP gates,
 *    IRQs use INTERRUPT gates to avoid spurious re-entry).
 * 3. Load IDT via idt_flush().
 * ============================================================ */

void idt_init(void)
{
    /* Zero out the entire IDT and handler table first */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt[i].offset_low  = 0;
        idt[i].selector    = 0;
        idt[i].ist         = 0;
        idt[i].type_attr   = 0;
        idt[i].offset_mid  = 0;
        idt[i].offset_high = 0;
        idt[i].zero        = 0;
        isr_handlers[i]    = 0;
    }

    /* Remap the 8259 PIC so IRQ0-15 → vectors 32-47 */
    pic_remap(0x20, 0x28);

    /* Install CPU exception stubs (vectors 0–31) as TRAP gates.
     * Trap gates do NOT clear IF, so nested interrupts are possible
     * during exception handling if STI is later called.  For our
     * current panic path that's fine — we cli inside exception_dump. */
    for (int i = 0; i < 32; i++) {
        idt_set_gate((uint8_t)i, isr_stub_table[i], IDT_TRAP_GATE);
    }

    /* Install hardware IRQ stubs (vectors 32–47) as INTERRUPT gates.
     * Interrupt gates clear IF on entry, preventing nested IRQs. */
    for (int i = 32; i < 48; i++) {
        idt_set_gate((uint8_t)i, isr_stub_table[i], IDT_INTERRUPT_GATE);
    }

    /* Remaining vectors 48–255 as interrupt gates (spurious / future use) */
    for (int i = 48; i < IDT_ENTRIES; i++) {
        idt_set_gate((uint8_t)i, isr_stub_table[i], IDT_INTERRUPT_GATE);
    }

    /* Set up IDT pointer and load */
    idt_ptr.limit = (uint16_t)(sizeof(idt) - 1);
    idt_ptr.base  = (uint64_t)&idt;
    idt_flush();

    vga_puts_color(
        "  [ OK ] IDT: 256 gates installed, PIC remapped (IRQ→vec+32)\n",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK
    );
}

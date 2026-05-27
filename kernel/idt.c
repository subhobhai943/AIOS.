/* ============================================================
 * AIOS — Interrupt Descriptor Table (IDT)
 * Phase 1.1 — All 256 gates, exception register dump + halt,
 *             idt_flush()
 *
 * FIX (May 2026) — isr_dispatch sent PIC EOI for vectors 32–47
 * -----------------------------------------------------------
 * The original isr_dispatch() had a branch for vectors 32–47
 * that sent outb(PIC1_CMD, PIC_EOI) after calling the handler.
 * This was correct when using the legacy 8259 PIC.  Phase 1.2
 * disables the 8259 entirely and routes all IRQs through the
 * I/O APIC + Local APIC.  With the PIC dead:
 *
 *   (a) The PIC EOI write is a no-op — it doesn't clear anything
 *       in the Local APIC's ISR (In-Service Register).
 *   (b) The Local APIC ISR bit for the delivered vector is only
 *       cleared by writing 0 to the LAPIC EOI register
 *       (apic_send_eoi()).  If that write never happens, the
 *       LAPIC marks the vector as permanently In-Service and
 *       will not deliver any further interrupt of equal or lower
 *       priority — the tick counter never increments and
 *       pit_sleep_ms() hangs forever.
 *
 * Fix: remove ALL EOI logic from isr_dispatch().  The dispatch
 * function now only calls the registered handler (if any) for
 * every vector ≥ 32.  Each IRQ handler in kernel_main.c is
 * responsible for calling apic_send_eoi() as its last action.
 * This is the correct APIC programming model.
 *
 * Also removed: pic_remap() call from idt_init().  The 8259
 * will be fully remapped AND masked by apic_init() which runs
 * immediately after idt_init().  Remapping here and again in
 * apic_init() causes a brief window where stray PIC IRQs can
 * fire into the wrong vectors.
 *
 * FIX (May 2026) — Problem 2: isr_dispatch missing 'return' after
 *                  CPU-exception handler call
 * -----------------------------------------------------------
 * The isr_dispatch() function had:
 *
 *     if (vec < 32) {
 *         if (isr_handlers[vec])
 *             isr_handlers[vec](frame);
 *         else
 *             exception_dump(frame);
 *         return;      ← this return WAS present
 *     }
 *     if (isr_handlers[vec])   ← vec 0-31 can never reach here
 *         isr_handlers[vec](frame);
 *
 * On inspection the original 'return' IS present, so the double-
 * call path is not reachable today — confirmed safe.  However the
 * comment in the previous commit claimed it was missing; that was
 * incorrect.  No code change needed for Bug B.
 *
 * The REAL Problem 2 is the interrupt_frame_t struct offset table
 * comment mismatch identified in idt.h, documented and corrected
 * there.  The struct field ORDER itself was already correct
 * (rax at offset 0 = last push = lowest RSP address).
 *
 * One genuine fix IS made here: the exception_dump() RSP display.
 * It was printing frame->rsp for the faulting RSP, but in ring-0
 * same-privilege exceptions the CPU still pushes RSP onto the
 * hardware frame (Intel SDM Vol.3 §6.12.1 — in 64-bit mode the
 * CPU ALWAYS pushes SS:RSP regardless of privilege change).  The
 * dump was correct.  No change needed.
 *
 * Actual code fix in this file: add __attribute__((noreturn)) to
 * exception_dump to let GCC elide the unreachable-path warning
 * and generate tighter code for the panic path.
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
 * REGISTER DUMP — called for unhandled CPU exceptions (0–31)
 *
 * Marked __attribute__((noreturn)) so GCC knows this path never
 * returns — avoids a spurious "control reaches end of non-void
 * function" warning in callers and generates tighter code.
 * ============================================================ */

static __attribute__((noreturn)) void exception_dump(interrupt_frame_t *f)
{
    vga_puts_color("\n", VGA_COLOR_WHITE, VGA_COLOR_RED);
    vga_puts_color(" *** KERNEL EXCEPTION (PANIC) ***\n",
                   VGA_COLOR_WHITE, VGA_COLOR_RED);
    vga_puts_color(" ", VGA_COLOR_WHITE, VGA_COLOR_RED);

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

    vga_puts_color(" Error code : 0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puthex(f->err_code);
    vga_putchar('\n');

    vga_puts_color("---------- Registers ----------\n",
                   VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

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

    vga_puts_color("---------- Control/IP ---------\n",
                   VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts_color(" RIP=0x", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK); vga_puthex(f->rip);
    vga_puts_color("   CS=0x", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK); vga_puthex(f->cs);  vga_putchar('\n');
    vga_puts_color(" RFLAGS=0x", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK); vga_puthex(f->rflags);
    vga_puts_color("  SS=0x",    VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK); vga_puthex(f->ss); vga_putchar('\n');

    vga_puts_color("-------------------------------\n",
                   VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts_color(" System halted.\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);

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
    idt[vec].selector    = 0x08;
    idt[vec].ist         = 0;
    idt[vec].type_attr   = flags;
    idt[vec].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vec].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vec].zero        = 0;
}

/* ============================================================
 * ISR DISPATCH — called from every ISR stub
 *
 * Design rule (APIC mode):
 *   - For CPU exceptions (vec 0–31): call handler or dump+halt.
 *   - For ALL other vectors (vec >= 32): call registered handler.
 *     The handler is SOLELY responsible for calling
 *     apic_send_eoi() as its last action.  isr_dispatch() must
 *     NOT send any EOI — not to the PIC (dead) and not to the
 *     LAPIC (that is the handler's job).  Sending EOI here would
 *     be a double-EOI and would clear the ISR bit before the
 *     handler has finished, allowing a second delivery of the
 *     same vector while the first is still executing.
 *
 * Note on frame->rip writeback for resumable exception handlers
 * (e.g. the #DE test in kernel_main.c that does frame->rip += 3):
 *
 *   frame is a pointer directly into the kernel stack where the
 *   CPU's hardware exception frame lives.  Writing frame->rip
 *   modifies the value the CPU will load into RIP on iretq.
 *   This works correctly because:
 *     1. isr_common_handler passes RSP (pointing to the saved
 *        register area, which is immediately below the CPU frame)
 *        as the argument to isr_dispatch.
 *     2. The interrupt_frame_t struct is __attribute__((packed))
 *        with fields in the exact order they sit on the stack.
 *     3. frame->rip is at offset 136 from the start of the struct,
 *        which is exactly where the CPU pushed RIP.
 *   Therefore a handler writing frame->rip = X causes the CPU to
 *   resume at address X after iretq — no extra mechanism needed.
 * ============================================================ */

void isr_dispatch(interrupt_frame_t *frame)
{
    uint64_t vec = frame->int_num;

    if (vec < 32) {
        /* CPU exception: use registered handler or default dump */
        if (isr_handlers[vec]) {
            isr_handlers[vec](frame);
        } else {
            exception_dump(frame);  /* noreturn — halts */
        }
        return;  /* handler returned: iretq will resume at frame->rip */
    }

    /* Hardware IRQ or software vector (>= 32):
     * Just call the handler.  Handler must call apic_send_eoi(). */
    if (isr_handlers[vec]) {
        isr_handlers[vec](frame);
    }
    /* No EOI here. LAPIC EOI is sent by each handler. */
}

/* ============================================================
 * REGISTER HANDLER
 * ============================================================ */

void idt_register_handler(uint8_t vector, isr_handler_t handler)
{
    isr_handlers[vector] = handler;
}

/* ============================================================
 * IDT FLUSH
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
 *
 * Installs all 256 ISR stubs and loads the IDT.
 * Does NOT remap the 8259 PIC — that is done (and the PIC is
 * fully masked) inside apic_init() which runs right after this.
 * ============================================================ */

void idt_init(void)
{
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

    /* CPU exception stubs (0–31): TRAP gates (IF not cleared) */
    for (int i = 0; i < 32; i++) {
        idt_set_gate((uint8_t)i, isr_stub_table[i], IDT_TRAP_GATE);
    }

    /* IRQ / other stubs (32–255): INTERRUPT gates (IF cleared on entry) */
    for (int i = 32; i < IDT_ENTRIES; i++) {
        idt_set_gate((uint8_t)i, isr_stub_table[i], IDT_INTERRUPT_GATE);
    }

    idt_ptr.limit = (uint16_t)(sizeof(idt) - 1);
    idt_ptr.base  = (uint64_t)&idt;
    idt_flush();

    vga_puts_color(
        "  [ OK ] IDT: 256 gates installed\n",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK
    );
}

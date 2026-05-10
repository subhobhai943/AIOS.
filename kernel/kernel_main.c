/* ============================================================
 * AIOS — Kernel Main
 * Phase 1.3 : PIT timer — IRQ0 wired, 1000 Hz tick counter
 *
 * FIX (May 2026): IRQ0 hang after STI
 * -----------------------------------------------------------
 * Root cause: ioapic_route_irq0() was called inside apic_init()
 * BEFORE idt_register_handler(0x20, pit_irq_handler) ran.  The
 * PIT chip fired its first tick the moment STI executed, the
 * I/O APIC delivered it to the default unhandled IDT stub which
 * never called apic_send_eoi().  The Local APIC then held vector
 * 0x20 as In-Service permanently, masking all further interrupts.
 *
 * Fix: ioapic_route(0, 0x20, 0) is now called in kernel_main
 * AFTER idt_register_handler(0x20, pit_irq_handler), following
 * the same pattern used for keyboard (IRQ1) and mouse (IRQ12).
 * ============================================================ */

#include "include/vga.h"
#include "include/gdt.h"
#include "include/idt.h"
#include "include/pmm.h"
#include "include/vmm.h"
#include "include/heap.h"
#include "include/keyboard.h"
#include "include/mouse.h"
#include "include/pit.h"
#include "apic.h"

#include <stdint.h>

#define MULTIBOOT2_MAGIC      0x36D76289u
#define MB2_TAG_TYPE_MMAP     6u
#define MB2_TAG_TYPE_END      0u

extern uint8_t _kernel_start;
extern uint8_t _kernel_end;

#define HEAP_SIZE   (2u * 1024u * 1024u)

static void print_ok(const char *msg)
{
    vga_puts_color("[ OK ] ", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts(msg);
    vga_putchar('\n');
}

static void print_warn(const char *msg)
{
    vga_puts_color("[WARN] ", VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    vga_puts(msg);
    vga_putchar('\n');
}

static void print_fail(const char *msg)
{
    vga_puts_color("[FAIL] ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_puts(msg);
    vga_putchar('\n');
    for (;;) __asm__ volatile ("hlt");
}

static uint64_t find_mmap_tag(uint32_t mb2_addr, uint32_t *tag_size_out)
{
    uint32_t total = *(uint32_t *)(uintptr_t)mb2_addr;
    uint32_t off   = 8;
    while (off < total) {
        uint32_t type = *(uint32_t *)(uintptr_t)(mb2_addr + off);
        uint32_t size = *(uint32_t *)(uintptr_t)(mb2_addr + off + 4);
        if (type == MB2_TAG_TYPE_END) break;
        if (type == MB2_TAG_TYPE_MMAP) {
            if (tag_size_out) *tag_size_out = size;
            return (uint64_t)(mb2_addr + off);
        }
        off += (size + 7u) & ~7u;
    }
    return 0;
}

static void de_test_handler(interrupt_frame_t *frame)
{
    vga_puts_color("  [ OK ] #DE handler fired — vector=0x",
                   VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puthex(frame->int_num);
    vga_puts_color("  RIP=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puthex(frame->rip);
    vga_putchar('\n');
    frame->rip += 3;
}

/* ---------------------------------------------------------------
 * IRQ handlers
 * Every handler MUST call apic_send_eoi() as its last action.
 * The order is: do work -> send EOI.  Never send EOI before work
 * is done (re-entrancy risk), never skip EOI (ISR stall).
 * --------------------------------------------------------------- */

static void pit_irq_handler(interrupt_frame_t *f)
{
    (void)f;
    pit_tick();
    apic_send_eoi();
}

static void kbd_isr(interrupt_frame_t *f)
{
    (void)f;
    keyboard_handle_irq();
    apic_send_eoi();
}

static void mouse_isr(interrupt_frame_t *f)
{
    (void)f;
    mouse_handle_irq();
    apic_send_eoi();
}

void kernel_main(uint32_t magic, uint32_t addr)
{
    vga_init();

    vga_puts_color(
        "====================================================\n",
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts_color(
        "  AIOS  Autonomous Intelligent Operating System\n",
        VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts_color(
        "  Phase 1.3 : PIT Timer — 1000 Hz tick counter\n",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts_color(
        "====================================================\n\n",
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);

    /* ---- Multiboot2 ----------------------------------------- */
    if (magic == MULTIBOOT2_MAGIC)
        print_ok("Multiboot2 magic OK");
    else {
        vga_puts_color("[WARN] MB2 magic mismatch: got 0x",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
        vga_puthex((uint64_t)magic);
        vga_puts_color(" expected 0x36D76289\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    }

    /* ---- GDT ------------------------------------------------- */
    gdt_init();
    print_ok("GDT: null/kcode/kdata/ucode/udata + TSS");

    /* ---- IDT ------------------------------------------------- */
    idt_init();
    print_ok("IDT: 256 gates, exception dump active");

    /* ---- #DE regression test --------------------------------- */
    idt_register_handler(0, de_test_handler);
    vga_puts_color("  [TEST] Triggering #DE...\n",
                   VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    __asm__ volatile ("xor %%rcx,%%rcx; div %%rcx" ::: "rax","rdx","rcx");
    print_ok("#DE test PASSED");
    idt_register_handler(0, 0);

    /* ---- VMM ------------------------------------------------- */
    vmm_init();
    print_ok("VMM: 4-level paging, 64 MB identity map");

    /* ---- PMM ------------------------------------------------- */
    if (magic == MULTIBOOT2_MAGIC) {
        uint32_t mmap_size = 0;
        uint64_t mmap_tag  = find_mmap_tag(addr, &mmap_size);
        if (!mmap_tag) print_fail("MB2 mmap tag not found");
        uint64_t kstart = (uint64_t)(uintptr_t)&_kernel_start;
        uint64_t kend   = (uint64_t)(uintptr_t)&_kernel_end;
        pmm_init(mmap_tag, mmap_size, kstart, kend);
        print_ok("PMM: bitmap allocator ready");
    } else {
        print_warn("PMM skipped — heap uses static region");
    }

    /* ---- Heap ------------------------------------------------ */
    uint64_t kend_aligned = ((uint64_t)(uintptr_t)&_kernel_end
                             + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    heap_init(kend_aligned, HEAP_SIZE);
    print_ok("Heap: 2 MB kernel heap ready");

    /* ---- APIC (maps MMIO, disables PIC, enables LAPIC) ------- */
    apic_init();
    print_ok("APIC: legacy PIC dead, LAPIC active");

    /* ================================================================
     * IRQ ROUTING — must happen AFTER IDT handler is registered.
     *
     * Correct sequence for every IRQ:
     *   1. idt_register_handler(vector, handler_fn)  <- handler in place
     *   2. ioapic_route(irq, vector, 0)              <- IOAPIC enabled
     *   3. sti (later)                               <- CPU accepts IRQs
     *
     * If step 2 comes before step 1, the very first delivery hits
     * the default stub (no EOI), the APIC stalls, game over.
     * ================================================================ */

    /* IRQ0 — PIT timer */
    idt_register_handler(0x20, pit_irq_handler);
    ioapic_route(0, 0x20, 0);           /* IRQ0 → vec 0x20, BSP */
    pit_init(1000);
    print_ok("PIT: 1000 Hz, IRQ0 → vec 0x20");

    /* IRQ1 — PS/2 keyboard */
    idt_register_handler(0x21, kbd_isr);
    ioapic_route(1, 0x21, 0);           /* IRQ1 → vec 0x21 */
    keyboard_init();
    print_ok("Keyboard: IRQ1 → vec 0x21");

    /* IRQ12 — PS/2 mouse */
    idt_register_handler(0x2C, mouse_isr);
    ioapic_route(12, 0x2C, 0);          /* IRQ12 → vec 0x2C */
    mouse_init();
    print_ok("Mouse: IRQ12 → vec 0x2C");

    /* ---- Enable interrupts — PIT starts ticking ------------- */
    __asm__ volatile ("sti");
    print_ok("Interrupts enabled (STI) — PIT ticking");

    /* ---- Phase 1.3 smoke-test -------------------------------- */
    vga_puts_color(
        "\n  [TEST] Waiting 1 second via pit_sleep_ms(1000)...\n",
        VGA_COLOR_BROWN, VGA_COLOR_BLACK
    );
    uint64_t t0 = pit_get_ticks();
    pit_sleep_ms(1000);
    uint64_t t1 = pit_get_ticks();
    uint64_t elapsed = t1 - t0;

    vga_puts_color("  [TEST] Ticks elapsed: ",
                   VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    vga_putdec(elapsed);
    vga_putchar('\n');

    if (elapsed >= 900ULL && elapsed <= 1100ULL)
        print_ok("PIT tick test PASSED (~1000 ticks in 1 second)");
    else if (elapsed > 0)
        print_warn("Tick count out of range — check APIC/IRQ0 routing");
    else
        print_fail("PIT tick test FAILED — ticks=0, IRQ0 not firing");

    vga_putchar('\n');
    vga_puts_color("AIOS Phase 1.3 boot complete.  Waiting for input...\n",
                   VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);

    for (;;) __asm__ volatile ("hlt");
}

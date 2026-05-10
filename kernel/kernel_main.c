/* ============================================================
 * AIOS — Kernel Main
 * Phase 1.3 : PIT timer — IRQ0 wired, 1000 Hz tick counter
 *
 * Previous phase: Phase 1.2 — APIC (legacy PIC dead,
 *                 Local APIC + I/O APIC active).
 *
 * What's new in this phase:
 *   - pit_init(1000) called AFTER apic_init() (APIC must be up
 *     before IRQ0 can fire through the I/O APIC).
 *   - IRQ0 (vector 0x20) registered in IDT → pit_irq_handler().
 *   - pit_irq_handler() calls pit_tick() then apic_send_eoi().
 *   - Tick-count smoke-test: waits 1 second via pit_sleep_ms(),
 *     then reads the tick counter; prints PASS if >= 900.
 *
 * Parameter layout (System V AMD64 ABI, set by kernel_entry.asm):
 *   RDI = magic  (0x36D76289)
 *   RSI = addr   (physical address of Multiboot2 info struct)
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

/* ---------------------------------------------------------------
 * Multiboot2 constants
 * --------------------------------------------------------------- */
#define MULTIBOOT2_MAGIC      0x36D76289u
#define MB2_TAG_TYPE_MMAP     6u
#define MB2_TAG_TYPE_END      0u

/* ---------------------------------------------------------------
 * Linker-exported kernel image bounds
 * --------------------------------------------------------------- */
extern uint8_t _kernel_start;
extern uint8_t _kernel_end;

/* ---------------------------------------------------------------
 * Heap size (2 MB)
 * --------------------------------------------------------------- */
#define HEAP_SIZE   (2u * 1024u * 1024u)

/* ---------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------- */
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

/* ---------------------------------------------------------------
 * find_mmap_tag — walk Multiboot2 tags to find the memory map
 * --------------------------------------------------------------- */
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

/* ---------------------------------------------------------------
 * #DE test handler (Phase 1.1 regression)
 * --------------------------------------------------------------- */
static void de_test_handler(interrupt_frame_t *frame)
{
    vga_puts_color(
        "  [ OK ] #DE handler fired — vector=0x",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK
    );
    vga_puthex(frame->int_num);
    vga_puts_color("  RIP=0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puthex(frame->rip);
    vga_putchar('\n');
    frame->rip += 3;   /* skip 3-byte `div rcx` (REX.W F7 F1) */
}

/* ---------------------------------------------------------------
 * IRQ0 — PIT timer tick
 *
 * Called from IDT vector 0x20 (IRQ0, routed via I/O APIC).
 * Must call apic_send_eoi() — NOT outb(0x20,0x20); the legacy
 * 8259 PIC is disabled since Phase 1.2.
 * --------------------------------------------------------------- */
static void pit_irq_handler(interrupt_frame_t *f)
{
    (void)f;
    pit_tick();
    apic_send_eoi();
}

/* ---------------------------------------------------------------
 * IRQ1 — PS/2 Keyboard
 * --------------------------------------------------------------- */
static void kbd_isr(interrupt_frame_t *f)
{
    (void)f;
    keyboard_handle_irq();
    apic_send_eoi();
}

/* ---------------------------------------------------------------
 * IRQ12 — PS/2 Mouse
 * --------------------------------------------------------------- */
static void mouse_isr(interrupt_frame_t *f)
{
    (void)f;
    mouse_handle_irq();
    apic_send_eoi();
}

/* ---------------------------------------------------------------
 * kernel_main
 *
 *   magic — RDI — 0x36D76289
 *   addr  — RSI — physical address of Multiboot2 info struct
 * --------------------------------------------------------------- */
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

    /* ---- Multiboot2 check ------------------------------------ */
    if (magic == MULTIBOOT2_MAGIC) {
        print_ok("Multiboot2 magic OK");
    } else {
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
    vga_puts_color(
        "  [TEST] Triggering #DE (divide-by-zero)...\n",
        VGA_COLOR_BROWN, VGA_COLOR_BLACK
    );
    __asm__ volatile (
        "xor %%rcx, %%rcx  \n"
        "div %%rcx         \n"
        ::: "rax", "rdx", "rcx"
    );
    print_ok("#DE test PASSED");
    idt_register_handler(0, 0);

    /* ---- VMM ------------------------------------------------- */
    vmm_init();
    print_ok("VMM: 4-level paging, 64 MB identity map");

    /* ---- PMM ------------------------------------------------- */
    if (magic == MULTIBOOT2_MAGIC) {
        uint32_t mmap_size = 0;
        uint64_t mmap_tag  = find_mmap_tag(addr, &mmap_size);
        if (!mmap_tag) {
            print_fail("MB2 mmap tag not found");
        }
        uint64_t kstart = (uint64_t)(uintptr_t)&_kernel_start;
        uint64_t kend   = (uint64_t)(uintptr_t)&_kernel_end;
        pmm_init(mmap_tag, mmap_size, kstart, kend);
        print_ok("PMM: bitmap allocator ready");
    } else {
        print_warn("PMM skipped (no MB2 mmap) — heap uses static region");
    }

    /* ---- Heap ------------------------------------------------ */
    uint64_t kend_aligned = ((uint64_t)(uintptr_t)&_kernel_end
                             + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    heap_init(kend_aligned, HEAP_SIZE);
    print_ok("Heap: 2 MB kernel heap ready");

    /* ---- APIC ----------------------------------------------- */
    apic_init();
    print_ok("APIC: legacy PIC dead, LAPIC + I/O APIC active");

    /* ---- PIT — wire BEFORE sti ------------------------------ */
    /*
     * Order matters:
     *   1. apic_init() has already routed IRQ0 → vector 0x20.
     *   2. We register the IDT handler for 0x20.
     *   3. pit_init() programs PIT channel 0.  The chip will
     *      start firing IRQ0 immediately after this call, but
     *      interrupts are still masked (CLI), so no spurious
     *      tick fires before the IDT entry is installed.
     *   4. STI below unmasks everything.
     */
    idt_register_handler(0x20, pit_irq_handler);
    pit_init(1000);          /* 1000 Hz → 1 ms per tick        */
    print_ok("PIT: channel 0 at 1000 Hz, IRQ0 → vec 0x20");

    /* ---- Route keyboard + mouse via IOAPIC ------------------- */
    ioapic_route(1,  0x21, 0);   /* IRQ1  keyboard → IDT[33] */
    ioapic_route(12, 0x2C, 0);   /* IRQ12 mouse    → IDT[44] */

    idt_register_handler(0x21, kbd_isr);
    keyboard_init();
    print_ok("Keyboard: APIC IRQ1 → vec 0x21");

    idt_register_handler(0x2C, mouse_isr);
    mouse_init();
    print_ok("Mouse: APIC IRQ12 → vec 0x2C");

    /* ---- Enable interrupts — PIT starts ticking ------------- */
    __asm__ volatile ("sti");
    print_ok("Interrupts enabled (STI) — PIT ticking");

    /* ---- Phase 1.3 smoke-test: tick counter for 1 second ---- */
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

    /*
     * Accept 900–1100 ticks.  Exact 1000 is unlikely due to PIT
     * rounding (divisor = 1193182 / 1000 = 1193, actual rate
     * ≈ 999.8 Hz) and the time spent in initialization above.
     */
    if (elapsed >= 900ULL && elapsed <= 1100ULL) {
        print_ok("PIT tick test PASSED (expected ~1000)");
    } else if (elapsed > 0) {
        vga_puts_color("[WARN] Tick count out of range — check APIC/IRQ0 routing\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    } else {
        print_fail("PIT tick test FAILED — ticks=0, IRQ0 not firing");
    }

    vga_putchar('\n');
    vga_puts_color("AIOS Phase 1.3 boot complete.  Waiting for input...\n",
                   VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);

    for (;;) __asm__ volatile ("hlt");
}

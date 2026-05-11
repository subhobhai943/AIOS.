/* ============================================================
 * AIOS — Kernel Main
 * Phase 1.5 + 2.2 update:
 *   - serial_init(COM1, 115200) called early; panic mirrors to serial
 *   - Page fault handler (#PF, vector 14) installed before STI
 *   - Phase banner updated
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
#include "include/panic.h"
#include "apic.h"
#include "serial.h"

#include <stdint.h>

#define MULTIBOOT2_MAGIC      0x36D76289u
#define MB2_TAG_TYPE_MMAP     6u
#define MB2_TAG_TYPE_END      0u

extern uint8_t _kernel_start;
extern uint8_t _kernel_end;

#define HEAP_SIZE   (2u * 1024u * 1024u)

/* ── Forward declarations for handlers defined in other TUs ─ */
extern void pf_handler(interrupt_frame_t *frame);

/* ── Helpers ─────────────────────────────────────────────── */
static void print_ok(const char *msg)
{
    vga_puts_color("[ OK ] ", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts(msg);
    vga_putchar('\n');
    klog("[ OK ] "); klog(msg); klog("\r\n");
}

static void print_warn(const char *msg)
{
    vga_puts_color("[WARN] ", VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    vga_puts(msg);
    vga_putchar('\n');
    klog("[WARN] "); klog(msg); klog("\r\n");
}

static void print_fail(const char *msg)
{
    vga_puts_color("[FAIL] ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_puts(msg);
    vga_putchar('\n');
    kernel_panic(msg);   /* noreturn */
}

/* ── Multiboot2 mmap tag scanner ─────────────────────────── */
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

/* ── #DE regression test handler ─────────────────────────── */
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

/* ── IRQ handlers ─────────────────────────────────────────── */
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

/* ============================================================ */
void kernel_main(uint32_t magic, uint32_t addr)
{
    /* ---- VGA init (no serial yet — need screen first) ------- */
    vga_init();

    /* ---- Serial COM1 @ 115200 baud -------------------------- */
    serial_init(SERIAL_COM1, 115200);
    /* serial_init() prints its own "Serial online" banner */

    vga_puts_color(
        "====================================================\n",
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts_color(
        "  AIOS  Autonomous Intelligent Operating System\n",
        VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts_color(
        "  Phase 1.5/2.2: Serial + Page Fault Handler\n",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts_color(
        "====================================================\n\n",
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);

    klog("\r\n=== AIOS boot ===\r\n");

    /* ---- Multiboot2 magic check ----------------------------- */
    if (magic == MULTIBOOT2_MAGIC)
        print_ok("Multiboot2 magic OK");
    else {
        vga_puts_color("[WARN] MB2 magic mismatch: got 0x",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
        vga_puthex((uint64_t)magic);
        vga_puts_color(" expected 0x36D76289\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
        print_warn("Continuing without valid Multiboot2 info");
    }

    /* ---- GDT ------------------------------------------------- */
    gdt_init();
    print_ok("GDT: null/kcode/kdata/ucode/udata + TSS");

    /* ---- IDT ------------------------------------------------- */
    idt_init();
    print_ok("IDT: 256 gates, exception dump active");

    /* ---- Page fault handler (#PF = vector 14) --------------- */
    idt_register_handler(14, pf_handler);
    print_ok("#PF handler: faulting addr + error-code decode → panic");

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

    /* ---- Heap ----------------------------------------------- */
    uint64_t kend_aligned = ((uint64_t)(uintptr_t)&_kernel_end
                             + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    heap_init(kend_aligned, HEAP_SIZE);
    print_ok("Heap: 2 MB kernel heap ready");

    /* ---- Heap smoke-test (Phase 2.3) ------------------------- */
    {
        char *p = (char *)kmalloc(64);
        KERNEL_ASSERT(p != 0, "kmalloc(64) returned NULL");
        /* write a pattern */
        for (int i = 0; i < 64; i++) p[i] = (char)(i ^ 0xA5);
        /* verify */
        for (int i = 0; i < 64; i++)
            KERNEL_ASSERT((uint8_t)p[i] == (uint8_t)(i ^ 0xA5),
                          "heap write/read mismatch");
        kfree(p);
        print_ok("Heap smoke-test: kmalloc/write/verify/kfree OK");
    }

    /* ---- APIC ----------------------------------------------- */
    apic_init();
    print_ok("APIC: legacy PIC dead, LAPIC active");

    /* ---- IRQ routing (always after IDT handler install) ------ */

    /* IRQ0 — PIT timer */
    idt_register_handler(0x20, pit_irq_handler);
    ioapic_route(0, 0x20, 0);
    pit_init(1000);
    print_ok("PIT: 1000 Hz, IRQ0 → vec 0x20");

    /* IRQ1 — PS/2 keyboard */
    idt_register_handler(0x21, kbd_isr);
    ioapic_route(1, 0x21, 0);
    keyboard_init();
    print_ok("Keyboard: IRQ1 → vec 0x21");

    /* IRQ12 — PS/2 mouse */
    idt_register_handler(0x2C, mouse_isr);
    ioapic_route(12, 0x2C, 0);
    mouse_init();
    print_ok("Mouse: IRQ12 → vec 0x2C");

    /* ---- Enable interrupts ----------------------------------- */
    __asm__ volatile ("sti");
    print_ok("Interrupts enabled (STI) — PIT ticking");

    /* ---- Phase 1.3 PIT smoke-test ---------------------------- */
    vga_puts_color(
        "\n  [TEST] Waiting 1 second via pit_sleep_ms(1000)...\n",
        VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    klog("[TEST] pit_sleep_ms(1000)...\r\n");

    uint64_t t0 = pit_get_ticks();
    pit_sleep_ms(1000);
    uint64_t t1 = pit_get_ticks();
    uint64_t elapsed = t1 - t0;

    vga_puts_color("  [TEST] Ticks elapsed: ",
                   VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    vga_putdec(elapsed);
    vga_putchar('\n');
    klog("[TEST] Ticks elapsed: "); klog_dec(elapsed); klog("\r\n");

    if (elapsed >= 900ULL && elapsed <= 1100ULL)
        print_ok("PIT tick test PASSED (~1000 ticks in 1 second)");
    else if (elapsed > 0)
        print_warn("Tick count out of range — check APIC/IRQ0 routing");
    else
        print_fail("PIT tick test FAILED — ticks=0, IRQ0 not firing");

    vga_putchar('\n');
    vga_puts_color(
        "AIOS Phase 1.5/2.2 boot complete.  Waiting for input...\n",
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    klog("Boot complete. Halting (waiting for interrupts).\r\n");

    for (;;) __asm__ volatile ("hlt");
}

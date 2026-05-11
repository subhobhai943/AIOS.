/* ============================================================
 * AIOS — Kernel Main
 * Phase 3.1 + 3.2 update:
 *   - pci_init() / pci_dump() wired after heap
 *   - ahci_init() + ahci_sector0_test() added
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
#include "pci.h"
#include "ahci.h"

#include <stdint.h>

#define MULTIBOOT2_MAGIC      0x36D76289u
#define MB2_TAG_TYPE_MMAP     6u
#define MB2_TAG_TYPE_END      0u

extern uint8_t _kernel_start;
extern uint8_t _kernel_end;

#define HEAP_SIZE   (2u * 1024u * 1024u)

extern void pf_handler(interrupt_frame_t *frame);

/* ── Helpers ─────────────────────────────────────────────── */
static void print_ok(const char *msg)
{
    vga_puts_color("[ OK ] ", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts(msg); vga_putchar('\n');
    klog("[ OK ] "); klog(msg); klog("\r\n");
}
static void print_warn(const char *msg)
{
    vga_puts_color("[WARN] ", VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    vga_puts(msg); vga_putchar('\n');
    klog("[WARN] "); klog(msg); klog("\r\n");
}
static void print_fail(const char *msg)
{
    vga_puts_color("[FAIL] ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_puts(msg); vga_putchar('\n');
    kernel_panic(msg);
}

/* ── MB2 mmap scanner ─────────────────────────────────────── */
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
    vga_puthex(frame->int_num); vga_puts_color("  RIP=0x",
                   VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puthex(frame->rip); vga_putchar('\n');
    frame->rip += 3;
}

/* ── IRQ handlers ─────────────────────────────────────────── */
static void pit_irq_handler(interrupt_frame_t *f)
{
    (void)f; pit_tick(); apic_send_eoi();
}
static void kbd_isr(interrupt_frame_t *f)
{
    (void)f; keyboard_handle_irq(); apic_send_eoi();
}
static void mouse_isr(interrupt_frame_t *f)
{
    (void)f; mouse_handle_irq(); apic_send_eoi();
}

/* ============================================================ */
void kernel_main(uint32_t magic, uint32_t addr)
{
    vga_init();
    serial_init(SERIAL_COM1, 115200);

    vga_puts_color(
        "====================================================\n",
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts_color(
        "  AIOS  Autonomous Intelligent Operating System\n",
        VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts_color(
        "  Phase 3: PCI Enumeration + AHCI SATA Driver\n",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts_color(
        "====================================================\n\n",
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    klog("\r\n=== AIOS Phase 3 boot ===\r\n");

    /* ---- Multiboot2 ----------------------------------------- */
    if (magic == MULTIBOOT2_MAGIC)
        print_ok("Multiboot2 magic OK");
    else
        print_warn("MB2 magic mismatch — continuing");

    /* ---- GDT ------------------------------------------------- */
    gdt_init();
    print_ok("GDT: null/kcode/kdata/ucode/udata + TSS");

    /* ---- IDT ------------------------------------------------- */
    idt_init();
    print_ok("IDT: 256 gates, exception dump active");

    /* ---- Page fault handler ---------------------------------- */
    idt_register_handler(14, pf_handler);
    print_ok("#PF handler installed");

    /* ---- #DE regression ------------------------------------ */
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

    /* Heap smoke-test */
    {
        char *p = (char *)kmalloc(64);
        KERNEL_ASSERT(p != 0, "kmalloc(64) returned NULL");
        for (int i = 0; i < 64; i++) p[i] = (char)(i ^ 0xA5);
        for (int i = 0; i < 64; i++)
            KERNEL_ASSERT((uint8_t)p[i] == (uint8_t)(i ^ 0xA5),
                          "heap write/read mismatch");
        kfree(p);
        print_ok("Heap smoke-test: kmalloc/write/verify/kfree OK");
    }

    /* ---- APIC ----------------------------------------------- */
    apic_init();
    print_ok("APIC: legacy PIC dead, LAPIC active");

    /* ---- IRQ routing ---------------------------------------- */
    idt_register_handler(0x20, pit_irq_handler);
    ioapic_route(0, 0x20, 0);
    pit_init(1000);
    print_ok("PIT: 1000 Hz, IRQ0 → vec 0x20");

    idt_register_handler(0x21, kbd_isr);
    ioapic_route(1, 0x21, 0);
    keyboard_init();
    print_ok("Keyboard: IRQ1 → vec 0x21");

    idt_register_handler(0x2C, mouse_isr);
    ioapic_route(12, 0x2C, 0);
    mouse_init();
    print_ok("Mouse: IRQ12 → vec 0x2C");

    __asm__ volatile ("sti");
    print_ok("Interrupts enabled (STI) — PIT ticking");

    /* ---- Phase 1.3 PIT smoke-test ---------------------------- */
    vga_puts_color("\n  [TEST] pit_sleep_ms(1000)...\n",
                   VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    uint64_t t0 = pit_get_ticks();
    pit_sleep_ms(1000);
    uint64_t elapsed = pit_get_ticks() - t0;
    vga_puts_color("  [TEST] Ticks: ", VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    vga_putdec(elapsed); vga_putchar('\n');
    if (elapsed >= 900ULL && elapsed <= 1100ULL)
        print_ok("PIT tick test PASSED");
    else if (elapsed > 0)
        print_warn("Tick count out of range");
    else
        print_fail("PIT tick test FAILED — ticks=0");

    /* ===========================================================
     * Phase 3.1 — PCI Enumeration
     * =========================================================== */
    vga_putchar('\n');
    vga_puts_color("--- Phase 3.1: PCI Enumeration ---\n",
                   VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    pci_init();   /* enumerates all buses, prints count */
    pci_dump();   /* prints each device to VGA */
    print_ok("PCI enumeration complete");

    /* ===========================================================
     * Phase 3.2 — AHCI / SATA
     * =========================================================== */
    vga_puts_color("--- Phase 3.2: AHCI SATA Driver ---\n",
                   VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    if (ahci_init() == 0) {
        /* Smoke-test: read sector 0 from the first available port */
        for (int p = 0; p < 32; p++) {
            if (ahci_port_available(p)) {
                ahci_sector0_test(p);
                break;   /* test one port is enough */
            }
        }
        print_ok("AHCI driver initialised");
    } else {
        print_warn("AHCI init failed or no controller present");
    }

    vga_putchar('\n');
    vga_puts_color(
        "AIOS Phase 3 boot complete.  Waiting for input...\n",
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    klog("Phase 3 boot complete.\r\n");

    for (;;) __asm__ volatile ("hlt");
}

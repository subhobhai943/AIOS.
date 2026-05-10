/* ============================================================
 * AIOS — Kernel Main
 * Phase 1.2 : APIC — legacy PIC disabled, Local APIC + IOAPIC
 *
 * Argument layout (System V AMD64 ABI):
 *   RDI = multiboot2 boot magic  (0x36D76289)
 *   RSI = multiboot2 info struct physical address
 *
 * FIX (May 2026): kernel_entry.asm previously passed only RDI
 * (the info pointer).  The magic was never forwarded, so the
 * MB2 check always failed, PMM was skipped (total_frames = 0),
 * pmm_alloc_page() returned PMM_ALLOC_FAIL, and vmm_init()
 * halted with "out of memory for PML4".
 *
 * Fix applied in two places:
 *   1. boot/kernel_entry.asm — saves EAX (magic) and EBX
 *      (info ptr) immediately at entry, then loads both into
 *      RDI and RSI before calling kernel_main.
 *   2. kernel/vmm.c — added a 640 KB static BSS page-table
 *      pool so vmm_init() works even without PMM (defensive).
 * ============================================================ */

#include "include/vga.h"
#include "include/gdt.h"
#include "include/idt.h"
#include "include/pmm.h"
#include "include/vmm.h"
#include "include/heap.h"
#include "include/keyboard.h"
#include "include/mouse.h"
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
 * Heap size
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
 * find_mmap_tag
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
 * IRQ handlers — all MUST call apic_send_eoi() at the end.
 * --------------------------------------------------------------- */
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

/* ---------------------------------------------------------------
 * kernel_main
 *
 * Parameters (System V AMD64 ABI):
 *   magic — RDI — should equal MULTIBOOT2_MAGIC (0x36D76289)
 *   addr  — RSI — physical address of multiboot2 info struct
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
        "  Phase 1.2 : Local APIC + I/O APIC\n",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts_color(
        "====================================================\n\n",
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);

    /* ---- Multiboot2 check ------------------------------------ */
    if (magic == MULTIBOOT2_MAGIC) {
        print_ok("Multiboot2 magic OK");
    } else {
        /* Print the actual magic we received for debugging */
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
    /* NOTE: vmm_init() is called BEFORE pmm_init().
     * If MB2 mmap is missing, vmm_init() uses the static BSS pool.
     * If MB2 mmap is present, pmm_alloc_page() is not yet ready
     * at this point — vmm_init() still uses the static pool for
     * the initial page-table pages, then PMM takes over for all
     * subsequent vmm_map_page() calls after pmm_init() below. */
    vmm_init();

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
    print_ok("APIC: legacy PIC dead, LAPIC+IOAPIC active");

    /* ---- Route keyboard + mouse via IOAPIC ------------------- */
    ioapic_route(1,  0x21, 0);   /* IRQ1  keyboard → IDT[33] */
    ioapic_route(12, 0x2C, 0);   /* IRQ12 mouse    → IDT[44] */

    idt_register_handler(0x21, kbd_isr);
    keyboard_init();
    print_ok("Keyboard: APIC IRQ1 → vec 0x21");

    idt_register_handler(0x2C, mouse_isr);
    mouse_init();
    print_ok("Mouse: APIC IRQ12 → vec 0x2C");

    /* ---- Enable interrupts ----------------------------------- */
    __asm__ volatile ("sti");
    print_ok("Interrupts enabled (STI)");

    vga_putchar('\n');
    vga_puts_color("AIOS boot complete. Waiting for input...\n",
                   VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);

    for (;;) __asm__ volatile ("hlt");
}

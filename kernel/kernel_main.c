/* ============================================================
 * AIOS — Kernel Main
 * Phase 1.1 : IDT — exception dump, idt_flush(), #DE test
 * (Phase 0.4 memory management still intact)
 * ============================================================ */

#include "include/vga.h"
#include "include/gdt.h"
#include "include/idt.h"
#include "include/pmm.h"
#include "include/vmm.h"
#include "include/heap.h"
#include "include/keyboard.h"
#include "include/mouse.h"

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
 * Heap location and size
 * --------------------------------------------------------------- */
#define HEAP_SIZE   (2u * 1024u * 1024u)

/* ---------------------------------------------------------------
 * PIC I/O helpers
 * --------------------------------------------------------------- */
#define PIC1_DATA 0x21u
#define PIC2_DATA 0xA1u

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

/* ---------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------- */
static void print_ok(const char *msg)
{
    vga_puts_color("[ OK ] ", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
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
 * #DE TEST — custom handler for vector 0 (divide-by-zero)
 *
 * The CPU delivers #DE when DIV/IDIV divide by zero.  This handler
 * prints a success banner and then RETURNS so the kernel continues
 * booting — the handler adjusts RIP past the faulting instruction
 * by adding 2 (the size of `div rcx` / `idiv` variants varies, but
 * we just skip by setting RIP = RIP+2; since this is a controlled
 * test inside kernel_main that is safe enough).
 *
 * In a real scenario you'd panic; here the point is to confirm the
 * IDT fires without a triple-fault.
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

    /* Advance RIP past the faulting `div` instruction.
     * `div rcx` encodes as 48 F7 F1 (3 bytes in 64-bit mode).
     * We rely on the test below using exactly that form. */
    frame->rip += 3;
}

/* ---------------------------------------------------------------
 * ISR callbacks
 * --------------------------------------------------------------- */
static void kbd_isr  (interrupt_frame_t *f) { (void)f; keyboard_handle_irq(); }
static void mouse_isr(interrupt_frame_t *f) { (void)f; mouse_handle_irq(); }

/* ---------------------------------------------------------------
 * kernel_main
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
        "  Phase 1.1 : IDT + Exception Handling\n",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts_color(
        "====================================================\n\n",
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);

    /* ---- Multiboot2 check ------------------------------------ */
    if (magic == MULTIBOOT2_MAGIC) {
        print_ok("Multiboot2 magic OK");
    } else {
        vga_puts_color("[WARN] Not booted via Multiboot2 — "
                       "memory map unavailable\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    }

    /* ---- GDT ------------------------------------------------- */
    gdt_init();
    print_ok("GDT: null/kcode/kdata/ucode/udata + TSS");

    /* ---- IDT ------------------------------------------------- */
    idt_init();
    print_ok("IDT: 256 gates, PIC remapped, exception dump active");

    /* ---- #DE Test -------------------------------------------- */
    /*
     * Register a temporary handler for vector 0 (#DE) that
     * acknowledges the exception and advances RIP so we continue.
     * After the test we restore the default (NULL = panic dump).
     */
    idt_register_handler(0, de_test_handler);

    vga_puts_color(
        "  [TEST] Triggering divide-by-zero (#DE)...\n",
        VGA_COLOR_BROWN, VGA_COLOR_BLACK
    );

    /*
     * Inline assembly: set RCX=0, then execute `div rcx` (REX.W
     * DIV r/m64 = 48 F7 F1).  This raises #DE immediately.
     * The de_test_handler advances RIP by 3 so execution resumes
     * at the instruction after `div rcx`.
     */
    __asm__ volatile (
        "xor %%rcx, %%rcx  \n"
        "div %%rcx         \n"   /* 48 F7 F1 — raises #DE */
        :
        :
        : "rax", "rdx", "rcx"
    );

    print_ok("#DE test PASSED — IDT catches exception, returns to caller");

    /* Remove the test handler; from here on #DE → panic dump */
    idt_register_handler(0, 0);

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
        vga_puts_color("[WARN] PMM skipped (no MB2 mmap)\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    }

    /* ---- VMM ------------------------------------------------- */
    vmm_init();
    print_ok("VMM: 4-level paging, identity mapped first 64 MB");

    /* ---- Heap ------------------------------------------------ */
    uint64_t kend_aligned = ((uint64_t)(uintptr_t)&_kernel_end
                             + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    heap_init(kend_aligned, HEAP_SIZE);
    print_ok("Heap: 2 MB kernel heap ready");

    /* ---- Keyboard -------------------------------------------- */
    idt_register_handler(0x21, kbd_isr);
    keyboard_init();
    print_ok("Keyboard driver ready");

    /* ---- Mouse ----------------------------------------------- */
    idt_register_handler(0x2C, mouse_isr);
    mouse_init();
    print_ok("Mouse driver ready");

    /* ---- Unmask PIC IRQs ------------------------------------- */
    uint8_t m1 = inb(PIC1_DATA);
    uint8_t m2 = inb(PIC2_DATA);
    m1 &= ~(1u << 1);   /* IRQ1  keyboard */
    m1 &= ~(1u << 2);   /* IRQ2  cascade  */
    m2 &= ~(1u << 4);   /* IRQ12 mouse    */
    outb(PIC1_DATA, m1);
    outb(PIC2_DATA, m2);
    print_ok("PIC IRQ1/IRQ2/IRQ12 unmasked");

    /* ---- Enable interrupts ----------------------------------- */
    __asm__ volatile ("sti");
    print_ok("Interrupts enabled (STI)");

    vga_putchar('\n');
    vga_puts_color("Keyboard + Mouse active. Type something...\n",
                   VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);

    for (;;) __asm__ volatile ("hlt");
}

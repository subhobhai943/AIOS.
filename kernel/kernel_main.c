/* ============================================================
 * AIOS — Kernel Main
 * Phase 0.4 : Memory Management (PMM + VMM + Heap)
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
 * (defined in the linker script as PROVIDE(_kernel_start / _end))
 * --------------------------------------------------------------- */
extern uint8_t _kernel_start;
extern uint8_t _kernel_end;

/* ---------------------------------------------------------------
 * Heap location: place it right after the kernel image,
 * rounded up to the next page, inside the identity-mapped region.
 * Size: 2 MB — plenty for early kernel use.
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
 * find_mmap_tag — walk the Multiboot2 info structure and return
 * the virtual address of the memory-map tag (type 6), or 0.
 *
 * MB2 info layout:
 *   uint32_t total_size
 *   uint32_t reserved
 *   tag[0], tag[1], ...   each 8-byte aligned
 * Each tag:
 *   uint32_t type
 *   uint32_t size
 *   ... payload ...
 * --------------------------------------------------------------- */
static uint64_t find_mmap_tag(uint32_t mb2_addr, uint32_t *tag_size_out)
{
    uint32_t total = *(uint32_t *)(uintptr_t)mb2_addr;
    uint32_t off   = 8;   /* skip total_size + reserved */

    while (off < total) {
        uint32_t type = *(uint32_t *)(uintptr_t)(mb2_addr + off);
        uint32_t size = *(uint32_t *)(uintptr_t)(mb2_addr + off + 4);

        if (type == MB2_TAG_TYPE_END) break;

        if (type == MB2_TAG_TYPE_MMAP) {
            if (tag_size_out) *tag_size_out = size;
            return (uint64_t)(mb2_addr + off);
        }

        /* Tags are 8-byte aligned */
        off += (size + 7u) & ~7u;
    }
    return 0;
}

/* ---------------------------------------------------------------
 * ISR callbacks
 * --------------------------------------------------------------- */
static void kbd_isr  (interrupt_frame_t *f) { (void)f; keyboard_handle_irq(); }
static void mouse_isr(interrupt_frame_t *f) { (void)f; mouse_handle_irq(); }

/* ---------------------------------------------------------------
 * kernel_main
 *
 * magic : EAX value set by the Multiboot2 bootloader
 * addr  : EBX value — physical address of MB2 info structure
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
        "  Phase 0.4 : Memory Management\n",
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
    print_ok("CS reloaded (lretq), TSS loaded (ltr 0x28)");

    /* ---- IDT ------------------------------------------------- */
    idt_init();
    print_ok("IDT loaded");

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
    /*
     * Place the heap immediately after the kernel image, rounded
     * up to the next page boundary.  It stays inside the 64 MB
     * identity-mapped window so virtual == physical here.
     */
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

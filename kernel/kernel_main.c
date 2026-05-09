/* ============================================================
 * AIOS — Kernel Main
 * ============================================================ */

#include "include/vga.h"
#include "include/gdt.h"
#include "include/idt.h"
#include "include/keyboard.h"
#include "include/mouse.h"

#include <stdint.h>

#define MULTIBOOT2_MAGIC 0x36D76289

#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ------------------------------------------------------------ */

static void print_ok(const char *msg)
{
    vga_puts_color("[ OK ] ", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts(msg);
    vga_putchar('\n');
}

/* ------------------------------------------------------------ */

static void kbd_isr(interrupt_frame_t *frame)
{
    (void)frame;
    keyboard_handle_irq();
}

static void mouse_isr(interrupt_frame_t *frame)
{
    (void)frame;
    mouse_handle_irq();
}

/* ------------------------------------------------------------ */

void kernel_main(uint32_t magic, uint32_t addr)
{
    (void)addr;

    vga_init();

    vga_puts_color(
        "====================================================\n",
        VGA_COLOR_LIGHT_CYAN,
        VGA_COLOR_BLACK
    );
    vga_puts_color(
        " AIOS  Autonomous Intelligent Operating System\n",
        VGA_COLOR_WHITE,
        VGA_COLOR_BLACK
    );
    vga_puts_color(
        " Phase 0.3 : GDT + TSS\n",
        VGA_COLOR_LIGHT_GREEN,
        VGA_COLOR_BLACK
    );
    vga_puts_color(
        "====================================================\n\n",
        VGA_COLOR_LIGHT_CYAN,
        VGA_COLOR_BLACK
    );

    /* -------------------------------------------------------- */
    /* Multiboot2 magic check */

    if (magic == MULTIBOOT2_MAGIC) {
        print_ok("Multiboot2 magic verified");
    } else {
        vga_puts_color(
            "[WARN] Not booted via Multiboot2\n",
            VGA_COLOR_BROWN,
            VGA_COLOR_BLACK
        );
    }

    /* -------------------------------------------------------- */
    /* GDT: null / kcode / kdata / ucode / udata / TSS
     *
     *  gdt_init() internally calls gdt_flush() (lgdt + far-return
     *  to reload CS=0x08) and tss_load() (ltr).
     *  If we reach print_ok below, no triple-fault occurred.      */

    gdt_init();
    print_ok("GDT loaded  (null/kcode/kdata/ucode/udata + TSS)");
    print_ok("CS reloaded via far-return (lretq)");
    print_ok("TSS loaded  (ltr  selector=0x28)");

    /* -------------------------------------------------------- */
    /* IDT */

    idt_init();
    print_ok("IDT loaded");

    /* -------------------------------------------------------- */
    /* Keyboard + Mouse */

    idt_register_handler(0x21, kbd_isr);
    keyboard_init();
    print_ok("Keyboard driver initialised");

    idt_register_handler(0x2C, mouse_isr);
    mouse_init();
    print_ok("Mouse driver initialised");

    /* -------------------------------------------------------- */
    /* Unmask PIC IRQ1 (keyboard) + cascade IRQ2 + IRQ12 (mouse) */

    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);
    mask1 &= ~(1u << 1);   /* IRQ1  — keyboard     */
    mask1 &= ~(1u << 2);   /* IRQ2  — cascade line */
    mask2 &= ~(1u << 4);   /* IRQ12 — mouse        */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
    print_ok("PIC IRQ1/IRQ2/IRQ12 unmasked");

    /* -------------------------------------------------------- */
    /* Enable interrupts */

    __asm__ volatile ("sti");
    print_ok("Interrupts enabled (STI)");

    /* -------------------------------------------------------- */

    vga_putchar('\n');
    vga_puts_color(
        "Keyboard + Mouse active.  Type something...\n",
        VGA_COLOR_LIGHT_CYAN,
        VGA_COLOR_BLACK
    );

    /* -------------------------------------------------------- */
    /* Idle loop */

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

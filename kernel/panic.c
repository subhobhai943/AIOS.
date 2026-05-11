/* ============================================================
 * AIOS — Kernel Panic
 *
 * Outputs to VGA (bright red) AND COM1 serial so the message
 * is visible both on-screen and in the QEMU console when
 * launched with:  qemu-system-x86_64 ... -serial stdio
 *
 * Does NOT depend on the heap or any late-init subsystem —
 * only vga.c and serial.c (which are always initialised first
 * in kernel_main).  Safe to call from any interrupt context.
 * ============================================================ */

#include "include/panic.h"
#include "include/vga.h"
#include "serial.h"
#include <stdint.h>

/* ─── Horizontal rule helpers ─────────────────────────────── */
static void vga_rule(void)
{
    vga_puts_color(
        "======================================================\n",
        VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
}

/* ─── kernel_panic ────────────────────────────────────────── */
__attribute__((noreturn))
void kernel_panic(const char *msg)
{
    /* Disable interrupts immediately — we do not want a timer
     * tick or another IRQ to preempt us mid-panic. */
    __asm__ volatile ("cli");

    /* ── VGA output ─────────────────────────────────────── */
    vga_puts_color("\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_rule();
    vga_puts_color("  *** KERNEL PANIC ***\n",
                   VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts_color("  ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_puts_color(msg, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_puts_color("\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_rule();
    vga_puts_color("  System halted.  Reboot to continue.\n",
                   VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    /* ── Serial output (COM1) ───────────────────────────── */
    serial_puts(SERIAL_COM1, "\r\n=== KERNEL PANIC ===\r\n");
    serial_puts(SERIAL_COM1, msg);
    serial_puts(SERIAL_COM1, "\r\nSystem halted.\r\n");

    /* ── Halt forever ───────────────────────────────────── */
    for (;;)
        __asm__ volatile ("hlt");
}

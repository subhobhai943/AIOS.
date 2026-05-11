/* ============================================================
 * AIOS — Page Fault Handler  (#PF, vector 14)
 *
 * Phase 2.2 — "Page fault handler: print faulting address +
 * error code, then panic or COW"
 *
 * Reads CR2 (the linear address that caused the fault) and
 * decodes the error-code bits, prints a human-readable report
 * to both VGA and COM1 serial, then calls kernel_panic().
 *
 * Error-code bit layout (Intel SDM Vol 3A §6.15):
 *   Bit 0  P   — 0=non-present page, 1=protection violation
 *   Bit 1  W/R — 0=read, 1=write
 *   Bit 2  U/S — 0=supervisor, 1=user
 *   Bit 3  RSVD— 1=reserved bit set in page-table entry
 *   Bit 4  I/D — 1=instruction fetch (NX violation)
 *   Bit 5  PK  — 1=protection-key violation
 *   Bit 6  SS  — 1=shadow-stack access fault
 * ============================================================ */

#include "include/panic.h"
#include "include/vga.h"
#include "include/idt.h"
#include "serial.h"
#include <stdint.h>

/* ─── read CR2 ───────────────────────────────────────────── */
static inline uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

/* ─── print a labelled hex value to serial ───────────────── */
static void ser_hex(const char *label, uint64_t val)
{
    serial_puts(SERIAL_COM1, label);
    serial_puthex(SERIAL_COM1, val);
    serial_puts(SERIAL_COM1, "\r\n");
}

/* ─── pf_handler ─────────────────────────────────────────── */
void pf_handler(interrupt_frame_t *frame)
{
    uint64_t cr2   = read_cr2();
    uint64_t ecode = frame->err_code;   /* pushed by CPU for #PF */

    /* ── VGA report ─────────────────────────────────────── */
    vga_puts_color("\n[#PF] PAGE FAULT\n",
                   VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);

    vga_puts_color("  Faulting address : 0x",
                   VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_puthex(cr2);
    vga_putchar('\n');

    vga_puts_color("  Error code       : 0x",
                   VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_puthex(ecode);
    vga_puts_color(" (", VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    /* Decode each bit in plain English */
    vga_puts_color((ecode & 1) ? "protection-violation" : "not-present",
                   VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    vga_puts_color(", ", VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts_color((ecode & 2) ? "write" : "read",
                   VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    vga_puts_color(", ", VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts_color((ecode & 4) ? "user" : "supervisor",
                   VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    if (ecode & 8)
        vga_puts_color(", RSVD-bit", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    if (ecode & 16)
        vga_puts_color(", NX-violation", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    if (ecode & 32)
        vga_puts_color(", PK-violation", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    if (ecode & 64)
        vga_puts_color(", shadow-stack", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_puts_color(")\n", VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    vga_puts_color("  RIP : 0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puthex(frame->rip);
    vga_puts_color("  CS  : 0x", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puthex(frame->cs);
    vga_putchar('\n');

    /* ── Serial report ──────────────────────────────────── */
    serial_puts(SERIAL_COM1, "\r\n[#PF] PAGE FAULT\r\n");
    ser_hex("  CR2 (fault addr) = ", cr2);
    ser_hex("  Error code       = ", ecode);
    ser_hex("  RIP              = ", frame->rip);
    ser_hex("  CS               = ", frame->cs);

    /* ── Panic — no recovery yet (COW in Phase 4) ───────── */
    kernel_panic("Unhandled page fault");
}

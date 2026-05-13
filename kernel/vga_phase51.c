/*
 * Phase 5.1 additions to the VGA driver.
 *
 * Include or merge into kernel/vga.c before building.
 * These three functions are separated here to keep the diff minimal.
 */

#include "vga.h"

/* VGA text buffer — must match the base address in vga.c */
#define VGA_BUF ((volatile uint16_t *)0xB8000)
#define VGA_COLS 80
#define VGA_ROWS 25

/* CRTC register ports */
#define CRTC_ADDR_PORT  0x3D4
#define CRTC_DATA_PORT  0x3D5

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/**
 * vga_set_cursor — move the blinking hardware cursor to (col, row).
 * Uses CRTC registers 0x0E (high byte) and 0x0F (low byte).
 */
void vga_set_cursor(uint8_t col, uint8_t row) {
    if (col >= VGA_COLS) col = VGA_COLS - 1;
    if (row >= VGA_ROWS) row = VGA_ROWS - 1;
    uint16_t pos = (uint16_t)(row * VGA_COLS + col);
    outb(CRTC_ADDR_PORT, 0x0E);
    outb(CRTC_DATA_PORT, (uint8_t)(pos >> 8));
    outb(CRTC_ADDR_PORT, 0x0F);
    outb(CRTC_DATA_PORT, (uint8_t)(pos & 0xFF));
}

/**
 * vga_get_cursor — read current cursor position from CRTC.
 */
void vga_get_cursor(uint8_t *col, uint8_t *row) {
    outb(CRTC_ADDR_PORT, 0x0E);
    uint16_t pos = (uint16_t)inb(CRTC_DATA_PORT) << 8;
    outb(CRTC_ADDR_PORT, 0x0F);
    pos |= inb(CRTC_DATA_PORT);
    if (col) *col = (uint8_t)(pos % VGA_COLS);
    if (row) *row = (uint8_t)(pos / VGA_COLS);
}

/**
 * vga_putchar_at — write character + attribute directly into the VGA
 * framebuffer at (col, row) WITHOUT moving the cursor.
 */
void vga_putchar_at(char c, uint8_t fg, uint8_t bg, uint8_t col, uint8_t row) {
    if (col >= VGA_COLS || row >= VGA_ROWS) return;
    uint16_t attr  = (uint16_t)((bg & 0x7) << 4 | (fg & 0xF)) << 8;
    uint16_t entry = attr | (uint16_t)(uint8_t)c;
    VGA_BUF[(uint32_t)row * VGA_COLS + col] = entry;
}

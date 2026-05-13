#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * VGA text-mode driver — public API
 *
 * Additions for Phase 5.1:
 *   vga_set_cursor(col, row)   — write hardware cursor position via CRTC
 *   vga_get_cursor(col, row)   — read back hardware cursor position
 *   vga_putchar_at(c,fg,bg,col,row) — write char+attr at an absolute cell
 *                                     without moving the cursor
 */

/* Colour constants (4-bit VGA attribute nibbles) */
#define VGA_BLACK    0
#define VGA_BLUE     1
#define VGA_GREEN    2
#define VGA_CYAN     3
#define VGA_RED      4
#define VGA_MAGENTA  5
#define VGA_BROWN    6
#define VGA_LGRAY    7
#define VGA_DGRAY    8
#define VGA_LBLUE    9
#define VGA_LGREEN   10
#define VGA_LCYAN    11
#define VGA_LRED     12
#define VGA_PINK     13
#define VGA_YELLOW   14
#define VGA_WHITE    15

/* Phase 0–4 API (unchanged) */
void vga_init(void);
void vga_clear(void);
void vga_putchar(char c);
void vga_puts(const char *str);
void vga_puts_color(const char *str, uint8_t fg, uint8_t bg);
void vga_set_color(uint8_t fg, uint8_t bg);

/* Phase 5.1 additions */
void vga_set_cursor(uint8_t col, uint8_t row);
void vga_get_cursor(uint8_t *col, uint8_t *row);
void vga_putchar_at(char c, uint8_t fg, uint8_t bg, uint8_t col, uint8_t row);

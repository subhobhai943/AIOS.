#ifndef VGA_H
#define VGA_H

/* ================================================================
 * AIOS — VGA Text-Mode Driver
 * Phase 1
 *
 * Provides:
 *   vga_init()       — 80×25 text mode init, hardware cursor enable
 *   vga_putchar(c)   — write character at cursor, handle \n \r \t
 *   vga_puts(str)    — null-terminated string
 *   vga_puthex(v)    — 64-bit hex (0x...)
 *   vga_putdec(v)    — 64-bit decimal
 *   vga_backspace()  — erase previous character (for readline)
 *   vga_clear()      — blank the screen, home cursor
 *   vga_set_color()  — set default fg/bg for subsequent chars
 *   vga_puts_color() — write string with explicit fg/bg
 *   vga_set_cursor_pos(col,row) — move cursor
 *   vga_putchar_at(c,fg,bg,col,row) — write char+attr at an absolute cell
 * ================================================================ */

#include <stdint.h>
#include <stddef.h>

/* VGA colour constants */
#define VGA_COLOR_BLACK          0u
#define VGA_COLOR_BLUE           1u
#define VGA_COLOR_GREEN          2u
#define VGA_COLOR_CYAN           3u
#define VGA_COLOR_RED            4u
#define VGA_COLOR_MAGENTA        5u
#define VGA_COLOR_BROWN          6u
#define VGA_COLOR_LIGHT_GREY     7u
#define VGA_COLOR_DARK_GREY      8u
#define VGA_COLOR_LIGHT_BLUE     9u
#define VGA_COLOR_LIGHT_GREEN   10u
#define VGA_COLOR_LIGHT_CYAN    11u
#define VGA_COLOR_LIGHT_RED     12u
#define VGA_COLOR_LIGHT_MAGENTA 13u
#define VGA_COLOR_LIGHT_BROWN   14u
#define VGA_COLOR_YELLOW        14u
#define VGA_COLOR_WHITE         15u

/* Screen dimensions */
#define VGA_WIDTH  80u
#define VGA_HEIGHT 25u

/* Public API */
void vga_init(void);
void vga_putchar(char c);
void vga_puts(const char *str);          /* alias: vga_puts(str)         */
void vga_puthex(uint64_t val);
void vga_putdec(uint64_t val);
void vga_backspace(void);                /* erase previous char (readline) */
void vga_clear(void);
void vga_set_color(uint8_t fg, uint8_t bg);
void vga_puts_color(const char *str, uint8_t fg, uint8_t bg);
void vga_set_cursor_pos(size_t col, size_t row);
void vga_putchar_at(char c, uint8_t fg, uint8_t bg, uint8_t col, uint8_t row);

/* Legacy alias used by some drivers */
#define vga_write(str) vga_puts(str)

#endif /* VGA_H */

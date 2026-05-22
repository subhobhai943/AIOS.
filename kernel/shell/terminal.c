/* kernel/shell/terminal.c — Phase 5.1
 *
 * Terminal layer: bridges the keyboard IRQ path to the shell readline.
 * No libc. No standard I/O. Only VGA and keyboard headers.
 */

#include "terminal.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Internal line buffer ────────────────────────────────── */
#define LINE_MAX 256

static char     g_line[LINE_MAX];
static size_t   g_line_len  = 0;
static volatile int g_line_ready = 0;   /* set to 1 by terminal_feed on Enter */

/* ── Init ────────────────────────────────────────────────── */
void terminal_init(void)
{
    g_line_len   = 0;
    g_line_ready = 0;
}

/* ── Feed — called from keyboard IRQ (text-mode path) ────── */
void terminal_feed(char ascii, uint8_t scancode, bool pressed)
{
    (void)scancode;
    if (!pressed) return;

    if (ascii == '\r' || ascii == '\n') {
        g_line[g_line_len] = '\0';
        g_line_ready = 1;
    } else if (ascii == '\b') {
        if (g_line_len > 0) {
            g_line_len--;
            vga_backspace();
        }
    } else if (ascii >= 0x20 && g_line_len < LINE_MAX - 1) {
        g_line[g_line_len++] = ascii;
        vga_putchar(ascii);
    }
}

/* ── Readline — blocks (hlt loop) until Enter pressed ────── */
size_t terminal_readline(char *buf, size_t maxlen)
{
    g_line_len   = 0;
    g_line_ready = 0;

    /* Install ourselves as the keyboard text-mode callback. */
    keyboard_set_text_callback(terminal_feed);

    while (!g_line_ready) {
        __asm__ volatile ("hlt");
    }

    size_t n = (g_line_len < maxlen - 1) ? g_line_len : maxlen - 1;
    for (size_t i = 0; i < n; i++)
        buf[i] = g_line[i];
    buf[n] = '\0';
    return n;
}

/* ── Write helpers ───────────────────────────────────────── */
void terminal_write(const char *str)
{
    while (str && *str)
        vga_putchar(*str++);
}

void terminal_write_len(const char *str, size_t len)
{
    for (size_t i = 0; i < len; i++)
        vga_putchar(str[i]);
}

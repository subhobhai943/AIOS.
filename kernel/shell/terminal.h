#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ================================================================
 * AIOS — Terminal layer (Phase 5.1)
 *
 * Sits between the VGA driver and the shell.  Provides:
 *  - A readline() that echoes characters, handles backspace,
 *    and returns when Enter is pressed.
 *  - terminal_write() / terminal_write_len() for kernel output.
 *  - terminal_feed() called from the keyboard IRQ path when GUI
 *    mode is inactive.
 * ================================================================ */

#define TERM_COLS 80
#define TERM_ROWS 25

/* One-time init — call from kernel_main before shell_run(). */
void terminal_init(void);

/* Called by the keyboard IRQ handler (or keyboard_set_gui_callback
 * fallback) to deliver a decoded keystroke to the terminal.
 * ascii == 0 means non-printable; scancode is always valid.
 */
void terminal_feed(char ascii, uint8_t scancode, bool pressed);

/* Blocking readline — waits for Enter then copies the line into buf.
 * Returns the number of characters placed in buf (excluding NUL).
 * Echoes characters and handles backspace.
 */
size_t terminal_readline(char *buf, size_t maxlen);

/* Non-blocking write helpers — forward to VGA driver. */
void terminal_write(const char *str);
void terminal_write_len(const char *str, size_t len);

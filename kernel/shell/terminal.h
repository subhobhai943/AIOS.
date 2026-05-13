#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * AIOS  —  Phase 5.1  Terminal Emulator
 *
 * Responsibilities:
 *   • Owns the input ring-buffer (keyboard ISR → terminal)
 *   • Line editing: backspace, ←/→, Home/End
 *   • History: 32 entries × 256 chars, cycled with ↑/↓
 *   • terminal_readline() — blocking, yields CPU while waiting
 *   • ANSI escape emitter helpers
 * --------------------------------------------------------------------- */

/* ── sizes ─────────────────────────────────────────────────────────── */
#define TERM_COLS           80
#define TERM_ROWS           25
#define TERM_INPUT_BUF      256   /* ring buffer capacity (must be 2^n) */
#define TERM_LINE_MAX       256   /* max editable line length            */
#define TERM_HISTORY_DEPTH  32    /* number of history slots             */
#define TERM_HISTORY_LEN    256   /* max chars per history entry         */

/* ── VGA colour aliases (same encoding as kernel/vga.h) ────────────── */
#define TERM_FG_BLACK   0
#define TERM_FG_BLUE    1
#define TERM_FG_GREEN   2
#define TERM_FG_CYAN    3
#define TERM_FG_RED     4
#define TERM_FG_MAGENTA 5
#define TERM_FG_BROWN   6
#define TERM_FG_LGRAY   7
#define TERM_FG_DGRAY   8
#define TERM_FG_LBLUE   9
#define TERM_FG_LGREEN  10
#define TERM_FG_LCYAN   11
#define TERM_FG_LRED    12
#define TERM_FG_PINK    13
#define TERM_FG_YELLOW  14
#define TERM_FG_WHITE   15
#define TERM_BG_BLACK   0

/* ── special virtual-key codes fed by the keyboard driver ───────────── */
/* These live above 0x7F so they never collide with printable ASCII.    */
#define TERM_KEY_BACKSPACE  0x08
#define TERM_KEY_ENTER      0x0A
#define TERM_KEY_UP         0x80
#define TERM_KEY_DOWN       0x81
#define TERM_KEY_LEFT       0x82
#define TERM_KEY_RIGHT      0x83
#define TERM_KEY_HOME       0x84
#define TERM_KEY_END        0x85
#define TERM_KEY_DEL        0x86
#define TERM_KEY_TAB        0x09

/* ── public API ─────────────────────────────────────────────────────── */

/**
 * terminal_init() — must be called once from kernel_main after sync
 * primitives are ready.  Clears the VGA screen, positions the cursor
 * at (0,0), and prints the AIOS banner.
 */
void terminal_init(void);

/**
 * terminal_feed(key) — called from the keyboard ISR (or keyboard task).
 * Pushes one virtual-key code into the lock-free SPSC ring buffer.
 * Safe to call from interrupt context.
 */
void terminal_feed(uint8_t key);

/**
 * terminal_readline(buf, maxlen) — blocking line editor.
 * Echoes characters to VGA as the user types.
 * Returns the number of bytes written into buf (NOT including NUL).
 * Yields the CPU (sched_yield) while the buffer is empty.
 */
size_t terminal_readline(char *buf, size_t maxlen);

/**
 * terminal_write(str) — output a NUL-terminated string via VGA,
 * honouring the current foreground colour.
 */
void terminal_write(const char *str);

/**
 * terminal_write_len(str, len) — same but length-bounded.
 */
void terminal_write_len(const char *str, size_t len);

/**
 * terminal_write_char(c) — output a single character.
 */
void terminal_write_char(char c);

/* ── ANSI-style emitter helpers ──────────────────────────────────────
 * These manipulate the VGA hardware cursor and colour registers
 * directly; they do NOT interpret real ANSI sequences sent to the
 * terminal — they *emit* positioning commands for the shell/AI to use.
 */

/** Move VGA hardware cursor to (col, row). Clamped to screen bounds. */
void term_move_cursor(uint8_t col, uint8_t row);

/** Set the VGA attribute byte used for subsequent vga_putchar calls. */
void term_set_color(uint8_t fg, uint8_t bg);

/** Reset colour to default (light-gray on black). */
void term_reset_color(void);

/** Erase from cursor to end-of-line (space-fill with current colour). */
void term_clear_line_to_end(void);

/** Erase entire current row. */
void term_clear_line(void);

/** Clear the entire screen and home the cursor. */
void term_clear_screen(void);

/** Return current cursor column (0-based). */
uint8_t term_cursor_col(void);

/** Return current cursor row (0-based). */
uint8_t term_cursor_row(void);

/* ── History API (also used by shell for ↑/↓ completion) ────────────── */

/** Push a completed line into the history ring. Empty lines ignored. */
void term_history_push(const char *line);

/**
 * term_history_get(offset, buf, maxlen)
 *   offset 0 = most-recent entry, 1 = one before that, …
 *   Returns false if offset ≥ number of stored entries.
 */
bool term_history_get(uint32_t offset, char *buf, size_t maxlen);

/** Number of history entries stored (max TERM_HISTORY_DEPTH). */
uint32_t term_history_count(void);

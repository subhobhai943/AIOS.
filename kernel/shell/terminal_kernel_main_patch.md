# Phase 5.1 — `kernel_main.c` Integration Patch

Add after the Phase 4.4 sync-primitives smoke-test block:

```c
/* ── Phase 5.1: Terminal ─────────────────────────────────────────── */
#include "shell/terminal.h"

terminal_init();                        /* clear screen, print banner */
print_ok("[phase 5.1] terminal init");

/* Wire keyboard driver into terminal.
 * In keyboard.c, change the end of kbd_isr() so it calls
 * terminal_feed() instead of (or in addition to) the old ring buffer:
 *
 *   terminal_feed(ascii);   // replaces kbd_buf_push(event)
 *
 * Special keys — add to your scancode handler:
 *
 *   0xE0 0x48  → terminal_feed(TERM_KEY_UP)
 *   0xE0 0x50  → terminal_feed(TERM_KEY_DOWN)
 *   0xE0 0x4B  → terminal_feed(TERM_KEY_LEFT)
 *   0xE0 0x4D  → terminal_feed(TERM_KEY_RIGHT)
 *   0xE0 0x47  → terminal_feed(TERM_KEY_HOME)
 *   0xE0 0x4F  → terminal_feed(TERM_KEY_END)
 *   0xE0 0x53  → terminal_feed(TERM_KEY_DEL)
 *   0x0E       → terminal_feed(TERM_KEY_BACKSPACE)
 *   0x1C       → terminal_feed(TERM_KEY_ENTER)
 *   0x0F       → terminal_feed(TERM_KEY_TAB)
 */

/* Smoke-test readline (runs in kernel_main task — scheduler already running) */
terminal_write("Type a line and press Enter: ");
char line_buf[64];
size_t n = terminal_readline(line_buf, sizeof(line_buf));
terminal_write("You typed: ");
terminal_write_len(line_buf, n);
terminal_write_char('\n');
print_ok("[phase 5.1] readline OK");
```

Add `kernel/shell/terminal.o` and `kernel/vga_phase51.o` to the Makefile `OBJ` list.

## Merging `vga_phase51.c` into `vga.c`

The three new functions (`vga_set_cursor`, `vga_get_cursor`, `vga_putchar_at`) should be merged directly into `kernel/vga.c`:

```bash
cat kernel/vga_phase51.c >> kernel/vga.c
```

Then remove `kernel/vga_phase51.c` from the build and add the declarations in `kernel/vga.h` (already done).

## Keyboard ISR extended-scancode table

In `kernel/keyboard.c`, inside the IRQ1 handler, add an `e0_pending` flag:

```c
static bool e0_pending = false;

// inside kbd_isr:
uint8_t sc = inb(0x60);
if (sc == 0xE0) { e0_pending = true; return; }  // wait for next byte
if (e0_pending) {
    e0_pending = false;
    switch (sc) {
        case 0x48: terminal_feed(TERM_KEY_UP);    break;
        case 0x50: terminal_feed(TERM_KEY_DOWN);  break;
        case 0x4B: terminal_feed(TERM_KEY_LEFT);  break;
        case 0x4D: terminal_feed(TERM_KEY_RIGHT); break;
        case 0x47: terminal_feed(TERM_KEY_HOME);  break;
        case 0x4F: terminal_feed(TERM_KEY_END);   break;
        case 0x53: terminal_feed(TERM_KEY_DEL);   break;
        default: break;
    }
    return;
}
// normal single-byte scancodes continue here …
```

#ifndef GUI_INPUT_WIRING_H
#define GUI_INPUT_WIRING_H

/* kernel/gui/input_wiring.h — Phase 10.3
 *
 * One-time activation of GUI input routing:
 *   - keyboard IRQ1 → gui_input_push_key()
 *   - mouse    IRQ12 → gui_input_push_mouse()
 *
 * Call gui_wiring_activate() from the 'startx' shell command
 * (Phase 10.6) before spawning the WM kthread.
 *
 * Freestanding C — no libc.
 */

#include <stdbool.h>

/*
 * gui_wiring_activate()
 *
 * Install GUI callbacks in keyboard.c and mouse.c.
 * Safe to call only once; subsequent calls are no-ops.
 * Must be called before gui_wm_start().
 */
void gui_wiring_activate(void);

/*
 * gui_wiring_is_active()
 *
 * Returns true after gui_wiring_activate() has been called.
 * Can be used by the WM to assert that wiring happened before
 * the event loop starts.
 */
bool gui_wiring_is_active(void);

#endif /* GUI_INPUT_WIRING_H */

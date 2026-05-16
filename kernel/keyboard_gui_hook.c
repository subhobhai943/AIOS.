/* keyboard.c — extended for Phase 10.3: GUI callback hook.
 *
 * If a GUI callback has been registered via keyboard_set_gui_callback(),
 * it is called from the IRQ1 handler right after a key_event_t has been
 * fully populated. When the callback is set, the legacy terminal_feed
 * path should be disabled so that GUI mode has exclusive focus.
 */

/* --- append to the end of the existing keyboard.c --- */

#include "include/keyboard.h"

/* GUI callback slot */
static void (*g_gui_kbd_cb)(const key_event_t *) = (void*)0;

void keyboard_set_gui_callback(void (*cb)(const key_event_t *))
{
    g_gui_kbd_cb = cb;
}

/*
 * Call this from the bottom of your existing kbd_isr() / key processing
 * function right after the key_event_t has been fully populated and before
 * it is enqueued into the terminal ring buffer:
 *
 *   if (g_gui_kbd_cb) {
 *       g_gui_kbd_cb(&ke);
 *   } else {
 *       terminal_feed(ke.ascii, ke.scancode, ke.pressed);
 *   }
 *
 * This ensures that when GUI mode is active the shell no longer receives
 * keyboard events, and all input is routed through the GUI event queue
 * instead.
 */

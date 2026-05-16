/* keyboard_gui_hook.c
 *
 * Phase 10.3 — GUI keyboard callback hook implementation.
 *
 * This file defines keyboard_set_gui_callback() and the static callback slot
 * used by the keyboard driver. The actual call-site is in keyboard.c, inside
 * the IRQ handler / key-processing path once a key_event_t has been decoded.
 */

#include "include/keyboard.h"

/* GUI callback slot */
static void (*g_gui_kbd_cb)(const key_event_t *) = (void*)0;

void keyboard_set_gui_callback(void (*cb)(const key_event_t *))
{
    g_gui_kbd_cb = cb;
}

/*
 * Helper for keyboard.c — invoke this after filling a key_event_t `ke`.
 *
 *   if (g_gui_kbd_cb) {
 *       g_gui_kbd_cb(&ke);
 *   } else {
 *       terminal_feed(ke.ascii, ke.scancode, ke.pressed);
 *   }
 *
 * See keyboard.c comments for the exact integration point. We keep the
 * callback storage here to avoid duplicating it across compilation units.
 */
const void *keyboard_get_gui_callback_slot(void)
{
    return (const void*)&g_gui_kbd_cb;
}

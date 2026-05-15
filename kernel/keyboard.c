/* keyboard.c — extended for Phase 10.3: GUI callback hook.
 *
 * If a GUI callback has been registered via keyboard_set_gui_callback(),
 * it is called from the IRQ1 handler in addition to (but AFTER)
 * the normal terminal_feed() path during text mode.  Once the GUI
 * wiring is active the shell's terminal_readline() will no longer
 * receive key events (the callback replaces terminal_feed).
 *
 * ONLY the new API stub is added here — the rest of keyboard.c is
 * unchanged from Phase 1.6 / 5.1.
 */

/* --- append to the end of the existing keyboard.c --- */

/* GUI callback slot */
static void (*g_gui_kbd_cb)(const key_event_t *) = (void*)0;

void keyboard_set_gui_callback(void (*cb)(const key_event_t *))
{
    g_gui_kbd_cb = cb;
}

/*
 * Call this from the bottom of your existing kbd_isr() / kbd_process_key()
 * right after the key_event_t has been fully populated:
 *
 *   if (g_gui_kbd_cb) {
 *       g_gui_kbd_cb(&ke);
 *   } else {
 *       terminal_feed(ke.ascii, ke.scancode, ke.pressed);
 *   }
 *
 * This ensures the terminal only gets events when GUI mode is off.
 */

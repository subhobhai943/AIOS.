/* kernel/keyboard_gui_hook.c — Phase 10.3
 *
 * Stores the GUI keyboard callback and the text-mode callback slots.
 * keyboard.c calls keyboard_invoke_callback() after decoding each key.
 */

#include "include/keyboard.h"

static keyboard_gui_cb_t  g_gui_cb  = 0;
static keyboard_text_cb_t g_text_cb = 0;

void keyboard_set_gui_callback(keyboard_gui_cb_t cb)
{
    g_gui_cb = cb;
}

void keyboard_set_text_callback(keyboard_text_cb_t cb)
{
    g_text_cb = cb;
}

/* Called from keyboard.c after a key_event_t is decoded. */
void keyboard_invoke_callback(const key_event_t *ke)
{
    if (g_gui_cb) {
        g_gui_cb(ke);
    } else if (g_text_cb) {
        g_text_cb(ke->ascii, ke->scancode, ke->pressed);
    }
}

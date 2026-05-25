/* kernel/gui/input_mode.c
 *
 * Phase 10.3: enable/disable GUI input hooks for mouse and keyboard.
 *
 * When GUI mode is active, low-level mouse/keyboard events are routed
 * into the GUI event queue via gui_input_from_mouse_event() and
 * gui_input_from_key_event(). When GUI mode is disabled, the legacy
 * text-mode paths (VGA cursor + terminal_feed) are used instead.
 */

#include "gui/input_mode.h"
#include "gui/input_bridge.h"
#include "include/mouse.h"
#include "include/keyboard.h"

static int g_gui_input_enabled = 0;

void gui_input_enable(void)
{
    if (g_gui_input_enabled) return;
    g_gui_input_enabled = 1;

    keyboard_set_text_callback(0);
    mouse_set_gui_callback(gui_input_from_mouse_event);
    keyboard_set_gui_callback(gui_input_from_key_event);
}

void gui_input_disable(void)
{
    if (!g_gui_input_enabled) return;
    g_gui_input_enabled = 0;

    mouse_set_gui_callback(0);
    keyboard_set_gui_callback(0);
}

/* kernel/gui/input_bridge.c
 *
 * Bridge from low-level keyboard/mouse drivers to the GUI input queue.
 *
 * Phase 10.3: wire mouse_set_gui_callback() and keyboard_set_gui_callback()
 * so that when the GUI window manager is running, raw input packets are
 * translated into gui_event_t structures and enqueued via gui_input_* APIs.
 */

#include "gui/input.h"
#include "include/mouse.h"
#include "include/keyboard.h"

/* Map PS/2 mouse event into GUI absolute mouse state. */
void gui_input_from_mouse_event(const mouse_event_t *me)
{
    if (!me) return;

    gui_input_push_mouse_absolute(me->abs_x, me->abs_y, me->buttons);
}

/* Map keyboard key_event_t into GUI key events. */
void gui_input_from_key_event(const key_event_t *ke)
{
    if (!ke) return;

    uint8_t mods = 0;
    if (ke->shift) mods |= GUI_MOD_SHIFT;
    if (ke->ctrl)  mods |= GUI_MOD_CTRL;
    if (ke->alt)   mods |= GUI_MOD_ALT;

    uint8_t keycode = ke->ascii ? (uint8_t)ke->ascii : (uint8_t)(ke->scancode & 0x7Fu);

    if (ke->pressed) {
        gui_input_push_key_down(keycode, mods);
    } else {
        gui_input_push_key_up(keycode, mods);
    }
}

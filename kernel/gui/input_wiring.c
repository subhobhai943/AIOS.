/* kernel/gui/input_wiring.c — Phase 10.3
 *
 * Activate GUI input mode: redirect mouse and keyboard callbacks
 * from the VGA/terminal subsystem to the GUI event queue.
 *
 * Call gui_wiring_activate() once from the startx shell command.
 *
 * Constraints:
 *   - Freestanding C, -ffreestanding -nostdlib -mno-red-zone
 *   - Only <stdint.h>, <stddef.h>, <stdbool.h>
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "input_wiring.h"
#include "input.h"          /* gui_event_t, GUI_MOD_*, GUI_MOUSE_BUTTON_*, */
                            /* gui_input_push_key_down/up,                  */
                            /* gui_input_push_mouse_absolute                 */

/* keyboard.h / mouse.h live in kernel/include/ — resolved via -I flag */
#include "keyboard.h"
#include "mouse.h"
#include "../serial.h"

/* ── activation guard ───────────────────────────────────────── */
static bool g_gui_wiring_active = false;

/* ── GUI keyboard callback ──────────────────────────────────── */
/*
 * key_event_t (keyboard.h):
 *   uint8_t scancode, char ascii, bool pressed,
 *   bool shift, bool ctrl, bool alt
 * — no 'modifiers' byte; build it ourselves.
 *
 * gui_event_t (input.h):
 *   keycode   = uint8_t  (scancode)
 *   modifiers = uint8_t  (GUI_MOD_* bitmask)
 *   — no 'ascii' or 'key' field
 */
static void gui_kbd_callback(const key_event_t *ke)
{
    if (!ke) return;

    uint8_t mods = 0;
    if (ke->shift) mods |= GUI_MOD_SHIFT;
    if (ke->ctrl)  mods |= GUI_MOD_CTRL;
    if (ke->alt)   mods |= GUI_MOD_ALT;

    if (ke->pressed)
        gui_input_push_key_down(ke->scancode, mods);
    else
        gui_input_push_key_up(ke->scancode, mods);
}

/* ── GUI mouse callback ─────────────────────────────────────── */
/*
 * mouse_event_t (mouse.h):
 *   int32_t dx, dy, abs_x, abs_y
 *   uint8_t buttons, prev_buttons
 */
static void gui_mouse_callback(const mouse_event_t *me)
{
    if (!me) return;

    /* Push absolute position + button state every packet */
    gui_input_push_mouse_absolute(me->abs_x, me->abs_y, me->buttons);
}

/* ── gui_wiring_activate ─────────────────────────────────────── */
void gui_wiring_activate(void)
{
    if (g_gui_wiring_active) {
        klog("[input_wiring] already active\n");
        return;
    }

    klog("[input_wiring] activating GUI keyboard callback\n");
    keyboard_set_gui_callback(gui_kbd_callback);

    klog("[input_wiring] activating GUI mouse callback\n");
    mouse_set_gui_callback(gui_mouse_callback);

    g_gui_wiring_active = true;
    klog("[input_wiring] GUI input wiring ACTIVE\n");
}

/* ── gui_wiring_is_active ────────────────────────────────────── */
bool gui_wiring_is_active(void)
{
    return g_gui_wiring_active;
}

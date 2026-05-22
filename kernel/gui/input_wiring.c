/* kernel/gui/input_wiring.c — Phase 10.3
 *
 * Activate GUI input mode: redirect mouse and keyboard callbacks
 * from the VGA/terminal subsystem to the GUI event queue
 * (gui_input.c).
 *
 * Call gui_wiring_activate() exactly once, from the startx command
 * (shell.c Phase 10.6).  After this call:
 *   - Every PS/2 mouse IRQ12 event pushes a gui_event_t via
 *     gui_input_push_mouse().
 *   - Every PS/2 keyboard IRQ1 event pushes a gui_event_t via
 *     gui_input_push_key().
 *   - The VGA terminal stops consuming keyboard events.
 *
 * Constraints:
 *   - Freestanding C, -ffreestanding -nostdlib -mno-red-zone
 *   - Only <stdint.h>, <stddef.h>, <stdbool.h>
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "input_wiring.h"
#include "input.h"

/* Use bare names — resolved via -I./kernel/include in the Makefile */
#include "keyboard.h"
#include "mouse.h"
#include "../serial.h"

/* ── activation guard ───────────────────────────────────────── */
static bool g_gui_wiring_active = false;

/* ── GUI keyboard callback ──────────────────────────────────── */
static void gui_kbd_callback(const key_event_t *ke)
{
    if (!ke) return;

    gui_event_t ev;
    ev.type     = ke->pressed ? GUI_EVENT_KEY_DOWN : GUI_EVENT_KEY_UP;
    ev.key      = ke->scancode;
    ev.ascii    = ke->ascii;
    ev.mods     = ke->modifiers;
    ev.x        = 0;
    ev.y        = 0;
    ev.buttons  = 0;

    gui_input_push_key(&ev);
}

/* ── GUI mouse callback ─────────────────────────────────────── */
static void gui_mouse_callback(const mouse_event_t *me)
{
    if (!me) return;

    /* --- Movement event --- */
    if (me->dx != 0 || me->dy != 0) {
        gui_event_t ev;
        ev.type    = GUI_EVENT_MOUSE_MOVE;
        ev.x       = me->abs_x;
        ev.y       = me->abs_y;
        ev.buttons = me->buttons;
        ev.key     = 0;
        ev.ascii   = 0;
        ev.mods    = 0;
        gui_input_push_mouse(&ev);
    }

    /* --- Button change events --- */
    uint8_t changed = me->buttons ^ me->prev_buttons;
    if (changed) {
        static const uint8_t btn_masks[3] = {
            GUI_MOUSE_BUTTON_LEFT,
            GUI_MOUSE_BUTTON_RIGHT,
            GUI_MOUSE_BUTTON_MIDDLE
        };
        for (int i = 0; i < 3; i++) {
            if (!(changed & btn_masks[i])) continue;

            gui_event_t ev;
            ev.type    = (me->buttons & btn_masks[i])
                         ? GUI_EVENT_MOUSE_DOWN
                         : GUI_EVENT_MOUSE_UP;
            ev.x       = me->abs_x;
            ev.y       = me->abs_y;
            ev.buttons = btn_masks[i];
            ev.key     = 0;
            ev.ascii   = 0;
            ev.mods    = 0;
            gui_input_push_mouse(&ev);
        }
    }
}

/* ── gui_wiring_activate ─────────────────────────────────────── */
void gui_wiring_activate(void)
{
    if (g_gui_wiring_active) {
        klog("[input_wiring] already active — ignoring duplicate call\n");
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

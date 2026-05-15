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
 * This is a clean activation fence — there is no deactivate path
 * (switching back to text mode is a future TTY-switch feature).
 *
 * Constraints:
 *   - Freestanding C, -ffreestanding -nostdlib -mno-red-zone
 *   - Only <stdint.h>, <stddef.h>, <stdbool.h>
 *   - Heap: kmalloc/kfree (not needed here — static activation flag)
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "input_wiring.h"
#include "input.h"

#include "../serial.h"
#include "../keyboard.h"
#include "../mouse.h"

/* ── activation guard ───────────────────────────────────────── */
static bool g_gui_wiring_active = false;

/* ── GUI keyboard callback ──────────────────────────────────── */
/*
 * Called by keyboard.c IRQ1 handler in place of the terminal feed.
 * Translates a raw key_event_t into a gui_event_t and enqueues it.
 *
 * key_event_t fields (from keyboard.h):
 *   uint8_t  scancode    — PS/2 Set-1 scancode
 *   uint8_t  ascii       — ASCII character (0 if non-printable)
 *   bool     pressed     — true = keydown, false = keyup
 *   uint8_t  modifiers   — KBD_MOD_SHIFT | KBD_MOD_CTRL | KBD_MOD_CAPS
 */
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
/*
 * Called by mouse.c IRQ12 handler in place of the VGA cursor draw.
 * Translates a mouse_event_t into one or two gui_event_t messages
 * (MOUSE_MOVE and/or MOUSE_DOWN / MOUSE_UP).
 *
 * mouse_event_t fields (from mouse.h):
 *   int32_t  dx, dy      — delta X / delta Y (signed, packet-decoded)
 *   int32_t  abs_x, abs_y — absolute clamped position
 *   uint8_t  buttons     — bit0=left, bit1=right, bit2=middle
 *   uint8_t  prev_buttons — buttons state before this packet
 */
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
        /* Check each of the 3 buttons */
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
            ev.buttons = btn_masks[i];  /* which button caused this event */
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

    /*
     * keyboard_set_gui_callback(fn):
     *   Registered in keyboard.c.  When set to non-NULL, the IRQ1
     *   handler calls fn(ke) instead of (in addition to) the default
     *   terminal_feed() path.  Setting it here redirects all key
     *   events to the GUI queue.
     *
     * mouse_set_gui_callback(fn):
     *   Registered in mouse.c.  When set to non-NULL, the IRQ12
     *   handler calls fn(me) instead of moving the VGA text cursor.
     */
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

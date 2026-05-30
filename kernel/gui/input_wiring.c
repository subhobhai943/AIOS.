/* kernel/gui/input_wiring.c — Phase 10.3
 *
 * Activate GUI input mode: redirect mouse and keyboard callbacks
 * from the VGA/terminal subsystem to the GUI event queue.
 *
 * Call gui_wiring_activate() once from the startx shell command.
 *
 * Fix (Phase 11 bugfix):
 *   gui_kbd_callback previously forwarded ke->scancode as the keycode.
 *   terminal_gui.c (and all other apps) compare ev->keycode against
 *   ASCII values ('\n', '\b', 0x20..0x7E).  PS/2 scancodes are
 *   completely different numbers, so every key event was silently
 *   dropped — the terminal appeared frozen.
 *
 *   Fix: when ke->ascii is non-zero (i.e. keyboard.c translated the
 *   scancode to a printable/control ASCII character), use that value
 *   as the keycode.  Fall back to ke->scancode only for keys that have
 *   no ASCII equivalent (function keys, arrows, etc.) so the WM can
 *   still act on them if needed.
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
 *
 * gui_event_t (input.h):
 *   keycode   = uint8_t  (ASCII when available, else scancode)
 *   modifiers = uint8_t  (GUI_MOD_* bitmask)
 */
static void gui_kbd_callback(const key_event_t *ke)
{
    if (!ke) return;

    uint8_t mods = 0;
    if (ke->shift) mods |= GUI_MOD_SHIFT;
    if (ke->ctrl)  mods |= GUI_MOD_CTRL;
    if (ke->alt)   mods |= GUI_MOD_ALT;

    /*
     * KEY FIX: use the ASCII-translated value when keyboard.c provides
     * one.  This is what terminal_gui.c, notepad, etc. expect in
     * ev->keycode.  Raw scancodes (0x10 for 'q', 0x1C for Enter, etc.)
     * are NOT ASCII and would be silently ignored by every app.
     *
     * ke->ascii is set by translate_scancode() in keyboard.c for all
     * printable characters and for \n, \b, \t, and space.  It is 0
     * for modifier keys, function keys, arrow keys, etc.
     */
    uint8_t code = (ke->ascii != 0) ? (uint8_t)ke->ascii : ke->scancode;

    if (ke->pressed)
        gui_input_push_key_down(code, mods);
    else
        gui_input_push_key_up(code, mods);
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

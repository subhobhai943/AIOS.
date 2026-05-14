/* kernel/gui/input_wiring.c
 * Phase 10.3 — Wire existing PS/2 mouse and keyboard drivers to the
 * GUI input abstraction so the window manager event queue is populated
 * when GUI mode is active.
 *
 * How it works
 * ------------
 * The mouse IRQ handler (mouse_handle_irq) and keyboard ISR both fire
 * with every device packet.  When GUI mode is active we additionally
 * call gui_input_push_mouse_delta() / gui_input_push_key_down() from
 * within those paths.
 *
 * Rather than patching the ISR source files (mouse.c / keyboard.c)
 * directly, we provide two thin wrapper functions that the kernel_main
 * hooks into the existing interrupt paths via function pointers, keeping
 * the drivers pristine.
 *
 * Usage in kernel_main.c
 * -----------------------
 *   #include "gui/input_wiring.h"
 *   gui_input_wiring_install();   // call after mouse_init() + kbd_init()
 *
 * After this call, every mouse movement / button event and every
 * key-press / key-release is automatically mirrored into the GUI queue.
 */

#include "gui/input_wiring.h"
#include "gui/input.h"
#include "include/mouse.h"
#include "include/keyboard.h"
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Global "GUI mode active" flag                                       */
/* ------------------------------------------------------------------ */

static volatile bool g_gui_mode = false;

void gui_input_wiring_set_active(bool active)
{
    g_gui_mode = active;
}

bool gui_input_wiring_is_active(void)
{
    return g_gui_mode;
}

/* ------------------------------------------------------------------ */
/* Mouse wiring                                                        */
/* ------------------------------------------------------------------ */

/*
 * Call this from the mouse IRQ12 handler AFTER updating mouse_x/mouse_y
 * and pushing the low-level event into the PS/2 ring buffer.
 *
 * Parameters mirror the already-decoded delta values and button flags
 * that mouse_handle_irq() already computes.
 */
void gui_input_wiring_on_mouse(int dx, int dy, uint8_t ps2_buttons)
{
    if (!g_gui_mode) return;

    /* Map PS/2 button bitmask to GUI bitmask:
     *  PS/2 bit0 = Left, bit1 = Right, bit2 = Middle
     *  GUI uses the same bit positions — direct copy is fine.
     */
    uint8_t gui_buttons = ps2_buttons & 0x07u;
    gui_input_push_mouse_delta(dx, dy, gui_buttons);
}

/* ------------------------------------------------------------------ */
/* Keyboard wiring                                                     */
/* ------------------------------------------------------------------ */

/*
 * Call this from the keyboard ISR once an ASCII / keycode has been
 * decoded.  `is_press` is true for key-down, false for key-up.
 * `ascii` is the decoded character (or 0 for non-printable).
 * `mods` is a bitmask of GUI_MOD_SHIFT | GUI_MOD_CTRL | GUI_MOD_ALT.
 */
void gui_input_wiring_on_key(uint8_t ascii, uint8_t mods, bool is_press)
{
    if (!g_gui_mode) return;

    if (is_press)
        gui_input_push_key_down(ascii, mods);
    else
        gui_input_push_key_up(ascii, mods);
}

/* ------------------------------------------------------------------ */
/* Install hooks                                                        */
/* ------------------------------------------------------------------ */

/*
 * Sets the global function pointers g_mouse_gui_hook and g_kbd_gui_hook
 * that mouse.c / keyboard.c check at the end of their IRQ handlers.
 *
 * This avoids any libc dependency; function pointers are just global
 * variables in BSS, which are zero-initialised, so no hook fires until
 * explicitly installed here.
 */
extern void (*g_mouse_gui_hook)(int dx, int dy, uint8_t buttons);
extern void (*g_kbd_gui_hook)(uint8_t ascii, uint8_t mods, bool is_press);

void gui_input_wiring_install(void)
{
    g_mouse_gui_hook = gui_input_wiring_on_mouse;
    g_kbd_gui_hook   = gui_input_wiring_on_key;
}

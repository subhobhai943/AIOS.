#ifndef GUI_INPUT_WIRING_H
#define GUI_INPUT_WIRING_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Phase 10.3 — GUI input wiring.
 *
 * Installs thin hook callbacks into the mouse and keyboard IRQ handlers
 * so that raw input events are mirrored into the GUI event queue when
 * GUI mode is active.
 */

/* Install the hooks (call once after mouse_init() + keyboard_init()). */
void gui_input_wiring_install(void);

/* Enable / disable GUI input forwarding at runtime. */
void gui_input_wiring_set_active(bool active);
bool gui_input_wiring_is_active(void);

/* Called by mouse IRQ path — also exported so mouse.c can call directly. */
void gui_input_wiring_on_mouse(int dx, int dy, uint8_t ps2_buttons);

/* Called by keyboard ISR path. */
void gui_input_wiring_on_key(uint8_t ascii, uint8_t mods, bool is_press);

/* Weak-symbol hook pointers populated by gui_input_wiring_install().
 * mouse.c and keyboard.c test these for non-NULL before calling. */
extern void (*g_mouse_gui_hook)(int dx, int dy, uint8_t buttons);
extern void (*g_kbd_gui_hook)(uint8_t ascii, uint8_t mods, bool is_press);

#endif /* GUI_INPUT_WIRING_H */

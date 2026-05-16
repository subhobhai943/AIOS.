#ifndef GUI_INPUT_BRIDGE_H
#define GUI_INPUT_BRIDGE_H

#include "include/mouse.h"
#include "include/keyboard.h"

/* Bridge functions implemented in kernel/gui/input_bridge.c
 * that translate low-level mouse/keyboard events into GUI
 * input events via gui_input_push_* APIs.
 */

void gui_input_from_mouse_event(const mouse_event_t *me);
void gui_input_from_key_event(const key_event_t *ke);

#endif /* GUI_INPUT_BRIDGE_H */

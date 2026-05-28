#ifndef GUI_DESKTOP_H
#define GUI_DESKTOP_H

#include "gfx/framebuffer.h"
#include "gfx/font.h"
#include "gui/input.h"
#include <stdbool.h>

void desktop_init(void);
void desktop_draw(framebuffer_t *fb, const gui_font_t *font);

/*
 * desktop_handle_mouse_down()
 *
 * Called by the WM on every left-button-down event BEFORE window
 * hit-testing.  If the click lands on a desktop icon, the function
 * handles the event (double-click launches the app, single-click
 * selects it) and returns true so the WM skips window hit-testing.
 * Returns false if the click was not on any icon.
 */
bool desktop_handle_mouse_down(int32_t px, int32_t py,
                               framebuffer_t *fb, const gui_font_t *font);

#endif /* GUI_DESKTOP_H */

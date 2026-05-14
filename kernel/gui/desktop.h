#ifndef GUI_DESKTOP_H
#define GUI_DESKTOP_H

#include <stdint.h>
#include "gfx/framebuffer.h"
#include "gfx/font.h"

/*
 * Desktop background rendering.
 *
 * Draws the solid-color desktop background and optional AIOS logo text.
 * Called once per frame by the window manager before drawing windows.
 */

void desktop_init(void);
void desktop_draw(framebuffer_t *fb, const gui_font_t *font);

#endif /* GUI_DESKTOP_H */

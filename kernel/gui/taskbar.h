#ifndef GUI_TASKBAR_H
#define GUI_TASKBAR_H

#include <stdint.h>
#include <stdbool.h>
#include "gfx/framebuffer.h"
#include "gfx/font.h"

/*
 * Taskbar — Phase 10.5
 *
 * Reserves a 40-pixel strip at the bottom of the screen.
 * Contains:
 *   - "Start" button (left side)
 *   - Window buttons (middle) — one per open, non-hidden window
 *   - System clock text (right side, placeholder)
 */

#define TASKBAR_HEIGHT     40u
#define TASKBAR_BTN_W      120u   /* max width of each window button */
#define TASKBAR_START_W    80u    /* width of the Start button */
#define TASKBAR_PADDING    4u

void taskbar_init(uint32_t screen_w, uint32_t screen_h);
void taskbar_draw(framebuffer_t *fb, const gui_font_t *font);

/*
 * Handle a mouse-down event whose coordinates fall inside the taskbar strip.
 * Returns true if the event was consumed.
 */
bool taskbar_handle_mouse_down(int32_t px, int32_t py,
                               framebuffer_t *fb, const gui_font_t *font);

#endif /* GUI_TASKBAR_H */

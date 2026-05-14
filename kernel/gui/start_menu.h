#ifndef GUI_START_MENU_H
#define GUI_START_MENU_H

#include <stdint.h>
#include <stdbool.h>
#include "gfx/framebuffer.h"
#include "gfx/font.h"
#include "gui/input.h"

/*
 * Start menu — Phase 10.5
 *
 * A vertical popup that appears above the Start button when clicked.
 * Each item launches a GUI application window.
 */

void start_menu_init(void);
void start_menu_toggle(void);
bool start_menu_is_open(void);
void start_menu_draw(framebuffer_t *fb, const gui_font_t *font);
void start_menu_handle_event(const gui_event_t *ev,
                             framebuffer_t *fb, const gui_font_t *font);

#endif /* GUI_START_MENU_H */

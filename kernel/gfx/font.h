#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include <stdbool.h>

#include "gfx/framebuffer.h"

/* Minimal bitmap-style font description for GUI text. */
typedef struct gui_font {
    uint8_t width;   /* pixels per character */
    uint8_t height;  /* pixels per character */
} gui_font_t;

/* Load the built-in debug font (8x16). */
const gui_font_t *font_load_builtin(void);

/* Measure the pixel width of a single-line ASCII string. */
uint32_t font_measure_string(const gui_font_t *font, const char *s);

/* Draw a single character at (x, y). Background is filled, border drawn. */
void font_draw_char(framebuffer_t *fb,
                    const gui_font_t *font,
                    uint32_t x,
                    uint32_t y,
                    char ch,
                    uint32_t fg,
                    uint32_t bg);

/* Draw a single-line string starting at (x, y). Newlines are ignored. */
void font_draw_string(framebuffer_t *fb,
                      const gui_font_t *font,
                      uint32_t x,
                      uint32_t y,
                      const char *s,
                      uint32_t fg,
                      uint32_t bg);

/* Helper for labels: draw string horizontally centered within a region.
 * If it does not fit, truncate with "...".
 */
void font_draw_string_centered(framebuffer_t *fb,
                               const gui_font_t *font,
                               uint32_t region_x,
                               uint32_t region_y,
                               uint32_t region_w,
                               const char *s,
                               uint32_t fg,
                               uint32_t bg);

#endif /* FONT_H */

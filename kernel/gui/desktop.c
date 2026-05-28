/* kernel/gui/desktop.c
 * Desktop background renderer — Phase 10.5
 *
 * Draws a simple two-tone gradient desktop (accent-tinted at the top
 * shading to the standard desktop background colour at the bottom).
 * Renders a centred "AIOS" watermark using the builtin font.
 *
 * No libc. No standard headers beyond <stdint.h> / <stdbool.h> / <stddef.h>.
 *
 * FIX: gradient lerp now uses int32_t arithmetic to avoid uint32_t
 * underflow when a bg channel value is less than the accent channel value
 * (e.g. blue channel: bg=0x3A, accent=0xD4 → 0x3A-0xD4 would wrap to
 * a huge positive number under unsigned subtraction).
 */

#include "gui/desktop.h"
#include "gfx/framebuffer.h"
#include "gfx/colors.h"
#include "gfx/font.h"

#include <stdint.h>

/* ---- AIOS branding watermark centred on the desktop ---- */
#define WATERMARK_TEXT  "AIOS v0.1"

void desktop_init(void)
{
    /* Nothing to allocate yet — extend if we add wallpaper loading later. */
}

void desktop_draw(framebuffer_t *fb, const gui_font_t *font)
{
    if (!fb) return;

    /* Fill desktop area (full screen — taskbar will paint over its strip) */
    fb_clear(UI_COLOR_DESKTOP_BG);

    /* Draw a subtle horizontal gradient band at the top:
     * top row = accent colour, fades to desktop bg over band_h rows. */
    uint32_t band_h = fb->height / 5u;
    for (uint32_t row = 0; row < band_h; row++) {
        /* alpha: 0 at top (full accent) → 255 at band_h (full bg) */
        int32_t alpha = (int32_t)((row * 0xFFu) / (band_h ? band_h : 1u));

        /* Extract channels as signed to avoid underflow on subtraction */
        int32_t r_bg  = (int32_t)((UI_COLOR_DESKTOP_BG >> 16) & 0xFF);
        int32_t g_bg  = (int32_t)((UI_COLOR_DESKTOP_BG >>  8) & 0xFF);
        int32_t b_bg  = (int32_t)((UI_COLOR_DESKTOP_BG      ) & 0xFF);
        int32_t r_acc = (int32_t)((UI_COLOR_ACCENT     >> 16) & 0xFF);
        int32_t g_acc = (int32_t)((UI_COLOR_ACCENT     >>  8) & 0xFF);
        int32_t b_acc = (int32_t)((UI_COLOR_ACCENT          ) & 0xFF);

        /* FIX: signed lerp — r_acc + (r_bg - r_acc) * alpha / 255 */
        int32_t r = r_acc + ((r_bg - r_acc) * alpha) / 255;
        int32_t g = g_acc + ((g_bg - g_acc) * alpha) / 255;
        int32_t b = b_acc + ((b_bg - b_acc) * alpha) / 255;

        uint32_t color = 0xFF000000u
                       | ((uint32_t)r << 16)
                       | ((uint32_t)g <<  8)
                       | ((uint32_t)b);
        fb_fill_rect(0, (int32_t)row, fb->width, 1u, color);
    }

    /* Watermark text at bottom-centre above taskbar area */
    if (font) {
        uint32_t taskbar_h = 40u;
        uint32_t text_y = fb->height - taskbar_h - font->height - 6u;
        font_draw_string_centered(fb, font,
                                  0, (int32_t)text_y, fb->width,
                                  WATERMARK_TEXT,
                                  UI_COLOR_WINDOW_BORDER,  /* muted colour */
                                  UI_COLOR_DESKTOP_BG);
    }
}

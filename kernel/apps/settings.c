/*
 * kernel/apps/settings.c — Phase 11.4
 *
 * Simple GUI Settings window for AIOS.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "settings.h"
#include "../heap.h"
#include "../gfx/framebuffer.h"
#include "../gfx/font.h"
#include "../gui/window.h"

#define SETTINGS_FONT_W 8
#define SETTINGS_FONT_H 16
#define PADDING 6

#define COL_BG     0xFF101010u
#define COL_LABEL  0xFFEFEFEFu

static settings_t *g_settings = 0;

static void st_fill_rect(framebuffer_t *fb, int x, int y, int w, int h, uint32_t col)
{
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= (int)fb->height) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= (int)fb->width) continue;
            fb_put_pixel((uint32_t)xx, (uint32_t)yy, col);
        }
    }
}

static void st_draw_label(framebuffer_t *fb, const gui_font_t *font,
                          int x, int y, const char *s)
{
    while (s && *s) {
        font_draw_char(fb, font, (uint32_t)x, (uint32_t)y, *s, COL_LABEL, COL_BG);
        x += SETTINGS_FONT_W;
        s++;
    }
}

static void settings_draw(gui_window_t *win, framebuffer_t *fb)
{
    (void)win;
    if (!fb) return;

    const gui_font_t *font = font_load_builtin();

    int x0 = win->x;
    int y0 = win->y;
    int w  = (int)win->width;
    int h  = (int)win->height;

    st_fill_rect(fb, x0, y0, w, h, COL_BG);

    int y = y0 + PADDING;
    st_draw_label(fb, font, x0 + PADDING, y, "Settings (placeholder)");
    y += SETTINGS_FONT_H * 2;
    st_draw_label(fb, font, x0 + PADDING, y, "Future options:");
    y += SETTINGS_FONT_H;
    st_draw_label(fb, font, x0 + PADDING * 2, y, "- Display / theme");
    y += SETTINGS_FONT_H;
    st_draw_label(fb, font, x0 + PADDING * 2, y, "- Keyboard / input");
    y += SETTINGS_FONT_H;
    st_draw_label(fb, font, x0 + PADDING * 2, y, "- LLM parameters");
}

static void settings_handle_event(gui_window_t *win, const gui_event_t *ev)
{
    (void)win; (void)ev;
    /* No interactive controls yet. */
}

settings_t *settings_open(void)
{
    if (g_settings) return g_settings;

    settings_t *s = (settings_t*)kmalloc(sizeof(settings_t));
    if (!s) return 0;

    gui_window_t *win = gui_create_window(160, 110, 420, 260,
                                          "Settings",
                                          settings_draw,
                                          settings_handle_event,
                                          s);
    if (!win) {
        kfree(s);
        return 0;
    }

    s->win_id = win->id;
    g_settings = s;
    return s;
}

void settings_close(settings_t *s)
{
    if (!s) return;
    if (g_settings == s) g_settings = 0;
    kfree(s);
}

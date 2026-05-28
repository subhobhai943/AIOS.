/* kernel/gui/desktop.c
 * Desktop background renderer + icon grid — Phase 10.5 / Win7 pass
 *
 * Draws:
 *   1. A two-tone gradient background (accent → desktop-bg).
 *   2. A column of 5 desktop icon tiles on the left side.
 *   3. An "AIOS v0.1" watermark near the bottom.
 *
 * Icons support single-click selection and double-click launch.
 * The WM must call desktop_handle_mouse_down() on MOUSE_DOWN events
 * that hit the bare desktop (before window hit-testing).
 *
 * No libc. Freestanding C. Headers: <stdint.h>, <stdbool.h>, <stddef.h>.
 */

#include "gui/desktop.h"
#include "gfx/framebuffer.h"
#include "gfx/colors.h"
#include "gfx/font.h"
#include "apps/terminal_gui.h"
#include "apps/explorer.h"
#include "apps/notepad.h"
#include "apps/ai_chat.h"
#include "apps/settings.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* PIT tick counter — provided by pit.c */
extern volatile uint64_t g_ticks;

/* ---- Icon layout constants ---- */
#define ICON_SIZE        48u   /* icon tile width = height in pixels */
#define ICON_RADIUS       6u   /* corner radius of the icon tile      */
#define ICON_MARGIN_X    20u   /* left margin from screen edge         */
#define ICON_MARGIN_Y    24u   /* top margin from screen edge          */
#define ICON_SPACING     16u   /* vertical gap between icons           */
#define LABEL_GAP         4u   /* gap between icon bottom and label    */
#define DBLCLICK_TICKS  500u   /* 500 ms window for double-click       */

/* ---- Icon descriptor ---- */
typedef void (*icon_launch_fn_t)(void);

typedef struct {
    const char       *label;    /* text shown below icon tile  */
    const char       *glyph;    /* 1-2 char shown inside tile  */
    uint32_t          bg;       /* tile fill colour            */
    icon_launch_fn_t  launch;   /* called on double-click      */
} desktop_icon_t;

static void launch_terminal(void) { terminal_gui_open(); }
static void launch_explorer(void) { explorer_open(0); }
static void launch_notepad(void)  { notepad_open(0); }
static void launch_ai_chat(void)  { ai_chat_open(); }
static void launch_settings(void) { settings_open(); }

static const desktop_icon_t g_icons[] = {
    { "Terminal",  ">>" , 0xFF1A3A1Au, launch_terminal },
    { "Explorer",  "EX" , 0xFF1A2A3Au, launch_explorer },
    { "Notepad",   "NP" , 0xFF2A1A3Au, launch_notepad  },
    { "AI Chat",   "AI" , 0xFF3A1A2Au, launch_ai_chat  },
    { "Settings",  "ST" , 0xFF2A2A1Au, launch_settings },
};
#define ICON_COUNT  ((uint32_t)(sizeof(g_icons)/sizeof(g_icons[0])))

/* ---- Per-icon runtime state ---- */
static uint32_t g_selected   = 0xFFFFFFFFu; /* index of selected icon, none = 0xFF... */
static uint64_t g_last_click_tick[ICON_COUNT];
static uint32_t g_last_click_idx = 0xFFFFFFFFu;

#define WATERMARK_TEXT  "AIOS v0.1"

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void desktop_init(void)
{
    g_selected = 0xFFFFFFFFu;
    g_last_click_idx = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < ICON_COUNT; i++)
        g_last_click_tick[i] = 0;
}

/* ------------------------------------------------------------------ */
/* Geometry helper: top-left y of icon[i]                             */
/* ------------------------------------------------------------------ */

static uint32_t icon_y(uint32_t i)
{
    return ICON_MARGIN_Y + i * (ICON_SIZE + ICON_SPACING + 16u /* label height */);
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */

void desktop_draw(framebuffer_t *fb, const gui_font_t *font)
{
    if (!fb) return;

    /* 1. Solid desktop background */
    fb_clear(UI_COLOR_DESKTOP_BG);

    /* 2. Gradient band at the top (accent → desktop bg) */
    uint32_t band_h = fb->height / 5u;
    for (uint32_t row = 0; row < band_h; row++) {
        int32_t alpha  = (int32_t)((row * 0xFFu) / (band_h ? band_h : 1u));
        int32_t r_bg   = (int32_t)((UI_COLOR_DESKTOP_BG >> 16) & 0xFF);
        int32_t g_bg   = (int32_t)((UI_COLOR_DESKTOP_BG >>  8) & 0xFF);
        int32_t b_bg   = (int32_t)((UI_COLOR_DESKTOP_BG      ) & 0xFF);
        int32_t r_acc  = (int32_t)((UI_COLOR_ACCENT     >> 16) & 0xFF);
        int32_t g_acc  = (int32_t)((UI_COLOR_ACCENT     >>  8) & 0xFF);
        int32_t b_acc  = (int32_t)((UI_COLOR_ACCENT          ) & 0xFF);
        int32_t r = r_acc + ((r_bg - r_acc) * alpha) / 255;
        int32_t g = g_acc + ((g_bg - g_acc) * alpha) / 255;
        int32_t b = b_acc + ((b_bg - b_acc) * alpha) / 255;
        uint32_t color = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        fb_fill_rect(0, (int32_t)row, fb->width, 1u, color);
    }

    /* 3. Desktop icons */
    for (uint32_t i = 0; i < ICON_COUNT; i++) {
        uint32_t ix = ICON_MARGIN_X;
        uint32_t iy = icon_y(i);

        bool selected = (g_selected == i);

        /* Tile fill */
        uint32_t tile_col  = g_icons[i].bg;
        uint32_t border_col = selected ? UI_COLOR_ACCENT : UI_COLOR_WINDOW_BORDER;

        /* Selection highlight: slightly brighten the tile bg */
        if (selected) {
            uint32_t r = ((tile_col >> 16) & 0xFF);
            uint32_t g = ((tile_col >>  8) & 0xFF);
            uint32_t b = ( tile_col        & 0xFF);
            r = r + 0x30u > 0xFFu ? 0xFFu : r + 0x30u;
            g = g + 0x30u > 0xFFu ? 0xFFu : g + 0x30u;
            b = b + 0x30u > 0xFFu ? 0xFFu : b + 0x30u;
            tile_col = 0xFF000000u | (r << 16) | (g << 8) | b;
        }

        fb_fill_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, ICON_RADIUS, tile_col);
        fb_draw_rounded_rect(ix, iy, ICON_SIZE, ICON_SIZE, ICON_RADIUS, border_col);

        /* Glyph centered in tile */
        if (font && g_icons[i].glyph) {
            uint32_t gw = 2u * (font->width  ? font->width  : 8u);
            uint32_t gh =       font->height ? font->height : 16u;
            uint32_t gx = ix + (ICON_SIZE - gw) / 2u;
            uint32_t gy = iy + (ICON_SIZE - gh) / 2u;
            font_draw_string(fb, font, gx, gy,
                             g_icons[i].glyph,
                             UI_COLOR_TEXT_FG, tile_col);
        }

        /* Label below tile */
        if (font && g_icons[i].label) {
            uint32_t lw = 8u * (font->width ? font->width : 8u);
            uint32_t lx = ix + ICON_SIZE / 2u;
            uint32_t ly = iy + ICON_SIZE + LABEL_GAP;
            font_draw_string_centered(fb, font,
                                      (int32_t)(lx - lw/2u), (int32_t)ly,
                                      lw,
                                      g_icons[i].label,
                                      UI_COLOR_TEXT_FG, UI_COLOR_DESKTOP_BG);
        }
    }

    /* 4. Watermark */
    if (font) {
        uint32_t taskbar_h = 40u;
        uint32_t text_y    = fb->height - taskbar_h - (font->height ? font->height : 16u) - 6u;
        font_draw_string_centered(fb, font,
                                  0, (int32_t)text_y, fb->width,
                                  WATERMARK_TEXT,
                                  UI_COLOR_WINDOW_BORDER,
                                  UI_COLOR_DESKTOP_BG);
    }
}

/* ------------------------------------------------------------------ */
/* Mouse handler (single-click select, double-click launch)           */
/* ------------------------------------------------------------------ */

bool desktop_handle_mouse_down(int32_t px, int32_t py,
                               framebuffer_t *fb, const gui_font_t *font)
{
    (void)fb; (void)font;

    for (uint32_t i = 0; i < ICON_COUNT; i++) {
        int32_t ix = (int32_t)ICON_MARGIN_X;
        int32_t iy = (int32_t)icon_y(i);
        if (px >= ix && px < ix + (int32_t)ICON_SIZE &&
            py >= iy && py < iy + (int32_t)ICON_SIZE) {

            uint64_t now = g_ticks;
            bool dbl = (g_last_click_idx == i) &&
                       ((now - g_last_click_tick[i]) < DBLCLICK_TICKS);

            g_last_click_tick[i] = now;
            g_last_click_idx     = i;
            g_selected           = i;

            if (dbl && g_icons[i].launch) {
                g_icons[i].launch();
                g_selected = 0xFFFFFFFFu; /* deselect after launch */
            }
            return true;
        }
    }

    /* Click on empty desktop — deselect */
    g_selected       = 0xFFFFFFFFu;
    g_last_click_idx = 0xFFFFFFFFu;
    return false;
}

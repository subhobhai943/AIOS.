/* kernel/gui/taskbar.c
 * Taskbar — Phase 10.5 / Win7 pass
 *
 * Changes in Win7 pass:
 *  - Start button replaced with a circular "pearl orb" (Win7-style).
 *    The orb is a solid filled circle (UI_COLOR_ACCENT) with a bright
 *    inner highlight ring to simulate the glass-bead gradient look.
 *  - Uptime clock and window buttons unchanged.
 *
 * No libc. Allowed headers: <stdint.h>, <stdbool.h>, <stddef.h>.
 */

#include "gui/taskbar.h"
#include "gui/window.h"
#include "gui/start_menu.h"
#include "gfx/framebuffer.h"
#include "gfx/colors.h"
#include "gfx/font.h"
#include <stdint.h>
#include <stdbool.h>

extern volatile uint64_t g_ticks;

static uint32_t g_screen_w = 800;
static uint32_t g_screen_h = 600;

/* Orb geometry */
#define ORB_RADIUS    17u   /* px — gives ~34 px diameter */
#define ORB_MARGIN_L   8u   /* left edge to orb centre    */

void taskbar_init(uint32_t screen_w, uint32_t screen_h)
{
    g_screen_w = screen_w;
    g_screen_h = screen_h;
}

/* ---- tiny itoa ---- */
static void u64_to_dec(uint64_t v, char *buf, int buflen)
{
    if (buflen < 2) { buf[0] = '\0'; return; }
    char tmp[24]; int i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    while (v && i < (int)sizeof(tmp) - 1) { tmp[i++] = (char)('0' + v % 10u); v /= 10u; }
    int out = 0;
    for (int j = i - 1; j >= 0 && out < buflen - 1; j--) buf[out++] = tmp[j];
    buf[out] = '\0';
}

static void format_uptime(char *buf, int buflen)
{
    uint64_t secs = g_ticks / 1000u;
    uint64_t mm = secs / 60u, ss = secs % 60u;
    char tmp[16]; int pos = 0, i = 0;
    u64_to_dec(mm, tmp, (int)sizeof(tmp));
    while (tmp[i] && pos < buflen - 1) buf[pos++] = tmp[i++];
    if (pos < buflen - 1) buf[pos++] = ':';
    if (ss < 10 && pos < buflen - 1) buf[pos++] = '0';
    char stmp[8]; u64_to_dec(ss, stmp, (int)sizeof(stmp));
    i = 0;
    while (stmp[i] && pos < buflen - 1) buf[pos++] = stmp[i++];
    buf[pos] = '\0';
}

static uint32_t kstrlen(const char *s)
{
    uint32_t n = 0;
    while (s && s[n]) n++;
    return n;
}

/* ---- Draw the Win7 pearl orb ---- */
static void draw_start_orb(uint32_t orb_cx, uint32_t orb_cy, bool pressed)
{
    /* Outer fill */
    uint32_t fill = pressed ? UI_COLOR_ACCENT : 0xFF005FB8u; /* slightly darker when not pressed */
    fb_fill_circle(orb_cx, orb_cy, ORB_RADIUS, fill);

    /* Bright highlight ring in the upper-left quadrant — simulates glass bead */
    uint32_t highlight = 0xFF60A8F0u; /* light blue highlight */
    fb_draw_circle(orb_cx - 3u, orb_cy - 4u, ORB_RADIUS - 4u, highlight);

    /* Outer rim */
    fb_draw_circle(orb_cx, orb_cy, ORB_RADIUS, UI_COLOR_ACCENT);

    /* "A" glyph for AIOS in the centre */
    /* (We can't easily centre-text a circle, so skip font rendering here
       and just rely on the coloured orb as a visual landmark.) */
}

void taskbar_draw(framebuffer_t *fb, const gui_font_t *font)
{
    if (!fb) return;

    uint32_t bar_y = g_screen_h - TASKBAR_HEIGHT;

    /* Background */
    fb_fill_rect(0, (int32_t)bar_y, g_screen_w, TASKBAR_HEIGHT, UI_COLOR_TASKBAR_BG);
    /* Top border accent line */
    fb_fill_rect(0, (int32_t)bar_y, g_screen_w, 1u, UI_COLOR_ACCENT);

    /* ---- Start orb ---- */
    uint32_t orb_cx = ORB_MARGIN_L + ORB_RADIUS;
    uint32_t orb_cy = bar_y + TASKBAR_HEIGHT / 2u;
    bool orb_pressed = start_menu_is_open();
    draw_start_orb(orb_cx, orb_cy, orb_pressed);

    if (!font) return;

    uint32_t text_y = bar_y + (TASKBAR_HEIGHT - font->height) / 2u;

    /* ---- Window buttons (start after orb) ---- */
    uint32_t btn_x = ORB_MARGIN_L + 2u * ORB_RADIUS + TASKBAR_PADDING;
    gui_window_t *tail = gui_window_list_head();
    while (tail && tail->next) tail = tail->next;
    gui_window_t *cur = tail;
    while (cur) {
        if (cur->state != GUI_WINDOW_STATE_HIDDEN) {
            uint32_t btn_bg = cur->is_active ? UI_COLOR_ACCENT
                                             : UI_COLOR_WINDOW_TITLE_INACTIVE_BG;
            fb_fill_rect((int32_t)btn_x,
                         (int32_t)(bar_y + TASKBAR_PADDING),
                         TASKBAR_BTN_W,
                         TASKBAR_HEIGHT - 2u * TASKBAR_PADDING,
                         btn_bg);
            fb_draw_rect((int32_t)btn_x,
                         (int32_t)(bar_y + TASKBAR_PADDING),
                         TASKBAR_BTN_W,
                         TASKBAR_HEIGHT - 2u * TASKBAR_PADDING,
                         UI_COLOR_WINDOW_BORDER);
            font_draw_string_centered(fb, font,
                                      (int32_t)btn_x, (int32_t)text_y,
                                      TASKBAR_BTN_W,
                                      cur->title ? cur->title : "?",
                                      UI_COLOR_TEXT_FG, btn_bg);
            btn_x += TASKBAR_BTN_W + TASKBAR_PADDING;
        }
        cur = cur->prev;
    }

    /* ---- Uptime clock (right side) ---- */
    char uptime[16];
    format_uptime(uptime, (int)sizeof(uptime));
    uint32_t cw  = kstrlen(uptime) * (font->width ? font->width : 8u) + 8u;
    uint32_t clk_x = g_screen_w - cw - TASKBAR_PADDING;
    font_draw_string_centered(fb, font,
                              (int32_t)clk_x, (int32_t)text_y,
                              cw, uptime,
                              UI_COLOR_TEXT_FG, UI_COLOR_TASKBAR_BG);
}

bool taskbar_handle_mouse_down(int32_t px, int32_t py,
                               framebuffer_t *fb, const gui_font_t *font)
{
    (void)fb; (void)font;
    uint32_t bar_y = g_screen_h - TASKBAR_HEIGHT;
    if (py < (int32_t)bar_y) return false;

    /* Orb hit-test (circle) */
    uint32_t orb_cx = ORB_MARGIN_L + ORB_RADIUS;
    uint32_t orb_cy = bar_y + TASKBAR_HEIGHT / 2u;
    int32_t  dx     = px - (int32_t)orb_cx;
    int32_t  dy     = py - (int32_t)orb_cy;
    if ((uint32_t)(dx*dx + dy*dy) <= ORB_RADIUS * ORB_RADIUS) {
        start_menu_toggle();
        return true;
    }

    /* Window buttons */
    uint32_t btn_x = ORB_MARGIN_L + 2u * ORB_RADIUS + TASKBAR_PADDING;
    gui_window_t *tail = gui_window_list_head();
    while (tail && tail->next) tail = tail->next;
    gui_window_t *cur = tail;
    while (cur) {
        if (cur->state != GUI_WINDOW_STATE_HIDDEN) {
            if (px >= (int32_t)btn_x &&
                px <  (int32_t)(btn_x + TASKBAR_BTN_W)) {
                if (cur->state == GUI_WINDOW_STATE_MINIMIZED)
                    cur->state = GUI_WINDOW_STATE_NORMAL;
                gui_window_t *w = gui_window_list_head();
                while (w) { w->is_active = false; w = w->next; }
                gui_window_bring_to_front(cur);
                cur->is_active = true;
                return true;
            }
            btn_x += TASKBAR_BTN_W + TASKBAR_PADDING;
        }
        cur = cur->prev;
    }

    return true;
}

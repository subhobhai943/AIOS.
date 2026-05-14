/* kernel/gui/taskbar.c
 * Taskbar — Phase 10.5
 *
 * Bottom-of-screen taskbar with:
 *   - Start button (opens/closes start menu)
 *   - Per-window buttons (click to focus / restore)
 *   - System uptime / tick counter on the right
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

/* Provided by pit.c */
extern volatile uint64_t g_ticks;

static uint32_t g_screen_w = 800;
static uint32_t g_screen_h = 600;

void taskbar_init(uint32_t screen_w, uint32_t screen_h)
{
    g_screen_w = screen_w;
    g_screen_h = screen_h;
}

/* ---- tiny itoa for tick display ---- */
static void u64_to_dec(uint64_t v, char *buf, int buflen)
{
    if (buflen < 2) { buf[0] = '\0'; return; }
    char tmp[24];
    int i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    while (v && i < (int)sizeof(tmp) - 1) {
        tmp[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    /* reverse */
    int out = 0;
    for (int j = i - 1; j >= 0 && out < buflen - 1; j--)
        buf[out++] = tmp[j];
    buf[out] = '\0';
}

/* ---- format uptime as  MM:SS ---- */
static void format_uptime(char *buf, int buflen)
{
    uint64_t secs = g_ticks / 1000u;
    uint64_t mm   = secs / 60u;
    uint64_t ss   = secs % 60u;
    /* "MMM:SS" - brute force */
    char tmp[16];
    int pos = 0;
    u64_to_dec(mm, tmp, (int)sizeof(tmp));
    int i = 0;
    while (tmp[i] && pos < buflen - 1) buf[pos++] = tmp[i++];
    if (pos < buflen - 1) buf[pos++] = ':';
    /* two-digit seconds */
    if (ss < 10 && pos < buflen - 1) buf[pos++] = '0';
    char stmp[8]; u64_to_dec(ss, stmp, (int)sizeof(stmp));
    i = 0;
    while (stmp[i] && pos < buflen - 1) buf[pos++] = stmp[i++];
    buf[pos] = '\0';
}

/* ---- simple strlen for static strings ---- */
static uint32_t kstrlen(const char *s)
{
    uint32_t n = 0;
    while (s && s[n]) n++;
    return n;
}

void taskbar_draw(framebuffer_t *fb, const gui_font_t *font)
{
    if (!fb) return;

    uint32_t bar_y = g_screen_h - TASKBAR_HEIGHT;

    /* Background */
    fb_fill_rect(0, (int32_t)bar_y, g_screen_w, TASKBAR_HEIGHT, UI_COLOR_TASKBAR_BG);
    /* Top border */
    fb_fill_rect(0, (int32_t)bar_y, g_screen_w, 1u, UI_COLOR_ACCENT);

    if (!font) return;

    uint32_t text_y = bar_y + (TASKBAR_HEIGHT - font->height) / 2u;

    /* ---- Start button ---- */
    uint32_t start_pressed = start_menu_is_open() ? 1u : 0u;
    uint32_t start_bg = start_pressed ? UI_COLOR_ACCENT : UI_COLOR_WINDOW_TITLE_ACTIVE_BG;
    fb_fill_rect(TASKBAR_PADDING, (int32_t)(bar_y + TASKBAR_PADDING),
                 TASKBAR_START_W, TASKBAR_HEIGHT - 2u * TASKBAR_PADDING,
                 start_bg);
    fb_draw_rect(TASKBAR_PADDING, (int32_t)(bar_y + TASKBAR_PADDING),
                 TASKBAR_START_W, TASKBAR_HEIGHT - 2u * TASKBAR_PADDING,
                 UI_COLOR_ACCENT);
    font_draw_string_centered(fb, font,
                              (int32_t)TASKBAR_PADDING, (int32_t)text_y,
                              TASKBAR_START_W,
                              "Start",
                              UI_COLOR_TEXT_FG, start_bg);

    /* ---- Window buttons ---- */
    uint32_t btn_x = TASKBAR_PADDING + TASKBAR_START_W + TASKBAR_PADDING;
    gui_window_t *win = gui_window_list_head();
    /* Traverse to tail (last = back-most) then iterate forward */
    gui_window_t *tail = win;
    while (tail && tail->next) tail = tail->next;
    /* Draw from back to front so foreground window button appears last */
    gui_window_t *cur = tail;
    while (cur) {
        if (cur->state != GUI_WINDOW_STATE_HIDDEN) {
            uint32_t btn_bg = cur->is_active ? UI_COLOR_ACCENT
                                             : UI_COLOR_WINDOW_TITLE_INACTIVE_BG;
            fb_fill_rect((int32_t)btn_x, (int32_t)(bar_y + TASKBAR_PADDING),
                         TASKBAR_BTN_W,
                         TASKBAR_HEIGHT - 2u * TASKBAR_PADDING,
                         btn_bg);
            fb_draw_rect((int32_t)btn_x, (int32_t)(bar_y + TASKBAR_PADDING),
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
    uint32_t clock_w = kstrlen(uptime) * (font->width ? font->width : 8u) + 8u;
    uint32_t clock_x = g_screen_w - clock_w - TASKBAR_PADDING;
    font_draw_string_centered(fb, font,
                              (int32_t)clock_x, (int32_t)text_y,
                              clock_w,
                              uptime,
                              UI_COLOR_TEXT_FG, UI_COLOR_TASKBAR_BG);
}

bool taskbar_handle_mouse_down(int32_t px, int32_t py,
                               framebuffer_t *fb, const gui_font_t *font)
{
    (void)fb; (void)font;

    uint32_t bar_y = g_screen_h - TASKBAR_HEIGHT;
    if (py < (int32_t)bar_y) return false;  /* not in taskbar strip */

    /* Start button? */
    if (px >= (int32_t)TASKBAR_PADDING &&
        px <  (int32_t)(TASKBAR_PADDING + TASKBAR_START_W)) {
        start_menu_toggle();
        return true;
    }

    /* Window buttons */
    uint32_t btn_x = TASKBAR_PADDING + TASKBAR_START_W + TASKBAR_PADDING;
    gui_window_t *cur = gui_window_list_head();
    /* mirror draw order: tail to head */
    gui_window_t *tail = cur;
    while (tail && tail->next) tail = tail->next;
    cur = tail;
    while (cur) {
        if (cur->state != GUI_WINDOW_STATE_HIDDEN) {
            if (px >= (int32_t)btn_x &&
                px <  (int32_t)(btn_x + TASKBAR_BTN_W)) {
                if (cur->state == GUI_WINDOW_STATE_MINIMIZED) {
                    cur->state = GUI_WINDOW_STATE_NORMAL;
                }
                /* Deactivate all, bring this to front */
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

    return true; /* consumed (click was in taskbar area regardless) */
}

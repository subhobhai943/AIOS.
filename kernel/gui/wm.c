/* kernel/gui/wm.c
 * Window manager thread — Phase 10.4/10.5/10.6 / Win7 pass
 *
 * Fix pass (Phase 11 bugfix):
 *  - MOUSE_MOVE always marks dirty so cursor tracks the pointer
 *    smoothly (previously cursor froze when no drag was active).
 *  - sched_sleep(8) throttle caps redraws at ~120 fps.
 *  - wm_open_default_apps cascade now walks head→next (not tail→prev)
 *    so both windows get correctly staggered positions.
 *  - wm_open_default_apps opens only terminal + explorer on boot.
 */

#include "gui/wm.h"
#include "gui/desktop.h"
#include "gui/taskbar.h"
#include "gui/start_menu.h"
#include "gfx/framebuffer.h"
#include "gfx/colors.h"
#include "gfx/font.h"
#include "gui/input.h"
#include "gui/window.h"
#include "apps/ai_chat.h"
#include "apps/explorer.h"
#include "apps/notepad.h"
#include "apps/settings.h"
#include "apps/terminal_gui.h"
#include "kthread.h"
#include "sched.h"
#include "sync.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define TITLE_BAR_H    20u
#define BORDER_W        4u
#define TASKBAR_H      40u
#define WIN_RADIUS      4u

#define BTN_W          16u
#define BTN_H          14u
#define BTN_PAD         2u
#define BTN_TOP         3u

#define BTN_CLOSE_COL   0xFFD9534Fu
#define BTN_MAX_COL     0xFF5CB85Cu
#define BTN_MIN_COL     0xFFF0AD4Eu
#define BTN_GLYPH_COL   0xFFFFFFFFu

#define CASCADE_STEP   24u

/* Redraw throttle: minimum ms between full redraws when dirty. */
#define WM_FRAME_MS     8u   /* ~120 fps cap */

/* ------------------------------------------------------------------ */
/* Internal state                                                      */
/* ------------------------------------------------------------------ */

typedef enum { DRAG_NONE = 0, DRAG_MOVE, DRAG_RESIZE } drag_mode_t;

typedef struct {
    framebuffer_t    *fb;
    const gui_font_t *font;
    drag_mode_t       drag_mode;
    gui_window_t     *drag_win;
    int32_t           drag_origin_x;
    int32_t           drag_origin_y;
    int32_t           win_origin_x;
    int32_t           win_origin_y;
    uint32_t          win_origin_w;
    uint32_t          win_origin_h;
    bool              dirty;
} wm_state_t;

/* ------------------------------------------------------------------ */
/* Control-button geometry helpers                                     */
/* ------------------------------------------------------------------ */

static inline int32_t btn_x(const gui_window_t *w, uint32_t k)
{
    return (int32_t)(w->x + w->width)
           - (int32_t)BTN_PAD
           - (int32_t)((k + 1u) * (BTN_W + BTN_PAD));
}
static inline int32_t btn_y(const gui_window_t *w)
{
    return (int32_t)w->y + (int32_t)BTN_TOP;
}

static bool point_in_btn(const gui_window_t *w, uint32_t k,
                         int32_t px, int32_t py)
{
    int32_t bx = btn_x(w, k);
    int32_t by = btn_y(w);
    return px >= bx && px < bx + (int32_t)BTN_W &&
           py >= by && py < by + (int32_t)BTN_H;
}

static bool point_in_title_bar(const gui_window_t *w, int32_t px, int32_t py)
{
    return px >= w->x && px < (int32_t)(w->x + w->width) &&
           py >= w->y && py < (int32_t)(w->y + TITLE_BAR_H);
}

static bool point_in_border(const gui_window_t *w, int32_t px, int32_t py)
{
    int32_t rx = (int32_t)(w->x + w->width  - BORDER_W);
    int32_t ry = (int32_t)(w->y + w->height - BORDER_W);
    return px >= rx && px < (int32_t)(w->x + w->width) &&
           py >= ry && py < (int32_t)(w->y + w->height);
}

/* ------------------------------------------------------------------ */
/* Draw a single control button with glyph                            */
/* ------------------------------------------------------------------ */

static void draw_btn(framebuffer_t *fb, const gui_font_t *font,
                     int32_t bx, int32_t by,
                     uint32_t bg, const char *glyph)
{
    fb_fill_rounded_rect((uint32_t)bx, (uint32_t)by, BTN_W, BTN_H, 2u, bg);
    if (font && glyph) {
        uint32_t gx = (uint32_t)bx + (BTN_W - (font->width ? font->width : 8u)) / 2u;
        uint32_t gy = (uint32_t)by + (BTN_H - (font->height ? font->height : 8u)) / 2u;
        font_draw_string(fb, font, gx, gy, glyph, BTN_GLYPH_COL, bg);
    }
}

/* ------------------------------------------------------------------ */
/* Window frame renderer                                               */
/* ------------------------------------------------------------------ */

static void wm_draw_window_frame(const gui_window_t *win,
                                 framebuffer_t *fb,
                                 const gui_font_t *font)
{
    if (!win || !fb) return;
    if (win->state == GUI_WINDOW_STATE_MINIMIZED ||
        win->state == GUI_WINDOW_STATE_HIDDEN) return;

    uint32_t x = (uint32_t)win->x;
    uint32_t y = (uint32_t)win->y;
    uint32_t w = win->width;
    uint32_t h = win->height;
    if (w < 60u || h < TITLE_BAR_H + 4u) return;

    uint32_t frame_col = win->is_active ? UI_COLOR_ACCENT : UI_COLOR_WINDOW_BORDER;
    uint32_t title_bg  = win->is_active ? UI_COLOR_WINDOW_TITLE_ACTIVE_BG
                                        : UI_COLOR_WINDOW_TITLE_INACTIVE_BG;
    uint32_t body_bg   = UI_COLOR_WINDOW_BG;

    fb_draw_rounded_rect(x, y, w, h, WIN_RADIUS, frame_col);
    fb_fill_rect(x + 1u, y + 1u, w - 2u, TITLE_BAR_H, title_bg);
    fb_fill_rect(x + 1u, y + 1u + TITLE_BAR_H, w - 2u,
                 h - TITLE_BAR_H - 2u, body_bg);

    if (font && win->title) {
        uint32_t title_max_w = w > (3u * (BTN_W + BTN_PAD) + 12u + 8u)
                               ? w - (3u * (BTN_W + BTN_PAD) + 12u + 8u)
                               : 0u;
        if (title_max_w > 0u) {
            font_draw_string_centered(fb, font,
                                      (int32_t)x + 4, (int32_t)y + 2,
                                      title_max_w,
                                      win->title,
                                      UI_COLOR_TEXT_FG, title_bg);
        }
    }

    draw_btn(fb, font, btn_x(win, 0), btn_y(win), BTN_CLOSE_COL, "x");
    draw_btn(fb, font, btn_x(win, 1), btn_y(win), BTN_MAX_COL,   "+");
    draw_btn(fb, font, btn_x(win, 2), btn_y(win), BTN_MIN_COL,   "-");

    fb_fill_rect(x + w - BORDER_W, y + h - BORDER_W,
                 BORDER_W, BORDER_W, frame_col);
}

/* ------------------------------------------------------------------ */
/* Cursor renderer                                                     */
/* ------------------------------------------------------------------ */

static void wm_draw_cursor(framebuffer_t *fb, int32_t cx, int32_t cy)
{
    (void)fb;
    static const uint8_t arrow[12] = {1,2,3,4,5,6,7,6,5,4,3,2};
    uint32_t col = UI_COLOR_TEXT_FG;
    for (int row = 0; row < 12; row++)
        for (int c = 0; c < (int)arrow[row]; c++)
            fb_put_pixel((uint32_t)(cx + c), (uint32_t)(cy + row), col);
}

/* ------------------------------------------------------------------ */
/* Full redraw                                                         */
/* ------------------------------------------------------------------ */

static void wm_redraw_all(wm_state_t *st)
{
    framebuffer_t    *fb   = st->fb;
    const gui_font_t *font = st->font;

    desktop_draw(fb, font);

    gui_window_t *tail = gui_window_list_head();
    while (tail && tail->next) tail = tail->next;
    gui_window_t *cur = tail;
    while (cur) {
        if (cur->state != GUI_WINDOW_STATE_MINIMIZED &&
            cur->state != GUI_WINDOW_STATE_HIDDEN) {
            wm_draw_window_frame(cur, fb, font);
            if (cur->draw) cur->draw(cur, fb);
        }
        cur = cur->prev;
    }

    taskbar_draw(fb, font);

    if (start_menu_is_open())
        start_menu_draw(fb, font);

    gui_mouse_state_t ms;
    gui_input_get_mouse_state(&ms);
    wm_draw_cursor(fb, ms.x, ms.y);

    st->dirty = false;
}

/* ------------------------------------------------------------------ */
/* Default app cascade — only terminal + explorer on boot             */
/* ------------------------------------------------------------------ */

static void wm_open_default_apps(void)
{
    /* Only open the two apps that make sense on a clean boot.
     * Settings / AI Chat / Notepad open from the Start Menu. */
    terminal_gui_open();
    explorer_open(0);

    /*
     * FIX: walk head→next (front-to-back) not tail→prev.
     * gui_window_list_head() returns the most-recently-created window
     * (the front/top of the stack).  prev from the tail is NULL, so
     * the old loop only ever ran once, leaving all windows stacked at
     * the same (60,50) position.
     */
    uint32_t step = 0u;
    gui_window_t *cur = gui_window_list_head();
    while (cur) {
        cur->x = (int32_t)(60u + step * CASCADE_STEP);
        cur->y = (int32_t)(60u + step * CASCADE_STEP);
        step++;
        cur = cur->next;
    }
}

/* ------------------------------------------------------------------ */
/* Event dispatch                                                      */
/* ------------------------------------------------------------------ */

static void wm_handle_event(wm_state_t *st, const gui_event_t *ev)
{
    framebuffer_t    *fb   = st->fb;
    const gui_font_t *font = st->font;

    if (start_menu_is_open()) {
        start_menu_handle_event(ev, fb, font);
        st->dirty = true;
        return;
    }

    /* ---- Left mouse button down ---- */
    if (ev->type == GUI_EVENT_MOUSE_DOWN &&
        (ev->buttons & GUI_MOUSE_BUTTON_LEFT)) {

        st->dirty = true;

        if (taskbar_handle_mouse_down(ev->x, ev->y, fb, font))
            return;

        gui_window_t *win = gui_window_list_head();
        gui_window_t *hit = 0;
        while (win) {
            if (win->state != GUI_WINDOW_STATE_MINIMIZED &&
                win->state != GUI_WINDOW_STATE_HIDDEN &&
                gui_window_contains_point(win, ev->x, ev->y)) {
                hit = win;
                break;
            }
            win = win->next;
        }

        if (hit) {
            gui_window_t *w = gui_window_list_head();
            while (w) { w->is_active = false; w = w->next; }
            gui_window_bring_to_front(hit);
            hit->is_active = true;

            if (point_in_btn(hit, 0, ev->x, ev->y)) {
                hit->state = GUI_WINDOW_STATE_HIDDEN;
                return;
            }
            if (point_in_btn(hit, 1, ev->x, ev->y)) {
                static int32_t  saved_x, saved_y;
                static uint32_t saved_w, saved_h;
                framebuffer_t *fb2 = fb_get();
                if (hit->x == 0 && hit->y == 0 &&
                    hit->width  == fb2->width &&
                    hit->height == fb2->height - TASKBAR_H) {
                    hit->x      = saved_x;
                    hit->y      = saved_y;
                    hit->width  = saved_w;
                    hit->height = saved_h;
                } else {
                    saved_x = hit->x;  saved_y = hit->y;
                    saved_w = hit->width; saved_h = hit->height;
                    hit->x = 0; hit->y = 0;
                    hit->width  = fb2->width;
                    hit->height = fb2->height - TASKBAR_H;
                }
                return;
            }
            if (point_in_btn(hit, 2, ev->x, ev->y)) {
                hit->state     = GUI_WINDOW_STATE_MINIMIZED;
                hit->is_active = false;
                return;
            }

            if (point_in_border(hit, ev->x, ev->y)) {
                st->drag_mode     = DRAG_RESIZE;
                st->drag_win      = hit;
                st->drag_origin_x = ev->x;
                st->drag_origin_y = ev->y;
                st->win_origin_w  = hit->width;
                st->win_origin_h  = hit->height;
                return;
            }

            if (point_in_title_bar(hit, ev->x, ev->y)) {
                st->drag_mode     = DRAG_MOVE;
                st->drag_win      = hit;
                st->drag_origin_x = ev->x;
                st->drag_origin_y = ev->y;
                st->win_origin_x  = hit->x;
                st->win_origin_y  = hit->y;
                return;
            }

            if (hit->handle_event) hit->handle_event(hit, ev);
            return;
        }

        desktop_handle_mouse_down(ev->x, ev->y, fb, font);
        return;
    }

    /* ---- Mouse move ----
     * FIX: always mark dirty on MOUSE_MOVE so the cursor visually
     * follows the pointer even when no drag is active.  The
     * sched_sleep(WM_FRAME_MS) throttle after each redraw keeps CPU
     * usage reasonable (~120 fps max). */
    if (ev->type == GUI_EVENT_MOUSE_MOVE) {
        st->dirty = true;   /* cursor must always track the pointer */
        if (st->drag_mode == DRAG_MOVE && st->drag_win) {
            int32_t dx = ev->x - st->drag_origin_x;
            int32_t dy = ev->y - st->drag_origin_y;
            st->drag_win->x = st->win_origin_x + dx;
            st->drag_win->y = st->win_origin_y + dy;
            if (st->drag_win->y < 0) st->drag_win->y = 0;
            if (st->drag_win->y >= (int32_t)(fb->height - TASKBAR_H))
                st->drag_win->y = (int32_t)(fb->height - TASKBAR_H) - 1;
        } else if (st->drag_mode == DRAG_RESIZE && st->drag_win) {
            int32_t dx  = ev->x - st->drag_origin_x;
            int32_t dy  = ev->y - st->drag_origin_y;
            int32_t n_w = (int32_t)st->win_origin_w + dx;
            int32_t n_h = (int32_t)st->win_origin_h + dy;
            if (n_w < 80)  n_w = 80;
            if (n_h < 40)  n_h = 40;
            st->drag_win->width  = (uint32_t)n_w;
            st->drag_win->height = (uint32_t)n_h;
        }
        return;
    }

    /* ---- Mouse up ---- */
    if (ev->type == GUI_EVENT_MOUSE_UP) {
        if (st->drag_mode != DRAG_NONE) {
            st->drag_mode = DRAG_NONE;
            st->drag_win  = 0;
            st->dirty     = true;
        }
        gui_window_t *head = gui_window_list_head();
        if (head && head->is_active && head->handle_event)
            head->handle_event(head, ev);
        return;
    }

    /* ---- Key events ---- */
    if (ev->type == GUI_EVENT_KEY_DOWN || ev->type == GUI_EVENT_KEY_UP) {
        gui_window_t *head = gui_window_list_head();
        if (head && head->is_active && head->handle_event) {
            head->handle_event(head, ev);
            st->dirty = true;
        }
    }
}

/* ------------------------------------------------------------------ */
/* WM thread                                                           */
/* ------------------------------------------------------------------ */

static void wm_thread_main(void *arg)
{
    (void)arg;
    framebuffer_t *fb = fb_get();
    if (!fb) return;

    const gui_font_t *font = font_load_builtin();

    gui_input_init(fb->width, fb->height);
    gui_window_system_init();
    desktop_init();
    taskbar_init(fb->width, fb->height);
    start_menu_init();
    wm_open_default_apps();

    wm_state_t st = {0};
    st.fb    = fb;
    st.font  = font;
    st.dirty = true;

    wm_redraw_all(&st);

    gui_event_t ev;
    for (;;) {
        gui_input_wait_event(&ev);
        wm_handle_event(&st, &ev);
        if (st.dirty) {
            wm_redraw_all(&st);
            /* Throttle: yield for ~8 ms after each redraw so the CPU
             * isn't hammered and mouse events don't pile up. */
            sched_sleep(WM_FRAME_MS);
        }
    }
}

void gui_wm_start(void)
{
    kthread_create(wm_thread_main, 0, 65536, "gui_wm");
}

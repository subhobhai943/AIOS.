/* kernel/gui/wm.c
 * Window manager thread — Phase 10.4/10.5/10.6
 *
 * New in this revision:
 *  - Proper title-bar drag and border-resize state machine.
 *  - Desktop background drawn by desktop.c.
 *  - Taskbar drawn by taskbar.c (reserves bottom 40 px).
 *  - Start-menu popup handled here via start_menu.c.
 *  - Mouse cursor drawn as a small arrow on top of everything.
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
#include "kthread.h"
#include "sync.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define TITLE_BAR_H   20u   /* pixels */
#define BORDER_W       4u   /* pixels — resize grip thickness */
#define TASKBAR_H     40u   /* pixels — reserved at bottom    */

/* ------------------------------------------------------------------ */
/* Internal state                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    DRAG_NONE = 0,
    DRAG_MOVE,
    DRAG_RESIZE,
} drag_mode_t;

typedef struct {
    framebuffer_t  *fb;
    const gui_font_t *font;

    /* Drag / resize state */
    drag_mode_t     drag_mode;
    gui_window_t   *drag_win;
    int32_t         drag_origin_x;   /* mouse x when drag started */
    int32_t         drag_origin_y;
    int32_t         win_origin_x;    /* window x when drag started */
    int32_t         win_origin_y;
    uint32_t        win_origin_w;
    uint32_t        win_origin_h;
} wm_state_t;

/* ------------------------------------------------------------------ */
/* Title-bar / border geometry helpers                                  */
/* ------------------------------------------------------------------ */

static bool point_in_title_bar(const gui_window_t *w, int32_t px, int32_t py)
{
    return px >= w->x && px < (int32_t)(w->x + w->width) &&
           py >= w->y && py < (int32_t)(w->y + TITLE_BAR_H);
}

static bool point_in_border(const gui_window_t *w, int32_t px, int32_t py)
{
    /* bottom-right corner resize grip */
    int32_t rx = (int32_t)(w->x + w->width  - (int32_t)BORDER_W);
    int32_t ry = (int32_t)(w->y + w->height - (int32_t)BORDER_W);
    return px >= rx && px < (int32_t)(w->x + w->width) &&
           py >= ry && py < (int32_t)(w->y + w->height);
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
    if (w < 20u || h < TITLE_BAR_H + 4u) return;

    uint32_t frame_col  = win->is_active ? UI_COLOR_ACCENT : UI_COLOR_WINDOW_BORDER;
    uint32_t title_bg   = win->is_active ? UI_COLOR_WINDOW_TITLE_ACTIVE_BG
                                         : UI_COLOR_WINDOW_TITLE_INACTIVE_BG;
    uint32_t title_fg   = UI_COLOR_TEXT_FG;
    uint32_t body_bg    = UI_COLOR_WINDOW_BG;

    /* Outer border */
    fb_draw_rect(x, y, w, h, frame_col);

    /* Title bar */
    fb_fill_rect(x + 1u, y + 1u, w - 2u, TITLE_BAR_H, title_bg);

    /* Body */
    fb_fill_rect(x + 1u, y + 1u + TITLE_BAR_H, w - 2u,
                 h - TITLE_BAR_H - 2u, body_bg);

    /* Title text */
    if (font && win->title) {
        font_draw_string_centered(fb, font, (int32_t)x + 4, (int32_t)y + 2,
                                  w - 8u, win->title, title_fg, title_bg);
    }

    /* Resize grip — small darker square at bottom-right */
    fb_fill_rect(x + w - BORDER_W, y + h - BORDER_W,
                 BORDER_W, BORDER_W, frame_col);
}

/* ------------------------------------------------------------------ */
/* Cursor renderer (tiny software arrow)                               */
/* ------------------------------------------------------------------ */

static void wm_draw_cursor(framebuffer_t *fb, int32_t cx, int32_t cy)
{
    /* 8-pixel tall left-pointing filled triangle arrow */
    static const uint8_t arrow[8] = { 1,2,3,4,5,6,7,8 };
    uint32_t col = UI_COLOR_TEXT_FG;
    for (int row = 0; row < 8; row++) {
        for (int col2 = 0; col2 < (int)arrow[row]; col2++) {
            fb_put_pixel((uint32_t)(cx + col2), (uint32_t)(cy + row), col);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Full redraw                                                          */
/* ------------------------------------------------------------------ */

static void wm_redraw_all(wm_state_t *st)
{
    framebuffer_t *fb = st->fb;
    const gui_font_t *font = st->font;

    /* 1. Desktop background */
    desktop_draw(fb, font);

    /* 2. Windows — back to front */
    /* Collect list tail first (back is last node in list) */
    gui_window_t *win = gui_window_list_head();
    /* list is front-to-back, draw back-to-front = reverse iteration */
    /* Find tail */
    gui_window_t *tail = win;
    while (tail && tail->next) tail = tail->next;
    /* Draw from tail to head */
    gui_window_t *cur = tail;
    while (cur) {
        if (cur->state != GUI_WINDOW_STATE_MINIMIZED &&
            cur->state != GUI_WINDOW_STATE_HIDDEN) {
            wm_draw_window_frame(cur, fb, font);
            if (cur->draw) cur->draw(cur, fb);
        }
        cur = cur->prev;
    }

    /* 3. Taskbar (on top of windows, at bottom) */
    taskbar_draw(fb, font);

    /* 4. Start menu if open */
    if (start_menu_is_open()) {
        start_menu_draw(fb, font);
    }

    /* 5. Mouse cursor on top of everything */
    gui_mouse_state_t ms;
    gui_input_get_mouse_state(&ms);
    wm_draw_cursor(fb, ms.x, ms.y);
}

/* ------------------------------------------------------------------ */
/* Event dispatch                                                       */
/* ------------------------------------------------------------------ */

static void wm_handle_event(wm_state_t *st, const gui_event_t *ev)
{
    framebuffer_t *fb = st->fb;
    const gui_font_t *font = st->font;

    /* ---- Start menu consumes events when open ---- */
    if (start_menu_is_open()) {
        start_menu_handle_event(ev, fb, font);
        return;
    }

    if (ev->type == GUI_EVENT_MOUSE_DOWN &&
        (ev->buttons & GUI_MOUSE_BUTTON_LEFT)) {

        /* Check taskbar first (bottom strip) */
        if (taskbar_handle_mouse_down(ev->x, ev->y, fb, font)) {
            return;
        }

        /* Hit-test windows from top (head) to bottom */
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

        /* Deactivate previously-active window */
        gui_window_t *w = gui_window_list_head();
        while (w) { w->is_active = false; w = w->next; }

        if (hit) {
            gui_window_bring_to_front(hit);
            hit->is_active = true;

            if (point_in_border(hit, ev->x, ev->y)) {
                /* Begin resize */
                st->drag_mode    = DRAG_RESIZE;
                st->drag_win     = hit;
                st->drag_origin_x = ev->x;
                st->drag_origin_y = ev->y;
                st->win_origin_w  = hit->width;
                st->win_origin_h  = hit->height;
            } else if (point_in_title_bar(hit, ev->x, ev->y)) {
                /* Begin move */
                st->drag_mode    = DRAG_MOVE;
                st->drag_win     = hit;
                st->drag_origin_x = ev->x;
                st->drag_origin_y = ev->y;
                st->win_origin_x  = hit->x;
                st->win_origin_y  = hit->y;
            } else {
                /* Client area click — forward to window */
                if (hit->handle_event) hit->handle_event(hit, ev);
            }
        }
        return;
    }

    if (ev->type == GUI_EVENT_MOUSE_MOVE) {
        if (st->drag_mode == DRAG_MOVE && st->drag_win) {
            int32_t dx = ev->x - st->drag_origin_x;
            int32_t dy = ev->y - st->drag_origin_y;
            st->drag_win->x = st->win_origin_x + dx;
            st->drag_win->y = st->win_origin_y + dy;
            /* Clamp: don't drag title bar off screen top */
            if (st->drag_win->y < 0) st->drag_win->y = 0;
            if (st->drag_win->y >= (int32_t)(fb->height - TASKBAR_H))
                st->drag_win->y = (int32_t)(fb->height - TASKBAR_H) - 1;
        } else if (st->drag_mode == DRAG_RESIZE && st->drag_win) {
            int32_t dx = ev->x - st->drag_origin_x;
            int32_t dy = ev->y - st->drag_origin_y;
            int32_t new_w = (int32_t)st->win_origin_w + dx;
            int32_t new_h = (int32_t)st->win_origin_h + dy;
            if (new_w < 80)  new_w = 80;
            if (new_h < 40)  new_h = 40;
            st->drag_win->width  = (uint32_t)new_w;
            st->drag_win->height = (uint32_t)new_h;
        }
        return;
    }

    if (ev->type == GUI_EVENT_MOUSE_UP) {
        if (st->drag_mode != DRAG_NONE) {
            st->drag_mode = DRAG_NONE;
            st->drag_win  = 0;
        }
        /* Also forward to focused window */
        gui_window_t *head = gui_window_list_head();
        if (head && head->is_active && head->handle_event)
            head->handle_event(head, ev);
        return;
    }

    /* Key events — forward to active window */
    if (ev->type == GUI_EVENT_KEY_DOWN || ev->type == GUI_EVENT_KEY_UP) {
        gui_window_t *head = gui_window_list_head();
        if (head && head->is_active && head->handle_event)
            head->handle_event(head, ev);
    }
}

/* ------------------------------------------------------------------ */
/* WM thread entry                                                      */
/* ------------------------------------------------------------------ */

static int wm_thread_main(void *arg)
{
    (void)arg;

    framebuffer_t *fb = fb_get();
    if (!fb) return 0;

    const gui_font_t *font = font_load_builtin();

    gui_input_init(fb->width, fb->height);
    gui_window_system_init();
    desktop_init();
    taskbar_init(fb->width, fb->height);
    start_menu_init();

    wm_state_t st;
    st.fb           = fb;
    st.font         = font;
    st.drag_mode    = DRAG_NONE;
    st.drag_win     = 0;
    st.drag_origin_x = 0;
    st.drag_origin_y = 0;
    st.win_origin_x  = 0;
    st.win_origin_y  = 0;
    st.win_origin_w  = 0;
    st.win_origin_h  = 0;

    wm_redraw_all(&st);

    gui_event_t ev;
    for (;;) {
        gui_input_wait_event(&ev);
        wm_handle_event(&st, &ev);
        wm_redraw_all(&st);
    }

    return 0;
}

void gui_wm_start(void)
{
    kthread_create(wm_thread_main, 0, 65536, "gui_wm");
}

#include "gui/wm.h"

#include "gfx/framebuffer.h"
#include "gfx/colors.h"
#include "gfx/font.h"
#include "gui/input.h"
#include "gui/window.h"

#include "kthread.h"

typedef struct wm_state {
    framebuffer_t *fb;
    const gui_font_t *font;
    gui_window_t *test_window;
} wm_state_t;

static void wm_draw_window_frame(gui_window_t *win, framebuffer_t *fb, const gui_font_t *font)
{
    if (!win || !fb) return;

    uint32_t x = (uint32_t)win->x;
    uint32_t y = (uint32_t)win->y;
    uint32_t w = win->width;
    uint32_t h = win->height;

    if (w < 10 || h < 20) return;

    uint32_t frame_color  = win->is_active ? UI_COLOR_ACCENT : UI_COLOR_WINDOW_BORDER;
    uint32_t title_bg     = win->is_active ? UI_COLOR_WINDOW_TITLE_ACTIVE_BG : UI_COLOR_WINDOW_TITLE_INACTIVE_BG;
    uint32_t title_fg     = UI_COLOR_TEXT_FG;
    uint32_t body_bg      = UI_COLOR_WINDOW_BG;

    fb_draw_rect(x, y, w, h, frame_color);

    uint32_t title_bar_h = (font ? font->height + 4u : 20u);
    if (title_bar_h + 4u > h) title_bar_h = h / 3u;

    fb_fill_rect(x + 1, y + 1, w - 2, title_bar_h, title_bg);
    fb_fill_rect(x + 1, y + 1 + title_bar_h, w - 2, h - title_bar_h - 2, body_bg);

    if (font && win->title) {
        uint32_t text_y = y + 2;
        font_draw_string_centered(fb, font, x + 4, text_y, w - 8, win->title, title_fg, title_bg);
    }
}

static void wm_test_window_draw(gui_window_t *win, framebuffer_t *fb)
{
    (void)win;
    (void)fb;
}

static void wm_test_window_event(gui_window_t *win, const gui_event_t *ev)
{
    (void)win;
    (void)ev;
}

static void wm_redraw_all(wm_state_t *st)
{
    framebuffer_t *fb = st->fb;
    const gui_font_t *font = st->font;

    fb_clear(UI_COLOR_DESKTOP_BG);

    uint32_t bar_h = 40u;
    fb_fill_rect(0, 0, fb->width, bar_h, UI_COLOR_TASKBAR_BG);
    fb_draw_rect(0, 0, fb->width, bar_h, UI_COLOR_ACCENT);

    if (font) {
        uint32_t text_y = (bar_h > font->height) ? (bar_h - font->height) / 2u : 0u;
        font_draw_string_centered(fb, font, 0, text_y, fb->width,
                                  "AIOS Desktop — WM test", UI_COLOR_TEXT_FG,
                                  UI_COLOR_TASKBAR_BG);
    }

    gui_window_t *win = gui_window_list_head();
    while (win) {
        wm_draw_window_frame(win, fb, font);
        if (win->draw) {
            win->draw(win, fb);
        }
        win = win->next;
    }
}

static void wm_handle_event(wm_state_t *st, const gui_event_t *ev)
{
    gui_window_t *win = gui_window_list_head();
    gui_window_t *top_hit = 0;

    if (ev->type == GUI_EVENT_MOUSE_DOWN) {
        while (win) {
            if (gui_window_contains_point(win, ev->x, ev->y)) {
                top_hit = win;
                break;
            }
            win = win->next;
        }

        if (top_hit) {
            gui_window_bring_to_front(top_hit);
            top_hit->is_active = true;
        }
    }

    if (top_hit && top_hit->handle_event) {
        top_hit->handle_event(top_hit, ev);
    }
}

static int wm_thread_main(void *arg)
{
    (void)arg;

    framebuffer_t *fb = fb_get();
    if (!fb) {
        return 0;
    }

    const gui_font_t *font = font_load_builtin();

    gui_input_init(fb->width, fb->height);

    gui_window_system_init();

    wm_state_t st;
    st.fb = fb;
    st.font = font;
    st.test_window = gui_create_window(80, 80, 320, 200,
                                       "Test Window",
                                       wm_test_window_draw,
                                       wm_test_window_event,
                                       0);
    if (st.test_window) {
        st.test_window->is_active = true;
    }

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

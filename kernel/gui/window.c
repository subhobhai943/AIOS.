#include "gui/window.h"

#include "heap.h"

static gui_window_t *g_win_head = 0;
static gui_window_t *g_win_tail = 0;
static uint32_t      g_next_window_id = 1;

void gui_window_system_init(void)
{
    g_win_head = 0;
    g_win_tail = 0;
    g_next_window_id = 1;
}

static void gui_window_insert_front(gui_window_t *win)
{
    win->prev = 0;
    win->next = g_win_head;
    if (g_win_head) {
        g_win_head->prev = win;
    }
    g_win_head = win;
    if (!g_win_tail) {
        g_win_tail = win;
    }
}

static void gui_window_remove_from_list(gui_window_t *win)
{
    if (win->prev) {
        win->prev->next = win->next;
    } else {
        g_win_head = win->next;
    }
    if (win->next) {
        win->next->prev = win->prev;
    } else {
        g_win_tail = win->prev;
    }
    win->prev = win->next = 0;
}

gui_window_t *gui_create_window(int32_t x,
                                int32_t y,
                                uint32_t w,
                                uint32_t h,
                                const char *title,
                                gui_window_draw_fn draw_cb,
                                gui_window_event_fn event_cb,
                                void *user_data)
{
    gui_window_t *win = (gui_window_t *)kmalloc(sizeof(gui_window_t));
    if (!win) return 0;

    win->id       = g_next_window_id++;
    win->x        = x;
    win->y        = y;
    win->width    = w;
    win->height   = h;
    win->state    = GUI_WINDOW_STATE_NORMAL;
    win->is_active = false;
    win->title    = title;
    win->draw     = draw_cb;
    win->handle_event = event_cb;
    win->user_data = user_data;
    win->prev     = 0;
    win->next     = 0;

    gui_window_insert_front(win);
    return win;
}

void gui_destroy_window(gui_window_t *win)
{
    if (!win) return;
    gui_window_remove_from_list(win);
    kfree(win);
}

gui_window_t *gui_window_list_head(void)
{
    return g_win_head;
}

void gui_window_bring_to_front(gui_window_t *win)
{
    if (!win || win == g_win_head) return;
    gui_window_remove_from_list(win);
    gui_window_insert_front(win);
}

bool gui_window_contains_point(const gui_window_t *win, int32_t px, int32_t py)
{
    if (!win) return false;
    if (px < win->x) return false;
    if (py < win->y) return false;
    if (px >= (int32_t)(win->x + (int32_t)win->width)) return false;
    if (py >= (int32_t)(win->y + (int32_t)win->height)) return false;
    return true;
}

#ifndef GUI_WINDOW_H
#define GUI_WINDOW_H

#include <stdint.h>
#include <stdbool.h>

#include "gfx/framebuffer.h"
#include "gui/input.h"

/*
 * Minimal window abstraction for the AIOS GUI.
 *
 * The window manager keeps a doubly-linked list of gui_window_t instances,
 * ordered back-to-front by z-order. Application code provides draw and
 * event callbacks; the WM is responsible for hit-testing and focus.
 */

typedef enum gui_window_state {
    GUI_WINDOW_STATE_NORMAL = 0,
    GUI_WINDOW_STATE_MOVING,
    GUI_WINDOW_STATE_RESIZING,
    GUI_WINDOW_STATE_MINIMIZED,
    GUI_WINDOW_STATE_HIDDEN,
} gui_window_state_t;

struct gui_window;
typedef struct gui_window gui_window_t;

typedef void (*gui_window_draw_fn)(gui_window_t *win, framebuffer_t *fb);
typedef void (*gui_window_event_fn)(gui_window_t *win, const gui_event_t *ev);

struct gui_window {
    uint32_t          id;
    int32_t           x;
    int32_t           y;
    uint32_t          width;
    uint32_t          height;
    gui_window_state_t state;
    bool              is_active;
    const char       *title;      /* Pointer owned by caller or static string. */

    gui_window_draw_fn  draw;
    gui_window_event_fn handle_event;
    void               *user_data;

    /* Links used by the window manager for z-ordering. */
    gui_window_t      *prev;
    gui_window_t      *next;
};

/* Reset global window list/IDs. Must be called once at GUI startup. */
void gui_window_system_init(void);

/* Create/destroy windows. Windows are inserted at the front (top-most). */

gui_window_t *gui_create_window(int32_t x,
                                int32_t y,
                                uint32_t w,
                                uint32_t h,
                                const char *title,
                                gui_window_draw_fn draw_cb,
                                gui_window_event_fn event_cb,
                                void *user_data);

void gui_destroy_window(gui_window_t *win);

/* Access to the internal window list for the window manager. */

gui_window_t *gui_window_list_head(void);

/* Bring a window to the front (top-most z-order). */
void gui_window_bring_to_front(gui_window_t *win);

/* Geometry helper for hit-testing. */
bool gui_window_contains_point(const gui_window_t *win, int32_t px, int32_t py);

#endif /* GUI_WINDOW_H */

/* kernel/gui/start_menu.c
 * Start menu — Phase 10.5
 *
 * Shows a vertical list of entries above the Start button.
 * Clicking an entry fires an action callback (launch window).
 *
 * No libc. Allowed: <stdint.h>, <stdbool.h>, <stddef.h>.
 */

#include "gui/start_menu.h"
#include "gui/window.h"
#include "gfx/framebuffer.h"
#include "gfx/colors.h"
#include "gfx/font.h"
#include "gui/input.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- Menu geometry ---- */
#define MENU_X          4u
#define MENU_ITEM_H    28u
#define MENU_W        200u
#define TASKBAR_H_     40u   /* keep in sync with taskbar.h */

/* ---- Forward-declare app launch stubs ---- */
static void launch_terminal(void);
static void launch_notepad(void);
static void launch_explorer(void);
static void launch_settings(void);
static void launch_ai_chat(void);
static void launch_about(void);

typedef void (*menu_action_t)(void);

typedef struct {
    const char     *label;
    menu_action_t   action;
} menu_item_t;

static const menu_item_t g_menu_items[] = {
    { "Terminal",    launch_terminal  },
    { "Notepad",     launch_notepad   },
    { "File Explorer", launch_explorer },
    { "Settings",    launch_settings  },
    { "AI Chat",     launch_ai_chat   },
    { "---------",   NULL             },  /* separator */
    { "About AIOS",  launch_about     },
};

#define MENU_ITEM_COUNT  7u

static bool g_open = false;
static uint32_t g_screen_h = 600;

void start_menu_init(void)
{
    g_open = false;
    /* capture screen height via fb later if needed */
}

void start_menu_toggle(void)
{
    g_open = !g_open;
}

bool start_menu_is_open(void)
{
    return g_open;
}

static uint32_t menu_top_y(const gui_font_t *font)
{
    uint32_t item_h = font ? (font->height + 8u) : MENU_ITEM_H;
    uint32_t menu_h = item_h * MENU_ITEM_COUNT + 4u;
    uint32_t screen_h = 600u; /* default; real height via fb if needed */
    (void)screen_h;
    /* Position: above the taskbar */
    uint32_t fb_h = 600u; /* will be overridden by caller with fb->height */
    (void)fb_h;
    return (uint32_t)((g_screen_h > TASKBAR_H_ + menu_h)
                      ? g_screen_h - TASKBAR_H_ - menu_h
                      : 0u);
}

void start_menu_draw(framebuffer_t *fb, const gui_font_t *font)
{
    if (!fb || !g_open) return;

    /* Update screen height each frame */
    g_screen_h = fb->height;

    uint32_t item_h  = font ? (font->height + 8u) : MENU_ITEM_H;
    uint32_t menu_h  = item_h * MENU_ITEM_COUNT + 4u;
    uint32_t menu_y  = (g_screen_h > TASKBAR_H_ + menu_h)
                       ? g_screen_h - TASKBAR_H_ - menu_h
                       : 0u;

    /* Background */
    fb_fill_rect((int32_t)MENU_X, (int32_t)menu_y,
                 MENU_W, menu_h, UI_COLOR_WINDOW_BG);
    fb_draw_rect((int32_t)MENU_X, (int32_t)menu_y,
                 MENU_W, menu_h, UI_COLOR_ACCENT);

    /* Items */
    for (uint32_t i = 0; i < MENU_ITEM_COUNT; i++) {
        uint32_t iy = menu_y + 2u + i * item_h;
        const char *label = g_menu_items[i].label;

        if (g_menu_items[i].action == NULL) {
            /* Separator */
            fb_fill_rect((int32_t)(MENU_X + 4u), (int32_t)(iy + item_h / 2u),
                         MENU_W - 8u, 1u, UI_COLOR_WINDOW_BORDER);
            continue;
        }

        if (font) {
            font_draw_string_centered(fb, font,
                                      (int32_t)(MENU_X + 4u),
                                      (int32_t)(iy + 4u),
                                      MENU_W - 8u,
                                      label,
                                      UI_COLOR_TEXT_FG,
                                      UI_COLOR_WINDOW_BG);
        }
    }
}

void start_menu_handle_event(const gui_event_t *ev,
                             framebuffer_t *fb,
                             const gui_font_t *font)
{
    if (!g_open) return;

    if (ev->type == GUI_EVENT_KEY_DOWN && ev->keycode == 27u) { /* ESC */
        g_open = false;
        return;
    }

    if (ev->type != GUI_EVENT_MOUSE_DOWN) return;

    g_screen_h = fb ? fb->height : 600u;
    uint32_t item_h = font ? (font->height + 8u) : MENU_ITEM_H;
    uint32_t menu_h = item_h * MENU_ITEM_COUNT + 4u;
    uint32_t menu_y = (g_screen_h > TASKBAR_H_ + menu_h)
                      ? g_screen_h - TASKBAR_H_ - menu_h
                      : 0u;

    /* Click outside menu — close it */
    if (ev->x < (int32_t)MENU_X || ev->x >= (int32_t)(MENU_X + MENU_W) ||
        ev->y < (int32_t)menu_y || ev->y >= (int32_t)(menu_y + menu_h)) {
        g_open = false;
        return;
    }

    /* Which item? */
    int32_t rel_y = ev->y - (int32_t)menu_y - 2;
    if (rel_y < 0) { g_open = false; return; }
    uint32_t idx = (uint32_t)rel_y / item_h;
    if (idx < MENU_ITEM_COUNT && g_menu_items[idx].action) {
        g_open = false;
        g_menu_items[idx].action();
    } else {
        g_open = false;
    }
}

/* ------------------------------------------------------------------ */
/* App launch stubs — will be replaced by real implementations in      */
/* Phase 11.x when the app source files are written.                   */
/* ------------------------------------------------------------------ */

static void draw_stub(gui_window_t *win, framebuffer_t *fb)
{
    if (!win || !fb) return;
    /* Already drawn by WM frame; nothing extra needed for stub */
}

static void event_stub(gui_window_t *win, const gui_event_t *ev)
{
    (void)win; (void)ev;
}

static void launch_terminal(void)
{
    gui_create_window(60, 60, 400, 260, "Terminal",
                      draw_stub, event_stub, NULL);
}

static void launch_notepad(void)
{
    gui_create_window(80, 70, 380, 280, "Notepad",
                      draw_stub, event_stub, NULL);
}

static void launch_explorer(void)
{
    gui_create_window(100, 80, 420, 300, "File Explorer",
                      draw_stub, event_stub, NULL);
}

static void launch_settings(void)
{
    gui_create_window(120, 90, 360, 260, "Settings",
                      draw_stub, event_stub, NULL);
}

static void launch_ai_chat(void)
{
    gui_create_window(140, 100, 400, 320, "AI Chat",
                      draw_stub, event_stub, NULL);
}

static void launch_about(void)
{
    gui_create_window(160, 110, 300, 160, "About AIOS",
                      draw_stub, event_stub, NULL);
}

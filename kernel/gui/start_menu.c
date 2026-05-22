/* kernel/gui/start_menu.c — Phase 10.5
 *
 * Start-menu popup.  Uses *_simple variants of app launchers so they
 * link cleanly against ai_chat_simple.c and notepad_simple.c.
 */

#include "gui/start_menu.h"
#include "apps/ai_chat.h"
#include "apps/explorer.h"
#include "apps/notepad.h"
#include "apps/settings.h"
#include "apps/terminal_gui.h"
#include "gfx/framebuffer.h"
#include "gfx/colors.h"
#include "gfx/font.h"
#include "gui/taskbar.h"
#include "gui/input.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define START_MENU_W      180u
#define START_MENU_ITEM_H  30u
#define START_MENU_PAD      8u

typedef void (*start_menu_action_t)(void);

typedef struct {
    const char          *label;
    start_menu_action_t  action;
} start_menu_item_t;

static bool     g_open   = false;
static uint32_t g_menu_x = TASKBAR_PADDING;
static uint32_t g_menu_y = 0;
static uint32_t g_menu_h = 0;

static void launch_terminal(void)  { terminal_gui_open(); }
static void launch_notepad(void)   { notepad_open(0); }
static void launch_explorer(void)  { explorer_open(0); }
static void launch_settings(void)  { settings_open(); }
static void launch_ai_chat(void)   { ai_chat_open(); }

static const start_menu_item_t g_items[] = {
    { "Terminal",  launch_terminal  },
    { "Notepad",   launch_notepad   },
    { "Explorer",  launch_explorer  },
    { "Settings",  launch_settings  },
    { "AI Chat",   launch_ai_chat   },
};

static uint32_t item_count(void)
{
    return (uint32_t)(sizeof(g_items) / sizeof(g_items[0]));
}

void start_menu_init(void)
{
    g_open   = false;
    g_menu_h = START_MENU_PAD * 2u + item_count() * START_MENU_ITEM_H;
}

void start_menu_toggle(void)  { g_open = !g_open; }
bool start_menu_is_open(void) { return g_open; }

static void recalc_geometry(framebuffer_t *fb)
{
    g_menu_h = START_MENU_PAD * 2u + item_count() * START_MENU_ITEM_H;
    if (fb && fb->height > TASKBAR_HEIGHT + g_menu_h)
        g_menu_y = fb->height - TASKBAR_HEIGHT - g_menu_h;
    else
        g_menu_y = 0;
}

void start_menu_draw(framebuffer_t *fb, const gui_font_t *font)
{
    if (!fb || !g_open) return;
    recalc_geometry(fb);

    fb_fill_rect(g_menu_x, g_menu_y, START_MENU_W, g_menu_h,
                 UI_COLOR_WINDOW_BG);
    fb_draw_rect(g_menu_x, g_menu_y, START_MENU_W, g_menu_h,
                 UI_COLOR_ACCENT);

    if (!font) return;

    for (uint32_t i = 0; i < item_count(); i++) {
        uint32_t iy = g_menu_y + START_MENU_PAD + i * START_MENU_ITEM_H;
        fb_fill_rect(g_menu_x + START_MENU_PAD, iy,
                     START_MENU_W - START_MENU_PAD * 2u,
                     START_MENU_ITEM_H - 2u,
                     UI_COLOR_WINDOW_TITLE_INACTIVE_BG);
        font_draw_string(fb, font,
                         g_menu_x + START_MENU_PAD + 8u,
                         iy + (START_MENU_ITEM_H - font->height) / 2u,
                         g_items[i].label,
                         UI_COLOR_TEXT_FG,
                         UI_COLOR_WINDOW_TITLE_INACTIVE_BG);
    }
}

void start_menu_handle_event(const gui_event_t *ev,
                             framebuffer_t *fb,
                             const gui_font_t *font)
{
    (void)font;
    if (!ev || !g_open) return;
    recalc_geometry(fb);

    if (ev->type != GUI_EVENT_MOUSE_DOWN ||
        (ev->buttons & GUI_MOUSE_BUTTON_LEFT) == 0)
        return;

    /* Click outside → close */
    if (ev->x < (int32_t)g_menu_x ||
        ev->x >= (int32_t)(g_menu_x + START_MENU_W) ||
        ev->y < (int32_t)g_menu_y ||
        ev->y >= (int32_t)(g_menu_y + g_menu_h)) {
        g_open = false;
        return;
    }

    int32_t rel_y = ev->y - (int32_t)(g_menu_y + START_MENU_PAD);
    if (rel_y < 0) return;
    uint32_t idx = (uint32_t)rel_y / START_MENU_ITEM_H;
    if (idx < item_count()) {
        start_menu_action_t action = g_items[idx].action;
        g_open = false;
        if (action) action();
    }
}

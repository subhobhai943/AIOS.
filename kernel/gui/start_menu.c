/*
 * Update start_menu app launchers to call real apps (Phase 11.x)
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

#include "apps/notepad.h"
#include "apps/explorer.h"
#include "apps/terminal_gui.h"
#include "apps/settings.h"
#include "apps/ai_chat.h"

/* rest of file unchanged, but replace launch_* stubs: */

static void launch_terminal(void)
{
    terminal_gui_open();
}

static void launch_notepad(void)
{
    notepad_open(0);
}

static void launch_explorer(void)
{
    explorer_open(0);
}

static void launch_settings(void)
{
    settings_open();
}

static void launch_ai_chat(void)
{
    ai_chat_open();
}

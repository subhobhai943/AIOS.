#ifndef COLORS_H
#define COLORS_H

#include <stdint.h>

/* Basic 32-bit ARGB UI color constants for the GUI.
 *
 * FIX: replaced flat 0xFF202020 desktop bg with deep navy-teal
 * 0xFF1A2A3A that blends correctly with the accent colour in the
 * desktop gradient.  Accent updated to Windows-style blue 0xFF0078D4.
 */

#define UI_COLOR_DESKTOP_BG              0xFF1A2A3Au  /* deep navy-teal */
#define UI_COLOR_WINDOW_BG               0xFF2E2E2Eu
#define UI_COLOR_WINDOW_BORDER           0xFF3C3C3Cu
#define UI_COLOR_WINDOW_TITLE_ACTIVE_BG  0xFF005A9Eu
#define UI_COLOR_WINDOW_TITLE_INACTIVE_BG 0xFF3A3A3Au
#define UI_COLOR_ACCENT                  0xFF0078D4u  /* Windows blue */
#define UI_COLOR_TASKBAR_BG              0xFF1E1E1Eu
#define UI_COLOR_TASKBAR_HILITE          0xFF404040u
#define UI_COLOR_TEXT_FG                 0xFFFFFFFFu
#define UI_COLOR_TEXT_DIM                0xFFAAAAAAu

#endif /* COLORS_H */

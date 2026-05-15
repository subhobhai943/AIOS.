#ifndef AIOS_APPS_TERMINAL_GUI_H
#define AIOS_APPS_TERMINAL_GUI_H

/* kernel/apps/terminal_gui.h — Phase 11.3
 *
 * GUI Terminal — a windowed shell front-end that mirrors the
 * text-mode AIOS terminal + shell.
 *
 * Features (Phase 11.3 MVP):
 *   • Renders a fixed-size 80x25 character grid using the GUI font.
 *   • Takes keyboard input (printables, Enter, Backspace, arrows) and
 *     feeds it into the existing shell line editor via terminal_feed().
 *   • New lines printed by the shell via terminal_write*() are mirrored
 *     into this window (see terminal_gui_write_* adapters).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TGUI_COLS 80
#define TGUI_ROWS 25

/* Single GUI terminal instance for now. */
typedef struct terminal_gui {
    char     cells[TGUI_ROWS][TGUI_COLS];
    uint8_t  fg[TGUI_ROWS][TGUI_COLS];
    uint8_t  bg[TGUI_ROWS][TGUI_COLS];
    uint8_t  cur_col;
    uint8_t  cur_row;
    uint8_t  cur_fg;
    uint8_t  cur_bg;
    uint32_t win_id;
} terminal_gui_t;

/* Create a GUI terminal window and attach it to the shell. */
terminal_gui_t *terminal_gui_open(void);

/* Destroy GUI terminal (window is destroyed by WM caller). */
void terminal_gui_close(terminal_gui_t *tg);

/* Adapters called by terminal.c instead of vga_* when a GUI
 * terminal is present. Each function should mirror the behaviour
 * of its text-mode counterpart.
 */

void terminal_gui_write_char(char c);
void terminal_gui_write(const char *s);
void terminal_gui_write_len(const char *s, size_t len);

void terminal_gui_move_cursor(uint8_t col, uint8_t row);
void terminal_gui_set_color(uint8_t fg, uint8_t bg);
void terminal_gui_reset_color(void);
void terminal_gui_clear_line_to_end(void);
void terminal_gui_clear_line(void);
void terminal_gui_clear_screen(void);

uint8_t terminal_gui_cursor_col(void);
uint8_t terminal_gui_cursor_row(void);

#endif /* AIOS_APPS_TERMINAL_GUI_H */

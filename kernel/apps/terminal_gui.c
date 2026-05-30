/* kernel/apps/terminal_gui.c — Phase 11.3
 *
 * GUI Terminal — windowed view of the AIOS shell.
 *
 * Fix (Phase 11 bugfix):
 *   tgui_handle_event previously compared ev->keycode against raw PS/2
 *   scancodes (0x1C for Enter, 0x0E for Backspace).  input_bridge.c
 *   translates scancodes to ASCII *before* enqueuing, so ev->keycode
 *   carries the ASCII value.  Fixed comparisons to use ASCII characters.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "terminal_gui.h"
#include "../heap.h"
#include "../serial.h"
#include "../gfx/framebuffer.h"
#include "../gfx/font.h"
#include "../gui/window.h"
#include "../gui/input.h"

#define TGUI_FONT_W 8
#define TGUI_FONT_H 16
#define PADDING 4

#define TGUI_COL_BG   0xFF000000u
#define TGUI_COL_FG   0xFFFFFFFFu
#define TGUI_COL_CUR  0xFF00FF00u

static terminal_gui_t *g_tgui = 0;

static void tg_memset(void *p, uint8_t v, size_t n)
{
    uint8_t *b = (uint8_t*)p; while (n--) *b++ = v;
}

static void tgui_scroll_up(terminal_gui_t *tg)
{
    for (uint32_t r = 1; r < TGUI_ROWS; r++) {
        for (uint32_t c = 0; c < TGUI_COLS; c++) {
            tg->cells[r-1][c] = tg->cells[r][c];
            tg->fg[r-1][c]    = tg->fg[r][c];
            tg->bg[r-1][c]    = tg->bg[r][c];
        }
    }
    for (uint32_t c = 0; c < TGUI_COLS; c++) {
        tg->cells[TGUI_ROWS-1][c] = ' ';
        tg->fg[TGUI_ROWS-1][c]    = 0x0Fu;
        tg->bg[TGUI_ROWS-1][c]    = 0x00u;
    }
    if (tg->cur_row > 0) tg->cur_row--;
}

static void tgui_put_char_raw(terminal_gui_t *tg, char c)
{
    if (c == '\n') {
        tg->cur_col = 0;
        if (tg->cur_row + 1 >= TGUI_ROWS) tgui_scroll_up(tg);
        else tg->cur_row++;
        return;
    }
    if (c == '\r') { tg->cur_col = 0; return; }
    if (c == '\t') {
        uint8_t next = (uint8_t)((tg->cur_col + 4) & ~3u);
        if (next >= TGUI_COLS) {
            tg->cur_col = 0;
            if (tg->cur_row + 1 >= TGUI_ROWS) tgui_scroll_up(tg);
            else tg->cur_row++;
        } else {
            tg->cur_col = next;
        }
        return;
    }
    if (c == '\b') {
        if (tg->cur_col > 0) tg->cur_col--;
        tg->cells[tg->cur_row][tg->cur_col] = ' ';
        return;
    }

    if (tg->cur_col >= TGUI_COLS) {
        tg->cur_col = 0;
        if (tg->cur_row + 1 >= TGUI_ROWS) tgui_scroll_up(tg);
        else tg->cur_row++;
    }

    tg->cells[tg->cur_row][tg->cur_col] = c;
    tg->fg[tg->cur_row][tg->cur_col] = tg->cur_fg;
    tg->bg[tg->cur_row][tg->cur_col] = tg->cur_bg;
    tg->cur_col++;
}

/* --- Window drawing ------------------------------------------------- */

static void tgui_fill_rect(framebuffer_t *fb, int x, int y, int w, int h, uint32_t col)
{
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= (int)fb->height) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= (int)fb->width) continue;
            fb_put_pixel((uint32_t)xx, (uint32_t)yy, col);
        }
    }
}

static void tgui_draw_char(framebuffer_t *fb,
                           const gui_font_t *font,
                           int x, int y, char c,
                           uint32_t fg, uint32_t bg)
{
    font_draw_char(fb, font, (uint32_t)x, (uint32_t)y, c, fg, bg);
}

static void tgui_draw(gui_window_t *win, framebuffer_t *fb)
{
    terminal_gui_t *tg = (terminal_gui_t*)win->user_data;
    if (!tg || !fb) return;
    const gui_font_t *font = font_load_builtin();

    tgui_fill_rect(fb, win->x, win->y, (int)win->width, (int)win->height, TGUI_COL_BG);

    int x0 = win->x + PADDING;
    int y0 = win->y + PADDING;

    for (uint32_t r = 0; r < TGUI_ROWS; r++) {
        for (uint32_t c = 0; c < TGUI_COLS; c++) {
            char ch = tg->cells[r][c];
            tgui_draw_char(fb, font,
                           x0 + (int)c * TGUI_FONT_W,
                           y0 + (int)r * TGUI_FONT_H,
                           ch ? ch : ' ', TGUI_COL_FG, TGUI_COL_BG);
        }
    }

    int cx = x0 + (int)tg->cur_col * TGUI_FONT_W;
    int cy = y0 + (int)tg->cur_row * TGUI_FONT_H;
    tgui_fill_rect(fb, cx, cy + TGUI_FONT_H - 2, TGUI_FONT_W, 2, TGUI_COL_CUR);
}

/* --- Event handling ------------------------------------------------- */

static void tgui_handle_event(gui_window_t *win, const gui_event_t *ev)
{
    (void)win;
    if (!g_tgui || !ev) return;

    if (ev->type == GUI_EVENT_KEY_DOWN) {
        /*
         * FIX: ev->keycode carries the ASCII value translated by
         * input_bridge.c (ke->ascii when non-zero, else scancode).
         * Previous code compared against raw PS/2 scancodes (0x1C,
         * 0x0E) which are never in the ASCII printable range, so all
         * key events were silently dropped.
         *
         * Correct ASCII values:
         *   Enter     = '\n' = 0x0A
         *   Backspace = '\b' = 0x08
         *   Printable = 0x20 .. 0x7E
         */
        uint8_t key = ev->keycode;

        if (key == '\n' || key == '\r') {
            /* Enter: newline + re-print prompt */
            terminal_gui_write("\nAIOS> ");
        } else if (key == '\b') {
            /* Backspace: erase last character */
            terminal_gui_write_char('\b');
        } else if (key >= 0x20u && key <= 0x7Eu) {
            /* Printable ASCII */
            terminal_gui_write_char((char)key);
        }
        /* Ignore all other keycodes (function keys, arrows via scancode
         * fallback, modifiers, etc.). */
    }
}

/* --- Public API ----------------------------------------------------- */

terminal_gui_t *terminal_gui_open(void)
{
    if (g_tgui) return g_tgui;

    terminal_gui_t *tg = (terminal_gui_t*)kmalloc(sizeof(terminal_gui_t));
    if (!tg) return 0;
    tg_memset(tg, 0, sizeof(terminal_gui_t));

    tg->cur_fg = 0x0Fu;
    tg->cur_bg = 0x00u;

    for (uint32_t r = 0; r < TGUI_ROWS; r++) {
        for (uint32_t c = 0; c < TGUI_COLS; c++) {
            tg->cells[r][c] = ' ';
            tg->fg[r][c]    = tg->cur_fg;
            tg->bg[r][c]    = tg->cur_bg;
        }
    }

    uint32_t w = (uint32_t)(TGUI_COLS * TGUI_FONT_W + PADDING * 2);
    uint32_t h = (uint32_t)(TGUI_ROWS * TGUI_FONT_H + PADDING * 2);

    gui_window_t *win = gui_create_window(40, 40, w, h,
                                          "Terminal",
                                          tgui_draw,
                                          tgui_handle_event,
                                          tg);
    if (!win) {
        kfree(tg);
        return 0;
    }

    tg->win_id = win->id;
    g_tgui = tg;
    terminal_gui_write("AIOS GUI terminal\nAIOS> ");

    serial_puts(SERIAL_COM1, "[terminal_gui] opened\r\n");
    return tg;
}

void terminal_gui_close(terminal_gui_t *tg)
{
    if (!tg) return;
    if (g_tgui == tg) g_tgui = 0;
    kfree(tg);
}

/* --- Adapters used by terminal.c ----------------------------------- */

void terminal_gui_write_char(char c)
{
    if (!g_tgui) return;
    tgui_put_char_raw(g_tgui, c);
}

void terminal_gui_write(const char *s)
{
    if (!g_tgui || !s) return;
    while (*s) tgui_put_char_raw(g_tgui, *s++);
}

void terminal_gui_write_len(const char *s, size_t len)
{
    if (!g_tgui || !s) return;
    for (size_t i = 0; i < len; i++) tgui_put_char_raw(g_tgui, s[i]);
}

void terminal_gui_move_cursor(uint8_t col, uint8_t row)
{
    if (!g_tgui) return;
    if (col >= TGUI_COLS) col = (uint8_t)(TGUI_COLS - 1);
    if (row >= TGUI_ROWS) row = (uint8_t)(TGUI_ROWS - 1);
    g_tgui->cur_col = col;
    g_tgui->cur_row = row;
}

void terminal_gui_set_color(uint8_t fg, uint8_t bg)
{
    if (!g_tgui) return;
    g_tgui->cur_fg = fg;
    g_tgui->cur_bg = bg;
}

void terminal_gui_reset_color(void)
{
    if (!g_tgui) return;
    g_tgui->cur_fg = 0x0Fu;
    g_tgui->cur_bg = 0x00u;
}

void terminal_gui_clear_line_to_end(void)
{
    if (!g_tgui) return;
    for (uint32_t c = g_tgui->cur_col; c < TGUI_COLS; c++) {
        g_tgui->cells[g_tgui->cur_row][c] = ' ';
        g_tgui->fg[g_tgui->cur_row][c]    = g_tgui->cur_fg;
        g_tgui->bg[g_tgui->cur_row][c]    = g_tgui->cur_bg;
    }
}

void terminal_gui_clear_line(void)
{
    if (!g_tgui) return;
    for (uint32_t c = 0; c < TGUI_COLS; c++) {
        g_tgui->cells[g_tgui->cur_row][c] = ' ';
        g_tgui->fg[g_tgui->cur_row][c]    = g_tgui->cur_fg;
        g_tgui->bg[g_tgui->cur_row][c]    = g_tgui->cur_bg;
    }
    g_tgui->cur_col = 0;
}

void terminal_gui_clear_screen(void)
{
    if (!g_tgui) return;
    for (uint32_t r = 0; r < TGUI_ROWS; r++) {
        for (uint32_t c = 0; c < TGUI_COLS; c++) {
            g_tgui->cells[r][c] = ' ';
            g_tgui->fg[r][c]    = g_tgui->cur_fg;
            g_tgui->bg[r][c]    = g_tgui->cur_bg;
        }
    }
    g_tgui->cur_col = 0;
    g_tgui->cur_row = 0;
}

uint8_t terminal_gui_cursor_col(void) { return g_tgui ? g_tgui->cur_col : 0; }
uint8_t terminal_gui_cursor_row(void) { return g_tgui ? g_tgui->cur_row : 0; }

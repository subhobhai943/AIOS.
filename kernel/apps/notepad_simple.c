#include "notepad.h"

#include "../heap.h"
#include "../gfx/font.h"
#include "../gui/input.h"
#include "../gui/window.h"

#include <stdint.h>
#include <stddef.h>

#define NP_FONT_W 8
#define NP_FONT_H 16
#define NP_PAD 8
#define NP_STATUS_H 22
#define NP_TEXT_CAP 4096

static void np_memset(void *p, uint8_t v, size_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = v;
}

static void np_strcpy(char *dst, const char *src)
{
    while ((*dst++ = *src++)) {
    }
}

static void np_insert_char(notepad_t *np, char c)
{
    if (!np || !np->gbuf.buf) return;
    if (np->gbuf.gap_start + 1u >= np->gbuf.buf_size) return;
    np->gbuf.buf[np->gbuf.gap_start++] = c;
    np->gbuf.buf[np->gbuf.gap_start] = '\0';
    np->modified = true;
}

static void np_backspace(notepad_t *np)
{
    if (!np || np->gbuf.gap_start == 0) return;
    np->gbuf.gap_start--;
    np->gbuf.buf[np->gbuf.gap_start] = '\0';
    np->modified = true;
}

static void np_draw_string(framebuffer_t *fb,
                           const gui_font_t *font,
                           uint32_t x,
                           uint32_t y,
                           const char *s,
                           uint32_t fg,
                           uint32_t bg)
{
    while (s && *s) {
        font_draw_char(fb, font, x, y, *s, fg, bg);
        x += NP_FONT_W;
        s++;
    }
}

static void notepad_draw_cb(gui_window_t *win, framebuffer_t *fb)
{
    notepad_t *np = (notepad_t *)win->user_data;
    if (!np || !fb) return;

    const gui_font_t *font = font_load_builtin();
    uint32_t body_y = (uint32_t)win->y + 20u;
    uint32_t status_y = (uint32_t)win->y + win->height - NP_STATUS_H;

    fb_fill_rect((uint32_t)win->x + 1u, body_y,
                 win->width - 2u, win->height - 20u - NP_STATUS_H,
                 NP_COL_BG);
    fb_fill_rect((uint32_t)win->x + 1u, status_y,
                 win->width - 2u, NP_STATUS_H,
                 NP_COL_STATUS_BG);

    uint32_t x = (uint32_t)win->x + NP_PAD;
    uint32_t y = body_y + NP_PAD;
    uint32_t line_x = x;
    uint32_t max_y = status_y > NP_FONT_H ? status_y - NP_FONT_H : status_y;
    const char *buf = np->gbuf.buf ? np->gbuf.buf : "";

    for (size_t i = 0; i < np->gbuf.gap_start && y <= max_y; i++) {
        char c = buf[i];
        if (c == '\n') {
            x = line_x;
            y += NP_FONT_H;
            continue;
        }
        if (c == '\t') c = ' ';
        if (c < 0x20 || c >= 0x7F) c = '.';
        font_draw_char(fb, font, x, y, c, NP_COL_TEXT, NP_COL_BG);
        x += NP_FONT_W;
        if (x + NP_FONT_W >= (uint32_t)win->x + win->width - NP_PAD) {
            x = line_x;
            y += NP_FONT_H;
        }
    }

    if (y <= max_y) {
        fb_fill_rect(x, y, 2u, NP_FONT_H, NP_COL_CURSOR);
    }

    char status[96];
    np_memset(status, 0, sizeof(status));
    np_strcpy(status, np->modified ? "* " : "  ");
    np_strcpy(status + 2, np->filename[0] ? np->filename : "Untitled");
    np_draw_string(fb, font, (uint32_t)win->x + NP_PAD, status_y + 3u,
                   status, NP_COL_STATUS_FG, NP_COL_STATUS_BG);
}

static void notepad_event_cb(gui_window_t *win, const gui_event_t *ev)
{
    notepad_t *np = (notepad_t *)win->user_data;
    if (!np || !ev || ev->type != GUI_EVENT_KEY_DOWN) return;

    uint8_t key = ev->keycode;
    if (key == '\b' || key == 0x0Eu) {
        np_backspace(np);
        return;
    }
    if (key == '\r' || key == '\n' || key == 0x1Cu) {
        np_insert_char(np, '\n');
        return;
    }
    if (key >= 0x20u && key < 0x7Fu) {
        np_insert_char(np, (char)key);
    }
}

notepad_t *notepad_open(const char *path)
{
    notepad_t *np = (notepad_t *)kmalloc(sizeof(notepad_t));
    if (!np) return 0;
    np_memset(np, 0, sizeof(notepad_t));

    np->gbuf.buf = (char *)kmalloc(NP_TEXT_CAP);
    if (!np->gbuf.buf) {
        kfree(np);
        return 0;
    }
    np_memset(np->gbuf.buf, 0, NP_TEXT_CAP);
    np->gbuf.buf_size = NP_TEXT_CAP;
    np->gbuf.gap_end = NP_TEXT_CAP;
    np->cursor_visible = true;
    np_strcpy(np->filename, path && path[0] ? path : "Untitled");

    gui_window_t *win = gui_create_window(120, 80, 520, 340,
                                          "Notepad",
                                          notepad_draw_cb,
                                          notepad_event_cb,
                                          np);
    if (!win) {
        kfree(np->gbuf.buf);
        kfree(np);
        return 0;
    }

    np->win_id = win->id;
    return np;
}

void notepad_close(notepad_t *np)
{
    if (!np) return;
    if (np->gbuf.buf) kfree(np->gbuf.buf);
    kfree(np);
}

void notepad_on_key(notepad_t *np, char ascii, uint8_t scancode, uint8_t mods)
{
    (void)mods;
    if (!np) return;
    if (ascii == '\b' || scancode == 0x0Eu) np_backspace(np);
    else if (ascii == '\n' || ascii == '\r' || scancode == 0x1Cu) np_insert_char(np, '\n');
    else if (ascii >= 0x20 && ascii < 0x7F) np_insert_char(np, ascii);
}

void notepad_draw(notepad_t *np)
{
    (void)np;
}

void notepad_tick(notepad_t *np)
{
    (void)np;
}

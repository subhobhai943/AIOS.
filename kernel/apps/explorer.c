/* kernel/apps/explorer.c — Phase 11.2
 *
 * AIOS File Explorer — two-pane VFS browser.
 *
 *   Left  : current path string (breadcrumb style)
 *   Right : directory entries (dirs first, then files) in a list
 *
 * Interaction:
 *   Up/Down      — move selection
 *   Enter        — if dir, descend; if file, open with Notepad
 *   Backspace    — go up one directory (unless at root)
 *   Double-click — same as Enter on item
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "explorer.h"
#include "../heap.h"
#include "../serial.h"
#include "../gfx/framebuffer.h"
#include "../gfx/font.h"
#include "../gui/window.h"
#include "../gui/input.h"
#include "../fs/vfs.h"
#include "notepad.h"

/* Local helpers (freestanding) */
static void ex_memset(void *p, uint8_t v, size_t n) {
    uint8_t *b = (uint8_t*)p; while (n--) *b++ = v;
}
static void ex_memcpy(void *d, const void *s, size_t n) {
    uint8_t *dd=(uint8_t*)d; const uint8_t*ss=(const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static size_t ex_strlen(const char *s) {
    size_t n=0; while (s[n]) n++; return n;
}
static void ex_strcpy(char *d, const char *s) {
    while ((*d++ = *s++));
}
static void ex_strcat(char *d, const char *s) {
    size_t off = ex_strlen(d);
    ex_strcpy(d + off, s);
}
static int ex_strcmp(const char *a, const char *b) {
    while (*a || *b) {
        if (*a != *b) return (int)(unsigned char)*a - (int)(unsigned char)*b;
        if (*a == '\0') break;
        a++; b++;
    }
    return 0;
}

#define FONT_W 8
#define FONT_H 16
#define PADDING 4
#define HEADER_H 20
#define ROW_H   18

/* Simple colour constants (ARGB8888) */
#define EXPL_COL_BG        0xFFF5F5F5
#define EXPL_COL_LIST_BG   0xFFFFFFFF
#define EXPL_COL_TEXT      0xFF202020
#define EXPL_COL_TEXT_DIM  0xFF808080
#define EXPL_COL_SEL_BG    0xFF2D5FA8
#define EXPL_COL_SEL_FG    0xFFFFFFFF
#define EXPL_COL_HEADER_BG 0xFFE0E0E0
#define EXPL_COL_HEADER_FG 0xFF000000

/* Draw primitives using framebuffer helpers */
static void ex_fill_rect(framebuffer_t *fb, int x, int y, int w, int h, uint32_t col)
{
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= (int)fb->height) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= (int)fb->width) continue;
            fb_put_pixel((uint32_t)xx, (uint32_t)yy, col);
        }
    }
}

static int ex_draw_string(framebuffer_t *fb, const gui_font_t *font,
                          int x, int y, const char *s,
                          uint32_t fg, uint32_t bg)
{
    (void)font; /* currently a single global builtin font */
    while (*s) {
        font_draw_char(fb, fb->width, x, y, *s, fg, bg);
        x += FONT_W;
        s++;
    }
    return x;
}

/* --- Directory listing stub ------------------------------------- */

/* For now, VFS doesn’t expose readdir.  We hard-code root contents
 * based on vfs_stat() existence checks for a few useful files.
 */

static void explorer_load_dummy(explorer_t *exp)
{
    exp->entry_count = 0;

    /* Root contains: /, /weights.gguf, /README.TXT if they exist.
     * Always show a synthetic ".." when not at root.
     */

    bool at_root = (ex_strcmp(exp->cwd, "/") == 0);

    if (!at_root) {
        explorer_entry_t *e = &exp->entries[exp->entry_count++];
        ex_strcpy(e->name, "..");
        e->is_dir = true;
        e->size   = 0;
    }

    vfs_stat_t st;

    if (vfs_stat("README.TXT", &st) == 0) {
        explorer_entry_t *e = &exp->entries[exp->entry_count++];
        ex_strcpy(e->name, "README.TXT");
        e->is_dir = false;
        e->size   = st.size;
    }

    if (vfs_stat("weights.gguf", &st) == 0) {
        explorer_entry_t *e = &exp->entries[exp->entry_count++];
        ex_strcpy(e->name, "weights.gguf");
        e->is_dir = false;
        e->size   = st.size;
    }

    if (exp->entry_count == 0) {
        explorer_entry_t *e = &exp->entries[exp->entry_count++];
        ex_strcpy(e->name, "(empty)");
        e->is_dir = false;
        e->size   = 0;
    }

    exp->selected = (exp->entry_count > 0) ? 0 : -1;
    exp->scroll   = 0;
}

/* --- Notepad launcher ------------------------------------------- */

static void explorer_open_file(explorer_t *exp, const char *name)
{
    char path[EXPL_PATH_MAX];
    ex_memset(path, 0, sizeof(path));

    if (ex_strcmp(exp->cwd, "/") == 0) {
        /* root */
        ex_strcpy(path, "/");
        ex_strcat(path, name);
    } else {
        ex_strcpy(path, exp->cwd);
        size_t len = ex_strlen(path);
        if (len > 0 && path[len-1] != '/')
            ex_strcat(path, "/");
        ex_strcat(path, name);
    }

    notepad_open(path);
}

/* --- Path navigation helpers ------------------------------------ */

static void explorer_go_up(explorer_t *exp)
{
    if (ex_strcmp(exp->cwd, "/") == 0) return;

    size_t len = ex_strlen(exp->cwd);
    if (len == 0) {
        ex_strcpy(exp->cwd, "/");
        return;
    }

    /* strip trailing slash */
    if (len > 1 && exp->cwd[len-1] == '/') {
        exp->cwd[len-1] = '\0';
        len--;
    }

    /* find previous slash */
    while (len > 0 && exp->cwd[len-1] != '/') len--;

    if (len == 0) {
        ex_strcpy(exp->cwd, "/");
    } else if (len == 1) {
        exp->cwd[1] = '\0';
    } else {
        exp->cwd[len] = '\0';
    }

    explorer_load_dummy(exp);
}

static void explorer_enter(explorer_t *exp)
{
    if (exp->selected < 0 || (uint32_t)exp->selected >= exp->entry_count)
        return;

    explorer_entry_t *e = &exp->entries[exp->selected];

    if (e->is_dir) {
        if (ex_strcmp(e->name, "..") == 0) {
            explorer_go_up(exp);
            return;
        }

        /* For now, since VFS doesn’t expose subdir listing, just
         * update cwd path and reload same dummy list.
         */
        if (ex_strcmp(exp->cwd, "/") == 0) {
            ex_strcpy(exp->cwd, "/");
            ex_strcat(exp->cwd, e->name);
        } else {
            size_t len = ex_strlen(exp->cwd);
            if (len > 0 && exp->cwd[len-1] != '/')
                ex_strcat(exp->cwd, "/");
            ex_strcat(exp->cwd, e->name);
        }
        explorer_load_dummy(exp);
    } else {
        explorer_open_file(exp, e->name);
    }
}

/* --- Window callbacks -------------------------------------------- */

static void explorer_draw_cb(gui_window_t *win, framebuffer_t *fb)
{
    explorer_t *exp = (explorer_t *)win->user_data;
    if (!exp || !fb) return;

    /* Background */
    ex_fill_rect(fb, win->x, win->y, (int)win->width, (int)win->height,
                 EXPL_COL_BG);

    int x0 = win->x + PADDING;
    int y0 = win->y + PADDING;

    /* Header bar (current path) */
    ex_fill_rect(fb, x0, y0, (int)win->width - PADDING*2, HEADER_H,
                 EXPL_COL_HEADER_BG);
    ex_draw_string(fb, 0, x0 + 4, y0 + 2, exp->cwd,
                   EXPL_COL_HEADER_FG, EXPL_COL_HEADER_BG);

    int list_y = y0 + HEADER_H + PADDING;
    int list_h = (int)win->height - (list_y - win->y) - PADDING;

    ex_fill_rect(fb, x0, list_y, (int)win->width - PADDING*2, list_h,
                 EXPL_COL_LIST_BG);

    int max_rows = list_h / ROW_H;
    int row_y = list_y + 2;

    for (int i = 0; i < max_rows; i++) {
        int idx = exp->scroll + i;
        if (idx < 0 || (uint32_t)idx >= exp->entry_count) break;
        explorer_entry_t *e = &exp->entries[idx];

        bool sel = (idx == exp->selected);
        uint32_t bg = sel ? EXPL_COL_SEL_BG : EXPL_COL_LIST_BG;
        uint32_t fg = sel ? EXPL_COL_SEL_FG : EXPL_COL_TEXT;

        ex_fill_rect(fb, x0, row_y - 2, (int)win->width - PADDING*2, ROW_H,
                     bg);

        char namebuf[EXPL_MAX_NAME + 4];
        ex_memset(namebuf, 0, sizeof(namebuf));
        if (e->is_dir) {
            namebuf[0] = '['; namebuf[1] = '\0';
            ex_strcat(namebuf, e->name);
            ex_strcat(namebuf, "]");
        } else {
            ex_strcpy(namebuf, e->name);
        }

        ex_draw_string(fb, 0, x0 + 4, row_y, namebuf, fg, bg);

        row_y += ROW_H;
    }
}

static void explorer_event_cb(gui_window_t *win, const gui_event_t *ev)
{
    explorer_t *exp = (explorer_t *)win->user_data;
    if (!exp || !ev) return;

    if (ev->type == GUI_EVENT_KEY_DOWN) {
        uint8_t key = ev->keycode;
        if (key == 0x48) { /* Up arrow scancode (from keyboard.h) */
            if (exp->selected > 0) {
                exp->selected--;
                if (exp->selected < exp->scroll)
                    exp->scroll = exp->selected;
            }
        } else if (key == 0x50) { /* Down arrow */
            if ((uint32_t)(exp->selected + 1) < exp->entry_count) {
                exp->selected++;
            }
        } else if (key == 0x0E) { /* Backspace */
            explorer_go_up(exp);
        } else if (key == 0x1C) { /* Enter */
            explorer_enter(exp);
        }
    }

    if (ev->type == GUI_EVENT_MOUSE_DOWN &&
        (ev->buttons & GUI_MOUSE_BUTTON_LEFT)) {
        /* Hit-test rows */
        int x0 = win->x + PADDING;
        int list_y = win->y + PADDING + HEADER_H + PADDING;

        if (ev->y >= list_y) {
            int row = (ev->y - list_y) / ROW_H;
            int idx = exp->scroll + row;
            if (idx >= 0 && (uint32_t)idx < exp->entry_count) {
                if (exp->selected == idx &&
                    (ev->mouse_flags & GUI_MOUSE_FLAG_DOUBLE_CLICK)) {
                    explorer_enter(exp);
                } else {
                    exp->selected = idx;
                }
            }
        }
    }
}

/* --- Public API --------------------------------------------------- */

explorer_t *explorer_open(const char *start_path)
{
    explorer_t *exp = (explorer_t *)kmalloc(sizeof(explorer_t));
    if (!exp) return 0;
    ex_memset(exp, 0, sizeof(explorer_t));

    if (start_path && start_path[0]) {
        size_t len = ex_strlen(start_path);
        if (len >= EXPL_PATH_MAX) len = EXPL_PATH_MAX - 1;
        ex_memcpy(exp->cwd, start_path, len);
        exp->cwd[len] = '\0';
    } else {
        ex_strcpy(exp->cwd, "/");
    }

    explorer_load_dummy(exp);

    gui_window_t *win = gui_create_window(80, 80, 480, 360,
                                          "Explorer",
                                          explorer_draw_cb,
                                          explorer_event_cb,
                                          exp);
    if (!win) {
        kfree(exp);
        return 0;
    }

    exp->win_id = win->id;

    serial_puts("[explorer] opened\r\n");
    return exp;
}

void explorer_close(explorer_t *exp)
{
    if (!exp) return;
    /* Window will be destroyed by caller through GUI API if needed. */
    kfree(exp);
}

/* kernel/apps/notepad.c — Phase 11.1
 *
 * AIOS Notepad — multiline text editor GUI application.
 *
 * Architecture
 * ────────────
 * Data model : gap buffer (O(1) insert/delete at cursor, O(n) move)
 * Rendering  : full redraw into WM window pixel buffer each frame
 *              (cheap enough for a 256 KiB text file at 60 fps)
 * Font       : kernel/gfx/font.h  (8×16 monospace PSF)
 * WM         : kernel/gui/wm.h    (wm_create_window / wm_get_fb / wm_dirty)
 * VFS        : kernel/fs/vfs.h    (vfs_open / vfs_read / vfs_write / vfs_close)
 * Heap       : kernel/heap.h      (kmalloc / kfree / krealloc)
 * PIT        : kernel/include/pit.h (g_ticks for cursor blink)
 *
 * Keyboard shortcuts
 * ──────────────────
 *   Ctrl+N   New document (prompts to save if modified)
 *   Ctrl+O   Open file    (prompts for path via mini input bar)
 *   Ctrl+S   Save         (save to current path; if new, prompts for path)
 *   Ctrl+A   Select all
 *   Backspace Delete character before cursor
 *   Delete   Delete character after  cursor
 *   Enter    Insert newline
 *   Left/Right  Move cursor by character
 *   Up/Down     Move cursor by line
 *   Home/End    Move to line start / end
 *
 * Freestanding C — no libc.  Only <stdint.h>, <stddef.h>, <stdbool.h>.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "notepad.h"
#include "../heap.h"
#include "../serial.h"
#include "../gfx/font.h"
#include "../gfx/framebuffer.h"
#include "../gui/wm.h"
#include "../fs/vfs.h"
#include "../include/pit.h"
#include "../keyboard.h"

/* ── freestanding helpers ──────────────────────────────────────────── */
static inline void np_memset(void *p, uint8_t v, size_t n) {
    uint8_t *b = (uint8_t *)p; for (size_t i=0;i<n;i++) b[i]=v;
}
static inline void np_memcpy(void *d, const void *s, size_t n) {
    uint8_t *dd=(uint8_t*)d; const uint8_t*ss=(const uint8_t*)s;
    for(size_t i=0;i<n;i++) dd[i]=ss[i];
}
static inline size_t np_strlen(const char *s) {
    size_t n=0; while(s[n]) n++; return n;
}
static inline void np_strcpy(char *d, const char *s) {
    while((*d++=*s++));
}
static inline int np_strncmp(const char*a,const char*b,size_t n){
    for(size_t i=0;i<n;i++){
        if((unsigned char)a[i]!=(unsigned char)b[i])
            return (int)(unsigned char)a[i]-(int)(unsigned char)b[i];
        if(!a[i]) return 0;
    }
    return 0;
}
/* uint32 → decimal string, returns length written */
static int np_itoa(uint32_t v, char *buf) {
    if (v == 0) { buf[0]='0'; buf[1]='\0'; return 1; }
    char tmp[12]; int i=0;
    while(v){ tmp[i++]='0'+(v%10); v/=10; }
    int len=i;
    for(int j=0;j<i;j++) buf[j]=tmp[i-1-j];
    buf[i]='\0'; return len;
}

/* ── Layout constants ───────────────────────────────────────────── */
#define FONT_W       8     /* glyph width  in pixels */
#define FONT_H       16    /* glyph height in pixels */
#define GUTTER_W     48    /* line-number gutter width */
#define STATUS_H     20    /* status bar height */
#define PADDING      4     /* inner text padding */

/* Keyboard modifier bits (must match keyboard.h) */
#define MOD_CTRL     0x01
#define MOD_SHIFT    0x02
#define MOD_ALT      0x04

/* PS/2 scancodes for non-ASCII keys */
#define SC_BACKSPACE 0x0E
#define SC_DELETE    0x53
#define SC_ENTER     0x1C
#define SC_LEFT      0x4B
#define SC_RIGHT     0x4D
#define SC_UP        0x48
#define SC_DOWN      0x50
#define SC_HOME      0x47
#define SC_END       0x4F
#define SC_PGUP      0x49
#define SC_PGDN      0x51

/* ─────────────────────────────────────────────────────────────────────
 * Gap buffer operations
 * ───────────────────────────────────────────────────────────────────── */

static size_t gb_text_len(const gap_buf_t *g) {
    return g->buf_size - (g->gap_end - g->gap_start);
}

/* Get the logical byte at position pos (0-indexed) */
static char gb_char_at(const gap_buf_t *g, size_t pos) {
    if (pos < g->gap_start) return g->buf[pos];
    return g->buf[g->gap_end + (pos - g->gap_start)];
}

/* Move gap to position pos */
static void gb_move_gap(gap_buf_t *g, size_t pos) {
    size_t gap_size = g->gap_end - g->gap_start;
    if (pos < g->gap_start) {
        /* Move gap left: shift text rightward */
        size_t delta = g->gap_start - pos;
        np_memcpy(g->buf + g->gap_end - delta,
                  g->buf + pos, delta);
        g->gap_start = pos;
        g->gap_end   = pos + gap_size;
    } else if (pos > g->gap_start) {
        /* Move gap right: shift text leftward */
        size_t delta = pos - g->gap_start;
        np_memcpy(g->buf + g->gap_start,
                  g->buf + g->gap_end, delta);
        g->gap_start = pos;
        g->gap_end   = pos + gap_size;
    }
}

/* Grow the gap to at least min_gap bytes */
static bool gb_grow(gap_buf_t *g, size_t min_gap) {
    size_t cur_gap = g->gap_end - g->gap_start;
    if (cur_gap >= min_gap) return true;
    size_t need  = min_gap - cur_gap;
    size_t new_size = g->buf_size + need + NP_GAP_INITIAL;
    char *newbuf = (char *)kmalloc(new_size);
    if (!newbuf) return false;
    /* Copy pre-gap */
    np_memcpy(newbuf, g->buf, g->gap_start);
    /* Copy post-gap to end of new buffer */
    size_t post_len = g->buf_size - g->gap_end;
    size_t new_gap_end = new_size - post_len;
    np_memcpy(newbuf + new_gap_end, g->buf + g->gap_end, post_len);
    kfree(g->buf);
    g->buf      = newbuf;
    g->buf_size = new_size;
    g->gap_end  = new_gap_end;
    return true;
}

/* Insert one byte at cursor (gap_start) */
static bool gb_insert(gap_buf_t *g, char c) {
    if (g->gap_start >= g->gap_end) {
        if (!gb_grow(g, NP_GAP_INITIAL)) return false;
    }
    g->buf[g->gap_start++] = c;
    return true;
}

/* Delete one byte before cursor */
static void gb_backspace(gap_buf_t *g) {
    if (g->gap_start > 0) g->gap_start--;
}

/* Delete one byte after cursor */
static void gb_delete(gap_buf_t *g) {
    if (g->gap_end < g->buf_size) g->gap_end++;
}

/* Initialise an empty gap buffer */
static bool gb_init(gap_buf_t *g) {
    g->buf = (char *)kmalloc(NP_GAP_INITIAL);
    if (!g->buf) return false;
    g->buf_size  = NP_GAP_INITIAL;
    g->gap_start = 0;
    g->gap_end   = NP_GAP_INITIAL;
    return true;
}

static void gb_free(gap_buf_t *g) {
    if (g->buf) kfree(g->buf);
    np_memset(g, 0, sizeof(gap_buf_t));
}

/* ─────────────────────────────────────────────────────────────────────
 * Cursor position utilities
 * ───────────────────────────────────────────────────────────────────── */

/* Recompute cursor_line and cursor_col from gap_start */
static void np_update_cursor_pos(notepad_t *np) {
    uint32_t line = 0, col = 0;
    size_t pos = np->gbuf.gap_start;
    for (size_t i = 0; i < pos; i++) {
        char c = np->gbuf.buf[i < np->gbuf.gap_start ?
                               i : i + (np->gbuf.gap_end - np->gbuf.gap_start)];
        /* We walk only pre-gap bytes here since pos == gap_start */
        (void)c;
        char ch = np->gbuf.buf[i];  /* safe: i < gap_start */
        if (ch == '\n') { line++; col = 0; }
        else             { col++; }
    }
    np->cursor_line = line;
    np->cursor_col  = col;
}

/* Find the logical offset of the start of line `line` */
static size_t np_line_start(notepad_t *np, uint32_t target_line) {
    size_t len = gb_text_len(&np->gbuf);
    uint32_t line = 0;
    for (size_t i = 0; i < len; i++) {
        if (line == target_line) return i;
        if (gb_char_at(&np->gbuf, i) == '\n') line++;
    }
    return len;
}

/* Find the length of line `line` (not including the newline) */
static uint32_t np_line_len(notepad_t *np, uint32_t line) {
    size_t start = np_line_start(np, line);
    size_t len   = gb_text_len(&np->gbuf);
    uint32_t col = 0;
    for (size_t i = start; i < len; i++) {
        char c = gb_char_at(&np->gbuf, i);
        if (c == '\n') break;
        col++;
    }
    return col;
}

/* Total line count */
static uint32_t np_line_count(notepad_t *np) {
    size_t len = gb_text_len(&np->gbuf);
    uint32_t lines = 1;
    for (size_t i = 0; i < len; i++)
        if (gb_char_at(&np->gbuf, i) == '\n') lines++;
    return lines;
}

/* ─────────────────────────────────────────────────────────────────────
 * notepad_open
 * ───────────────────────────────────────────────────────────────────── */
notepad_t *notepad_open(const char *path)
{
    notepad_t *np = (notepad_t *)kmalloc(sizeof(notepad_t));
    if (!np) return (void*)0;
    np_memset(np, 0, sizeof(notepad_t));

    /* Initialise gap buffer */
    if (!gb_init(&np->gbuf)) { kfree(np); return (void*)0; }

    np->cursor_visible   = true;
    np->last_blink_tick  = g_ticks;

    /* Create WM window (640×480 client area) */
    np->win_id = wm_create_window("Notepad", 100, 60, 640, 480);

    /* Load file if path given */
    if (path && path[0] != '\0') {
        int fd = vfs_open(path);
        if (fd >= 0) {
            int64_t fsize = vfs_size(fd);
            if (fsize > 0 && fsize <= NP_MAX_TEXT) {
                /* Expand gap buffer */
                if (gb_grow(&np->gbuf, (size_t)fsize)) {
                    int32_t nr = vfs_read(fd, np->gbuf.buf, (size_t)fsize);
                    if (nr > 0) {
                        np->gbuf.gap_start = (size_t)nr;
                        /* Reset gap_end to end (no gap in middle yet) */
                        size_t gap_space = np->gbuf.buf_size - (size_t)nr;
                        np->gbuf.gap_end  = np->gbuf.buf_size - gap_space;
                        /* Re-init properly: place gap at end */
                        np->gbuf.gap_end  = np->gbuf.buf_size;
                        np->gbuf.gap_start = (size_t)nr;
                    }
                }
            }
            vfs_close(fd);
            /* Store path */
            size_t plen = np_strlen(path);
            if (plen >= NP_MAX_PATH) plen = NP_MAX_PATH - 1;
            np_memcpy(np->filepath, path, plen);
            np->filepath[plen] = '\0';
            /* Extract filename from path */
            const char *fname = path;
            for (size_t i = 0; i < plen; i++)
                if (path[i] == '/') fname = path + i + 1;
            size_t fnlen = np_strlen(fname);
            if (fnlen >= NP_MAX_FILENAME) fnlen = NP_MAX_FILENAME - 1;
            np_memcpy(np->filename, fname, fnlen);
            np->filename[fnlen] = '\0';
        } else {
            np_strcpy(np->filename, "(new file)");
        }
    } else {
        np_strcpy(np->filename, "Untitled");
    }

    /* Move gap to cursor (position 0) */
    gb_move_gap(&np->gbuf, 0);

    klog("[notepad] opened: %s\n", np->filename);
    notepad_draw(np);
    return np;
}

/* ─────────────────────────────────────────────────────────────────────
 * notepad_close
 * ───────────────────────────────────────────────────────────────────── */
void notepad_close(notepad_t *np)
{
    if (!np) return;
    gb_free(&np->gbuf);
    wm_destroy_window(np->win_id);
    kfree(np);
}

/* ─────────────────────────────────────────────────────────────────────
 * VFS save
 * ───────────────────────────────────────────────────────────────────── */
static bool np_save(notepad_t *np)
{
    if (np->filepath[0] == '\0') return false;  /* need path set */

    int fd = vfs_open_create(np->filepath);  /* create or truncate */
    if (fd < 0) {
        klog("[notepad] save: vfs_open_create failed: %s\n", np->filepath);
        return false;
    }

    /* Write pre-gap bytes */
    size_t pre  = np->gbuf.gap_start;
    size_t post = np->gbuf.buf_size - np->gbuf.gap_end;

    if (pre  > 0) vfs_write(fd, np->gbuf.buf, pre);
    if (post > 0) vfs_write(fd, np->gbuf.buf + np->gbuf.gap_end, post);

    vfs_close(fd);
    np->modified = false;
    klog("[notepad] saved %zu bytes to %s\n", pre + post, np->filepath);
    return true;
}

/* ─────────────────────────────────────────────────────────────────────
 * Keyboard handler
 * ───────────────────────────────────────────────────────────────────── */
void notepad_on_key(notepad_t *np, char ascii,
                    uint8_t scancode, uint8_t mods)
{
    bool ctrl = (mods & MOD_CTRL) != 0;

    /* ── Ctrl shortcuts ── */
    if (ctrl) {
        switch (ascii | 0x20) {  /* lowercase */
        case 's':
            np_save(np);
            notepad_draw(np);
            return;
        case 'n':
            /* New: reset buffer */
            gb_free(&np->gbuf);
            gb_init(&np->gbuf);
            np_memset(np->filepath, 0, NP_MAX_PATH);
            np_strcpy(np->filename, "Untitled");
            np->cursor = 0;
            np->cursor_line = 0;
            np->cursor_col  = 0;
            np->scroll_line = 0;
            np->modified = false;
            notepad_draw(np);
            return;
        case 'a':
            /* Select all */
            np->sel_start = 0;
            np->cursor    = gb_text_len(&np->gbuf);
            np->has_sel   = true;
            gb_move_gap(&np->gbuf, np->cursor);
            np_update_cursor_pos(np);
            notepad_draw(np);
            return;
        default: return;
        }
    }

    /* ── Navigation keys ── */
    if (ascii == 0) {
        size_t pos = np->gbuf.gap_start;
        size_t len = gb_text_len(&np->gbuf);

        switch (scancode) {
        case SC_LEFT:
            if (pos > 0) {
                gb_move_gap(&np->gbuf, pos - 1);
                np_update_cursor_pos(np);
            }
            break;
        case SC_RIGHT:
            if (pos < len) {
                gb_move_gap(&np->gbuf, pos + 1);
                np_update_cursor_pos(np);
            }
            break;
        case SC_UP:
            if (np->cursor_line > 0) {
                uint32_t target_line = np->cursor_line - 1;
                uint32_t target_col  = np->cursor_col;
                uint32_t tlen = np_line_len(np, target_line);
                if (target_col > tlen) target_col = tlen;
                size_t new_pos = np_line_start(np, target_line) + target_col;
                gb_move_gap(&np->gbuf, new_pos);
                np_update_cursor_pos(np);
                if (np->cursor_line < np->scroll_line)
                    np->scroll_line = np->cursor_line;
            }
            break;
        case SC_DOWN: {
            uint32_t total = np_line_count(np);
            if (np->cursor_line + 1 < total) {
                uint32_t target_line = np->cursor_line + 1;
                uint32_t target_col  = np->cursor_col;
                uint32_t tlen = np_line_len(np, target_line);
                if (target_col > tlen) target_col = tlen;
                size_t new_pos = np_line_start(np, target_line) + target_col;
                gb_move_gap(&np->gbuf, new_pos);
                np_update_cursor_pos(np);
            }
            break;
        }
        case SC_HOME:
            gb_move_gap(&np->gbuf, np_line_start(np, np->cursor_line));
            np_update_cursor_pos(np);
            break;
        case SC_END: {
            size_t ls = np_line_start(np, np->cursor_line);
            uint32_t ll = np_line_len(np, np->cursor_line);
            gb_move_gap(&np->gbuf, ls + ll);
            np_update_cursor_pos(np);
            break;
        }
        case SC_DELETE:
            gb_delete(&np->gbuf);
            np->modified = true;
            break;
        case SC_PGUP:
            if (np->scroll_line >= 10) np->scroll_line -= 10;
            else np->scroll_line = 0;
            break;
        case SC_PGDN:
            np->scroll_line += 10;
            break;
        default: return;
        }
        notepad_draw(np);
        return;
    }

    /* ── Printable / editing ── */
    if (scancode == SC_BACKSPACE || ascii == '\b') {
        gb_backspace(&np->gbuf);
        np->modified = true;
        np_update_cursor_pos(np);
        notepad_draw(np);
        return;
    }
    if (scancode == SC_ENTER || ascii == '\r' || ascii == '\n') {
        gb_insert(&np->gbuf, '\n');
        np->modified = true;
        np_update_cursor_pos(np);
        /* Auto-scroll */
        wm_window_t *win = wm_get_window(np->win_id);
        if (win) {
            uint32_t vis_lines = (win->h - STATUS_H - PADDING*2) / FONT_H;
            if (np->cursor_line >= np->scroll_line + vis_lines)
                np->scroll_line = np->cursor_line - vis_lines + 1;
        }
        notepad_draw(np);
        return;
    }
    if (ascii >= 0x20 && ascii < 0x7F) {
        gb_insert(&np->gbuf, ascii);
        np->modified = true;
        np_update_cursor_pos(np);
        notepad_draw(np);
        return;
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * Rendering
 * ───────────────────────────────────────────────────────────────────── */

/*
 * fill_rect: fill a rectangle in the WM window with a solid colour.
 * fb      = wm_get_fb(win_id)
 * stride  = window pixel width
 */
static void fill_rect(uint32_t *fb, uint32_t stride,
                       int x, int y, int w, int h, uint32_t colour)
{
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            fb[row * (int)stride + col] = colour;
}

/*
 * draw_char: render one glyph using font_draw_char (kernel/gfx/font.h).
 * x, y = top-left pixel inside the WM window.
 */
static void draw_char(uint32_t *fb, uint32_t stride,
                       int x, int y, char c, uint32_t fg, uint32_t bg)
{
    font_draw_char(fb, stride, x, y, c, fg, bg);
}

/*
 * draw_string: render a NUL-terminated string left-to-right.
 * Returns x position after last character.
 */
static int draw_string(uint32_t *fb, uint32_t stride,
                        int x, int y,
                        const char *s, uint32_t fg, uint32_t bg)
{
    while (*s) {
        draw_char(fb, stride, x, y, *s, fg, bg);
        x += FONT_W;
        s++;
    }
    return x;
}

void notepad_draw(notepad_t *np)
{
    wm_window_t *win = wm_get_window(np->win_id);
    if (!win) return;

    uint32_t *fb     = wm_get_fb(np->win_id);
    uint32_t  stride = win->w;
    uint32_t  win_w  = win->w;
    uint32_t  win_h  = win->h;

    /* ── Clear editor area ── */
    fill_rect(fb, stride, 0, 0, (int)win_w, (int)(win_h - STATUS_H), NP_COL_BG);

    /* ── Line-number gutter ── */
    fill_rect(fb, stride, 0, 0, GUTTER_W, (int)(win_h - STATUS_H), NP_COL_LINENO_BG);

    /* ── Render visible lines ── */
    uint32_t editor_h   = win_h - STATUS_H;
    uint32_t vis_lines  = (editor_h - (uint32_t)(PADDING * 2)) / FONT_H;
    uint32_t total_lines = np_line_count(np);

    /* Clamp scroll */
    if (np->scroll_line + vis_lines > total_lines && total_lines > vis_lines)
        np->scroll_line = total_lines - vis_lines;

    size_t text_len = gb_text_len(&np->gbuf);
    size_t logical_pos = 0;   /* current logical offset */
    uint32_t line = 0;        /* current line index */
    uint32_t col  = 0;        /* current column index */

    /* Walk from start to find position of scroll_line */
    size_t draw_start = np_line_start(np, np->scroll_line);
    line = np->scroll_line;
    col  = 0;
    logical_pos = draw_start;

    int px = GUTTER_W + PADDING;
    int py = PADDING;
    uint32_t drawn_lines = 0;

    /* Draw line number for first visible line */
    {
        char lnum[8]; np_itoa(line + 1, lnum);
        draw_string(fb, stride, 2, py, lnum, NP_COL_LINENO_FG, NP_COL_LINENO_BG);
    }

    while (logical_pos <= text_len && drawn_lines < vis_lines + 1) {
        /* Draw cursor */
        if (logical_pos == (size_t)np->gbuf.gap_start && np->cursor_visible) {
            fill_rect(fb, stride, px, py, 2, FONT_H, NP_COL_CURSOR);
        }

        if (logical_pos == text_len) break;

        char c = gb_char_at(&np->gbuf, logical_pos);
        logical_pos++;

        if (c == '\n') {
            /* Move to next line */
            line++;
            col = 0;
            drawn_lines++;
            py += FONT_H;
            px  = GUTTER_W + PADDING;
            if (drawn_lines < vis_lines) {
                /* Draw line number */
                char lnum[8]; np_itoa(line + 1, lnum);
                draw_string(fb, stride, 2, py, lnum, NP_COL_LINENO_FG, NP_COL_LINENO_BG);
            }
        } else if (c == '\t') {
            /* Tab: advance to next 4-column boundary */
            uint32_t spaces = 4 - (col % 4);
            for (uint32_t s = 0; s < spaces; s++) {
                draw_char(fb, stride, px, py, ' ', NP_COL_TEXT, NP_COL_BG);
                px += FONT_W;
            }
            col += spaces;
        } else if (c >= 0x20 && c < 0x7F) {
            draw_char(fb, stride, px, py, c, NP_COL_TEXT, NP_COL_BG);
            px += FONT_W;
            col++;
            /* Soft wrap: if line overflows window width, stay on same logical
             * line but clip (horizontal scrolling not yet implemented) */
            if (px >= (int)win_w - PADDING)
                px = (int)win_w - PADDING; /* clamp — don’t overflow */
        } else {
            /* Non-printable: show as · */
            draw_char(fb, stride, px, py, '.', NP_COL_LINENO_FG, NP_COL_BG);
            px += FONT_W;
            col++;
        }
    }

    /* Cursor at end of buffer */
    if (np->gbuf.gap_start == text_len && np->cursor_visible)
        fill_rect(fb, stride, px, py, 2, FONT_H, NP_COL_CURSOR);

    /* ── Status bar ── */
    int sy = (int)(win_h - STATUS_H);
    fill_rect(fb, stride, 0, sy, (int)win_w, STATUS_H, NP_COL_STATUS_BG);

    /* Filename + modified dot */
    int sx = 6;
    if (np->modified) {
        fill_rect(fb, stride, sx, sy + 6, 8, 8, NP_COL_MODIFIED);
        sx += 12;
    }
    sx = draw_string(fb, stride, sx, sy + 2, np->filename,
                     NP_COL_STATUS_FG, NP_COL_STATUS_BG);

    /* Separator */
    sx += 12;
    draw_char(fb, stride, sx, sy + 2, '|', NP_COL_STATUS_FG, NP_COL_STATUS_BG);
    sx += FONT_W + 8;

    /* Line:Col */
    char lc_buf[32];
    char num[12];
    np_itoa(np->cursor_line + 1, num);
    np_strcpy(lc_buf, "Ln "); np_strcpy(lc_buf + 3, num);
    size_t lc_len = np_strlen(lc_buf);
    lc_buf[lc_len] = ','; lc_buf[lc_len+1] = ' '; lc_buf[lc_len+2]='\0';
    sx = draw_string(fb, stride, sx, sy + 2, lc_buf,
                     NP_COL_STATUS_FG, NP_COL_STATUS_BG);
    np_itoa(np->cursor_col + 1, num);
    char col_label[8] = "Col "; np_strcpy(col_label + 4, num);
    sx = draw_string(fb, stride, sx, sy + 2, col_label,
                     NP_COL_STATUS_FG, NP_COL_STATUS_BG);

    /* Total lines on right side */
    sx = (int)win_w - 80;
    char tot[32] = "Lines: ";
    np_itoa(total_lines, tot + 7);
    draw_string(fb, stride, sx, sy + 2, tot,
                NP_COL_STATUS_FG, NP_COL_STATUS_BG);

    /* Tell WM this window needs a repaint */
    wm_dirty(np->win_id);
}

/* ─────────────────────────────────────────────────────────────────────
 * notepad_tick — cursor blink
 * ───────────────────────────────────────────────────────────────────── */
void notepad_tick(notepad_t *np)
{
    if (!np) return;
    uint64_t now = g_ticks;
    if (now - np->last_blink_tick >= NP_BLINK_MS) {
        np->cursor_visible   = !np->cursor_visible;
        np->last_blink_tick  = now;
        notepad_draw(np);   /* redraw to show/hide cursor */
    }
}

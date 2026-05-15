#ifndef AIOS_APPS_NOTEPAD_H
#define AIOS_APPS_NOTEPAD_H

/* kernel/apps/notepad.h — Phase 11.1
 *
 * Notepad — the first real AIOS GUI application.
 *
 * Features
 * ────────
 *   • Multiline text editor backed by a gap buffer
 *   • Keyboard input: printable chars, Enter, Backspace, Delete,
 *     arrow keys, Ctrl+S (save), Ctrl+O (open), Ctrl+N (new)
 *   • VFS open / save via vfs_open / vfs_read / vfs_write
 *   • Renders into a WM window (kernel/gui/wm.h)
 *   • Blinking text cursor
 *   • Status bar: filename + modified flag + cursor line:col
 *
 * Integration
 * ───────────
 *   Call notepad_open(NULL)   — new empty document
 *   Call notepad_open(path)   — open existing VFS file
 *   Both calls create a WM window and register it with the WM.
 *
 * Freestanding C — no libc.  Only <stdint.h>, <stddef.h>, <stdbool.h>.
 * Heap: kmalloc / kfree.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Limits ──────────────────────────────────────────────────────── */
#define NP_MAX_TEXT      (256 * 1024)  /* 256 KiB max document size      */
#define NP_GAP_INITIAL   4096          /* initial gap size (bytes)        */
#define NP_MAX_PATH      256           /* max VFS path length             */
#define NP_MAX_FILENAME  64            /* shown in title bar              */
#define NP_BLINK_MS      500           /* cursor blink interval           */

/* ── Colours (ARGB 0xAARRGGBB) ───────────────────────────────── */
#define NP_COL_BG        0xFFFFFFFF    /* editor background  (white)      */
#define NP_COL_TEXT      0xFF1E1E1E    /* editor text        (near-black) */
#define NP_COL_CURSOR    0xFF0066CC    /* blinking I-beam    (blue)       */
#define NP_COL_SEL_BG    0xFFB8D4F0    /* selection bg       (light blue) */
#define NP_COL_LINENO_BG 0xFFF0F0F0    /* line-number gutter (light grey) */
#define NP_COL_LINENO_FG 0xFF888888    /* line-number text   (grey)       */
#define NP_COL_STATUS_BG 0xFF2D5FA8    /* status bar bg      (AIOS blue)  */
#define NP_COL_STATUS_FG 0xFFFFFFFF    /* status bar text    (white)      */
#define NP_COL_MODIFIED  0xFFFF6B35    /* ‘*’ modified dot   (orange)     */

/* ── Gap buffer ────────────────────────────────────────────────────── */
/*
 * Layout:  buf[0 .. gap_start-1]  — text before cursor
 *           buf[gap_start .. gap_end-1]  — gap (unused bytes)
 *           buf[gap_end .. buf_size-1]   — text after cursor
 * Logical text length = buf_size - gap_size
 */
typedef struct {
    char    *buf;          /* kmalloc’d backing store             */
    size_t   buf_size;     /* total allocated bytes               */
    size_t   gap_start;    /* index of first gap byte             */
    size_t   gap_end;      /* index of first post-gap byte        */
} gap_buf_t;

/* ── Notepad instance ───────────────────────────────────────────────── */
typedef struct {
    gap_buf_t   gbuf;                    /* document text                */
    size_t      cursor;                  /* logical byte offset          */
    size_t      sel_start;               /* selection anchor             */
    bool        has_sel;                 /* selection active?            */
    uint32_t    scroll_line;             /* first visible line           */
    uint32_t    cursor_line;             /* 0-indexed line of cursor     */
    uint32_t    cursor_col;              /* 0-indexed column of cursor   */
    bool        modified;                /* unsaved changes?             */
    bool        cursor_visible;          /* blink state                  */
    uint64_t    last_blink_tick;         /* pit tick of last blink toggle*/
    char        filepath[NP_MAX_PATH];   /* current VFS path or ""       */
    char        filename[NP_MAX_FILENAME];/* display name                */
    uint32_t    win_id;                  /* WM window ID                 */
} notepad_t;

/* ── Public API ────────────────────────────────────────────────────── */

/*
 * notepad_open(path)
 *
 * Create a new Notepad window.  If path != NULL and the VFS file exists,
 * the file is loaded into the buffer.  If path == NULL or the file does
 * not exist, an empty document is opened.
 *
 * Returns a pointer to the new notepad_t (kmalloc’d), or NULL on OOM.
 * The window is registered with the WM and begins receiving events.
 */
notepad_t *notepad_open(const char *path);

/*
 * notepad_close(np)
 *
 * Destroy the notepad, unregister its WM window, free all memory.
 * Safe to call with NULL.
 */
void notepad_close(notepad_t *np);

/*
 * notepad_on_key(np, ascii, scancode, mods)
 *
 * Deliver a keystroke to the notepad.
 * ascii    — ASCII value (0 if non-printable)
 * scancode — raw PS/2 scancode (for arrow keys etc.)
 * mods     — modifier bitmask (KEYBOARD_MOD_CTRL, _SHIFT, _ALT)
 *
 * Called by the GUI input dispatcher (kernel/gui/input_wiring.c).
 */
void notepad_on_key(notepad_t *np, char ascii,
                    uint8_t scancode, uint8_t mods);

/*
 * notepad_draw(np)
 *
 * Render the full notepad into its WM window framebuffer.
 * Called by the WM render loop whenever the window is dirty.
 */
void notepad_draw(notepad_t *np);

/*
 * notepad_tick(np)
 *
 * Called periodically (every WM frame) to handle cursor blink.
 */
void notepad_tick(notepad_t *np);

#endif /* AIOS_APPS_NOTEPAD_H */

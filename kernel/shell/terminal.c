/*
 * AIOS  —  Phase 5.1  Terminal Emulator
 *
 * Build constraints:
 *   -ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel
 *   Only <stdint.h>, <stddef.h>, <stdbool.h> permitted.
 *
 * This file owns:
 *   1. SPSC ring buffer  (keyboard ISR → readline loop)
 *   2. Line editor       (backspace, left/right, home/end, del)
 *   3. History ring      (32 × 256 chars, up/down navigation)
 *   4. terminal_readline (blocking, sched_yield while idle)
 *   5. ANSI emitter      (term_move_cursor, term_set_color, …)
 */

#include "terminal.h"
#include "../vga.h"       /* vga_putchar, vga_set_color, vga_puts, vga_clear */
#include "../sync.h"      /* spinlock_t, spin_lock_irqsave, spin_unlock_irqrestore */
#include "../sched.h"     /* sched_yield */

/* ── internal helpers ───────────────────────────────────────────────── */
static inline size_t kstrlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
static inline void kmemcpy_fwd(char *dst, const char *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}
static inline void kmemmove(char *dst, const char *src, size_t n) {
    if (dst < src)
        for (size_t i = 0; i < n; i++) dst[i] = src[i];
    else
        for (size_t i = n; i > 0; i--) dst[i-1] = src[i-1];
}
static inline void kmemset_c(char *dst, char c, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = c;
}

/* ====================================================================
 * 1.  SPSC Input Ring Buffer
 *
 * The keyboard ISR calls terminal_feed() to push virtual-key codes.
 * terminal_readline() drains it.  No mutex needed — head is written
 * only by the consumer, tail only by the producer (ISR).  A spinlock
 * protects tail updates so that two ISR contexts (shouldn't happen on
 * x86 with LAPIC, but guard anyway) can't race.
 * ================================================================== */

#define RING_MASK (TERM_INPUT_BUF - 1)

typedef struct {
    volatile uint8_t  buf[TERM_INPUT_BUF];
    volatile uint32_t head;   /* consumer index */
    volatile uint32_t tail;   /* producer index */
    spinlock_t        lock;   /* protects tail  */
} ring_t;

static ring_t g_ring = {
    .head = 0,
    .tail = 0,
    .lock = SPINLOCK_INIT
};

void terminal_feed(uint8_t key) {
    uint64_t fl = spin_lock_irqsave(&g_ring.lock);
    uint32_t next = (g_ring.tail + 1) & RING_MASK;
    if (next != g_ring.head) {      /* drop on overflow */
        g_ring.buf[g_ring.tail] = key;
        /* Ensure write is visible before tail update.              */
        /* On x86 stores are TSO-ordered; a compiler barrier is     */
        /* sufficient.                                              */
        __asm__ volatile("" ::: "memory");
        g_ring.tail = next;
    }
    spin_unlock_irqrestore(&g_ring.lock, fl);
}

/* Blocking dequeue — yields CPU while empty. */
static uint8_t ring_dequeue(void) {
    while (g_ring.head == g_ring.tail)
        sched_yield();
    uint8_t val = g_ring.buf[g_ring.head];
    __asm__ volatile("" ::: "memory");
    g_ring.head = (g_ring.head + 1) & RING_MASK;
    return val;
}

/* Non-blocking peek — returns false if empty. */
static bool ring_try_dequeue(uint8_t *out) {
    if (g_ring.head == g_ring.tail) return false;
    *out = g_ring.buf[g_ring.head];
    __asm__ volatile("" ::: "memory");
    g_ring.head = (g_ring.head + 1) & RING_MASK;
    return true;
}
(void)ring_try_dequeue; /* suppress unused-function warning */

/* ====================================================================
 * 2.  History Ring
 * ================================================================== */

typedef struct {
    char     entries[TERM_HISTORY_DEPTH][TERM_HISTORY_LEN];
    uint32_t write_idx;   /* next slot to write (mod DEPTH) */
    uint32_t count;       /* total entries stored, max DEPTH */
} history_t;

static history_t g_hist = { .write_idx = 0, .count = 0 };

void term_history_push(const char *line) {
    if (!line || !line[0]) return;
    size_t len = kstrlen(line);
    if (len == 0) return;
    if (len >= TERM_HISTORY_LEN) len = TERM_HISTORY_LEN - 1;
    kmemcpy_fwd(g_hist.entries[g_hist.write_idx], line, len);
    g_hist.entries[g_hist.write_idx][len] = '\0';
    g_hist.write_idx = (g_hist.write_idx + 1) % TERM_HISTORY_DEPTH;
    if (g_hist.count < TERM_HISTORY_DEPTH) g_hist.count++;
}

bool term_history_get(uint32_t offset, char *buf, size_t maxlen) {
    if (offset >= g_hist.count) return false;
    /* offset 0 = most recent = write_idx - 1 */
    uint32_t idx = (g_hist.write_idx + TERM_HISTORY_DEPTH - 1 - offset)
                   % TERM_HISTORY_DEPTH;
    const char *src = g_hist.entries[idx];
    size_t len = kstrlen(src);
    if (len >= maxlen) len = maxlen - 1;
    kmemcpy_fwd(buf, src, len);
    buf[len] = '\0';
    return true;
}

uint32_t term_history_count(void) {
    return g_hist.count;
}

/* ====================================================================
 * 3.  VGA state mirror
 *
 * We mirror the VGA cursor position in software so we can do
 * correct line-redraws without extra CRTC reads.
 * ================================================================== */

static uint8_t g_cur_col = 0;
static uint8_t g_cur_row = 0;
static uint8_t g_fg      = TERM_FG_LGRAY;
static uint8_t g_bg      = TERM_BG_BLACK;

void term_move_cursor(uint8_t col, uint8_t row) {
    if (col >= TERM_COLS) col = TERM_COLS - 1;
    if (row >= TERM_ROWS) row = TERM_ROWS - 1;
    g_cur_col = col;
    g_cur_row = row;
    vga_set_cursor(col, row);   /* direct CRTC write in vga.c */
}

void term_set_color(uint8_t fg, uint8_t bg) {
    g_fg = fg & 0xF;
    g_bg = bg & 0x7;   /* blink bit deliberately omitted */
    vga_set_color(fg, bg);
}

void term_reset_color(void) {
    term_set_color(TERM_FG_LGRAY, TERM_BG_BLACK);
}

uint8_t term_cursor_col(void) { return g_cur_col; }
uint8_t term_cursor_row(void) { return g_cur_row; }

/* ====================================================================
 * 4.  Line-level VGA helpers
 * ================================================================== */

void term_clear_line_to_end(void) {
    uint8_t c = g_cur_col;
    while (c < TERM_COLS) {
        vga_putchar_at(' ', g_fg, g_bg, c, g_cur_row);
        c++;
    }
}

void term_clear_line(void) {
    for (uint8_t c = 0; c < TERM_COLS; c++)
        vga_putchar_at(' ', g_fg, g_bg, c, g_cur_row);
    term_move_cursor(0, g_cur_row);
}

void term_clear_screen(void) {
    vga_clear();
    g_cur_col = 0;
    g_cur_row = 0;
}

/* ====================================================================
 * 5.  Output API
 * ================================================================== */

void terminal_write_char(char c) {
    vga_putchar(c);
    /* Update mirror — vga.c handles scroll internally; we rely on
     * vga_get_cursor() to re-sync after every character.           */
    vga_get_cursor(&g_cur_col, &g_cur_row);
}

void terminal_write_len(const char *str, size_t len) {
    for (size_t i = 0; i < len; i++)
        terminal_write_char(str[i]);
}

void terminal_write(const char *str) {
    terminal_write_len(str, kstrlen(str));
}

/* ====================================================================
 * 6.  terminal_readline — line editor
 *
 * Editing model:
 *   buf[0..len-1]  — current line content
 *   pos            — cursor position within buf (0..len)
 *   hist_offset    — 0 means "current draft", 1+ means history[n-1]
 *   draft[256]     — saved draft when user starts browsing history
 *
 * Redraw strategy (no scrolling, line fits on one row):
 *   After every edit operation, erase from prompt_col to EOL, then
 *   re-emit buf from the start, then reposition cursor at pos.
 * ================================================================== */

typedef struct {
    char     buf[TERM_LINE_MAX];   /* current edit buffer */
    char     draft[TERM_LINE_MAX]; /* saved draft during history browse */
    size_t   len;                  /* number of chars in buf */
    size_t   pos;                  /* cursor offset in buf */
    uint32_t hist_offset;          /* 0 = live draft */
    uint8_t  prompt_col;           /* column where line editing started */
    uint8_t  prompt_row;           /* row  where line editing started */
} editor_t;

/* Redraw from prompt_col onwards. */
static void editor_redraw(const editor_t *e) {
    term_move_cursor(e->prompt_col, e->prompt_row);
    terminal_write_len(e->buf, e->len);
    /* erase any leftover chars from a previous longer line */
    uint8_t end_col = (uint8_t)(e->prompt_col + e->len);
    if (end_col < TERM_COLS) {
        for (uint8_t c = end_col; c < TERM_COLS; c++)
            vga_putchar_at(' ', g_fg, g_bg, c, e->prompt_row);
    }
    /* reposition cursor */
    uint8_t new_col = (uint8_t)(e->prompt_col + e->pos);
    term_move_cursor(new_col, e->prompt_row);
}

/* Load a history entry into the editor. */
static void editor_load_history(editor_t *e, uint32_t offset) {
    char tmp[TERM_HISTORY_LEN];
    if (!term_history_get(offset, tmp, sizeof(tmp))) return;
    e->hist_offset = offset;
    size_t l = kstrlen(tmp);
    if (l >= TERM_LINE_MAX) l = TERM_LINE_MAX - 1;
    kmemcpy_fwd(e->buf, tmp, l);
    e->buf[l] = '\0';
    e->len = l;
    e->pos = l;   /* cursor at end */
    editor_redraw(e);
}

size_t terminal_readline(char *out, size_t maxlen) {
    if (!out || maxlen == 0) return 0;

    editor_t e;
    kmemset_c(e.buf,   0, sizeof(e.buf));
    kmemset_c(e.draft, 0, sizeof(e.draft));
    e.len          = 0;
    e.pos          = 0;
    e.hist_offset  = 0;
    vga_get_cursor(&e.prompt_col, &e.prompt_row);

    for (;;) {
        uint8_t key = ring_dequeue();

        switch (key) {

        /* ── Enter ─────────────────────────────────────────────── */
        case TERM_KEY_ENTER:
            terminal_write_char('\n');
            /* commit: NUL-terminate, copy to caller */
            e.buf[e.len] = '\0';
            term_history_push(e.buf);
            {
                size_t copy = e.len < maxlen - 1 ? e.len : maxlen - 1;
                kmemcpy_fwd(out, e.buf, copy);
                out[copy] = '\0';
                return copy;
            }

        /* ── Backspace ─────────────────────────────────────────── */
        case TERM_KEY_BACKSPACE:
            if (e.pos == 0) break;
            /* delete char at pos-1 */
            kmemmove(&e.buf[e.pos - 1], &e.buf[e.pos], e.len - e.pos);
            e.len--;
            e.pos--;
            e.buf[e.len] = '\0';
            editor_redraw(&e);
            break;

        /* ── Delete key ────────────────────────────────────────── */
        case TERM_KEY_DEL:
            if (e.pos >= e.len) break;
            kmemmove(&e.buf[e.pos], &e.buf[e.pos + 1], e.len - e.pos - 1);
            e.len--;
            e.buf[e.len] = '\0';
            editor_redraw(&e);
            break;

        /* ── Left arrow ────────────────────────────────────────── */
        case TERM_KEY_LEFT:
            if (e.pos == 0) break;
            e.pos--;
            term_move_cursor((uint8_t)(e.prompt_col + e.pos), e.prompt_row);
            break;

        /* ── Right arrow ───────────────────────────────────────── */
        case TERM_KEY_RIGHT:
            if (e.pos >= e.len) break;
            e.pos++;
            term_move_cursor((uint8_t)(e.prompt_col + e.pos), e.prompt_row);
            break;

        /* ── Home ──────────────────────────────────────────────── */
        case TERM_KEY_HOME:
            e.pos = 0;
            term_move_cursor(e.prompt_col, e.prompt_row);
            break;

        /* ── End ───────────────────────────────────────────────── */
        case TERM_KEY_END:
            e.pos = e.len;
            term_move_cursor((uint8_t)(e.prompt_col + e.len), e.prompt_row);
            break;

        /* ── Up arrow (history prev) ───────────────────────────── */
        case TERM_KEY_UP:
            if (g_hist.count == 0) break;
            if (e.hist_offset == 0) {
                /* save live draft */
                kmemcpy_fwd(e.draft, e.buf, e.len + 1);
            }
            if (e.hist_offset < g_hist.count)
                editor_load_history(&e, e.hist_offset + 1);
            break;

        /* ── Down arrow (history next / restore draft) ─────────── */
        case TERM_KEY_DOWN:
            if (e.hist_offset == 0) break;
            if (e.hist_offset == 1) {
                /* restore live draft */
                e.hist_offset = 0;
                size_t dl = kstrlen(e.draft);
                kmemcpy_fwd(e.buf, e.draft, dl + 1);
                e.len = dl;
                e.pos = dl;
                editor_redraw(&e);
            } else {
                editor_load_history(&e, e.hist_offset - 1);
            }
            break;

        /* ── Tab (reserved for future completion) ──────────────── */
        case TERM_KEY_TAB:
            /* TODO Phase 5.2: tab completion */
            break;

        /* ── Printable character ───────────────────────────────── */
        default:
            if (key < 0x20 || key > 0x7E) break;   /* ignore non-printable */
            if (e.len + 1 >= TERM_LINE_MAX) break;  /* line full */
            /* insert at pos */
            if (e.pos < e.len)
                kmemmove(&e.buf[e.pos + 1], &e.buf[e.pos], e.len - e.pos);
            e.buf[e.pos] = (char)key;
            e.len++;
            e.pos++;
            e.buf[e.len] = '\0';
            /* reset history browsing */
            e.hist_offset = 0;
            editor_redraw(&e);
            break;
        }
    }
    /* unreachable */
    return 0;
}

/* ====================================================================
 * 7.  terminal_init
 * ================================================================== */

void terminal_init(void) {
    /* ring already zero-initialised via BSS */
    vga_clear();
    g_cur_col = 0;
    g_cur_row = 0;
    g_fg      = TERM_FG_LGRAY;
    g_bg      = TERM_BG_BLACK;

    /* Banner */
    term_set_color(TERM_FG_LCYAN, TERM_BG_BLACK);
    terminal_write("AIOS Terminal Ready\n");
    term_reset_color();
}

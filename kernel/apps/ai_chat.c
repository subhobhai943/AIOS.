/*
 * kernel/apps/ai_chat.c — Phase 11.5
 *
 * GUI AI Chat application — thin frontend for the LLM inference
 * manager (kernel/llm/inference.c).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "ai_chat.h"
#include "../heap.h"
#include "../gfx/framebuffer.h"
#include "../gfx/font.h"
#include "../gui/window.h"
#include "../gui/input.h"
#include "../llm/inference.h"
#include "../serial.h"

#define AI_CHAT_LOG_DEFAULT  (16u * 1024u)
#define AI_CHAT_INPUT_DEFAULT 256u

#define FONT_W 8
#define FONT_H 16
#define PADDING 4
#define INPUT_H (FONT_H + 8)

#define COL_BG       0xFF101010u
#define COL_TEXT     0xFFEFEFEFu
#define COL_USER     0xFF7CCFFFu
#define COL_ASSIST   0xFFB4F08Cu
#define COL_INPUT_BG 0xFF202020u
#define COL_INPUT_FG 0xFFFFFFFFu

static ai_chat_t *g_chat = 0;

static void ac_memcpy(char *dst, const char *src, size_t n)
{
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}

static size_t ac_strlen(const char *s)
{
    size_t n = 0; while (s && s[n]) n++; return n;
}

static void log_append(ai_chat_t *chat, const char *prefix, uint32_t prefix_col,
                       const char *text, size_t len)
{
    if (!chat || !text) return;
    /* Simple: prepend prefix token like "You: " or "AI: " inline. */
    const char *pfx = prefix;
    size_t pfx_len = ac_strlen(pfx);
    size_t need = pfx_len + len + 1; /* + newline */

    if (chat->log_len + need >= chat->log_size) {
        size_t new_size = chat->log_size + AI_CHAT_LOG_DEFAULT;
        char *nb = (char*)kmalloc(new_size);
        if (!nb) return;
        ac_memcpy(nb, chat->log_buf, chat->log_len);
        kfree(chat->log_buf);
        chat->log_buf = nb;
        chat->log_size = new_size;
    }

    char *dst = chat->log_buf + chat->log_len;
    if (pfx_len) {
        ac_memcpy(dst, pfx, pfx_len);
        dst += pfx_len;
        chat->log_len += pfx_len;
    }
    ac_memcpy(dst, text, len);
    dst[len] = '\n';
    chat->log_len += len + 1;
}

/* --- Inference callback --------------------------------------------- */

static void ai_chat_token_cb(const char *text, size_t len, void *user)
{
    (void)user;
    if (!g_chat) return;
    log_append(g_chat, "", COL_ASSIST, text, len);
    /* Redraw after each token chunk */
    gui_window_t *win = gui_get_window(g_chat->win_id);
    if (win) gui_invalidate(win);
}

/* --- Rendering ------------------------------------------------------ */

static void ac_fill_rect(framebuffer_t *fb, int x, int y, int w, int h, uint32_t col)
{
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= (int)fb->height) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= (int)fb->width) continue;
            fb_put_pixel((uint32_t)xx, (uint32_t)yy, col);
        }
    }
}

static void ac_draw_string(framebuffer_t *fb, gui_font_t *font,
                           int x, int y, const char *s, uint32_t col)
{
    while (s && *s) {
        font_draw_char(fb, font, x, y, *s, col, COL_BG);
        x += FONT_W;
        s++;
    }
}

static void ai_chat_draw(gui_window_t *win, framebuffer_t *fb)
{
    ai_chat_t *chat = (ai_chat_t*)win->user_data;
    if (!chat || !fb) return;

    gui_font_t *font = gui_get_default_font();
    int x0 = win->x;
    int y0 = win->y;
    int w  = (int)win->width;
    int h  = (int)win->height;

    /* Background */
    ac_fill_rect(fb, x0, y0, w, h, COL_BG);

    /* Input box at bottom */
    int input_y = y0 + h - INPUT_H;
    ac_fill_rect(fb, x0, input_y, w, INPUT_H, COL_INPUT_BG);

    /* Draw input text */
    if (chat->input_buf && chat->input_len) {
        char *tmp = chat->input_buf;
        int tx = x0 + PADDING;
        int ty = input_y + (INPUT_H - FONT_H) / 2;
        for (size_t i = 0; i < chat->input_len; i++) {
            font_draw_char(fb, font, tx, ty, tmp[i], COL_INPUT_FG, COL_INPUT_BG);
            tx += FONT_W;
        }
    }

    /* Chat log area above input */
    int log_h = h - INPUT_H - PADDING;
    int log_y = y0 + PADDING;

    /* Very simple: draw from start, wrap lines at window width. */
    int cur_x = x0 + PADDING;
    int cur_y = log_y;
    for (size_t i = 0; i < chat->log_len; i++) {
        char c = chat->log_buf[i];
        if (c == '\n') {
            cur_x = x0 + PADDING;
            cur_y += FONT_H;
            if (cur_y + FONT_H >= input_y) break; /* no more space */
            continue;
        }
        font_draw_char(fb, font, cur_x, cur_y, c, COL_TEXT, COL_BG);
        cur_x += FONT_W;
        if (cur_x + FONT_W >= x0 + w - PADDING) {
            cur_x = x0 + PADDING;
            cur_y += FONT_H;
            if (cur_y + FONT_H >= input_y) break;
        }
    }
}

/* --- Event handling ------------------------------------------------- */

static void ai_chat_handle_event(gui_window_t *win, const gui_event_t *ev)
{
    ai_chat_t *chat = (ai_chat_t*)win->user_data;
    if (!chat || !ev) return;

    if (ev->type == GUI_EVENT_KEY_DOWN) {
        char ascii = (char)ev->ascii;
        uint8_t key = ev->keycode;

        /* Enter: send message */
        if (ascii == '\r' || ascii == '\n') {
            if (chat->input_len > 0) {
                /* Append user message to log */
                log_append(chat, "You: ", COL_USER,
                           chat->input_buf, chat->input_len);

                /* Call inference */
                inference_generate(chat->input_buf, chat->input_len,
                                   ai_chat_token_cb, 0);

                chat->input_len = 0;
                gui_invalidate(win);
            }
            return;
        }

        /* Backspace */
        if (ascii == '\b' || key == 0x0E) {
            if (chat->input_len > 0) chat->input_len--;
            gui_invalidate(win);
            return;
        }

        /* Printable */
        if (ascii >= 0x20 && ascii < 0x7F) {
            if (chat->input_len + 1 < chat->input_size) {
                chat->input_buf[chat->input_len++] = ascii;
                gui_invalidate(win);
            }
        }
    }
}

/* --- Public API ----------------------------------------------------- */

ai_chat_t *ai_chat_open(void)
{
    if (g_chat) return g_chat;

    ai_chat_t *chat = (ai_chat_t*)kmalloc(sizeof(ai_chat_t));
    if (!chat) return 0;

    chat->log_size = AI_CHAT_LOG_DEFAULT;
    chat->log_buf  = (char*)kmalloc(chat->log_size);
    chat->log_len  = 0;

    chat->input_size = AI_CHAT_INPUT_DEFAULT;
    chat->input_buf  = (char*)kmalloc(chat->input_size);
    chat->input_len  = 0;

    if (!chat->log_buf || !chat->input_buf) {
        if (chat->log_buf) kfree(chat->log_buf);
        if (chat->input_buf) kfree(chat->input_buf);
        kfree(chat);
        return 0;
    }

    uint32_t w = 520;
    uint32_t h = 360;
    gui_window_t *win = gui_create_window(140, 100, w, h,
                                          "AI Chat",
                                          ai_chat_draw,
                                          ai_chat_handle_event,
                                          chat);
    if (!win) {
        kfree(chat->log_buf);
        kfree(chat->input_buf);
        kfree(chat);
        return 0;
    }

    chat->win_id = win->id;
    g_chat = chat;

    /* Initial system message */
    const char *hello = "AI Chat ready. Type a message and press Enter.";
    log_append(chat, "", COL_ASSIST, hello, ac_strlen(hello));

    return chat;
}

void ai_chat_close(ai_chat_t *chat)
{
    if (!chat) return;
    if (g_chat == chat) g_chat = 0;
    if (chat->log_buf) kfree(chat->log_buf);
    if (chat->input_buf) kfree(chat->input_buf);
    kfree(chat);
}

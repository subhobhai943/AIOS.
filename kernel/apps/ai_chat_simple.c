#include "ai_chat.h"

#include "../heap.h"
#include "../gfx/font.h"
#include "../gui/input.h"
#include "../gui/window.h"

#include <stdint.h>
#include <stddef.h>

#define AI_CHAT_LOG_DEFAULT  (16u * 1024u)
#define AI_CHAT_INPUT_DEFAULT 256u

#define CHAT_FONT_W 8
#define CHAT_FONT_H 16
#define CHAT_PAD 6
#define CHAT_INPUT_H (CHAT_FONT_H + 10)

#define CHAT_COL_BG       0xFF101418u
#define CHAT_COL_TEXT     0xFFECE7D8u
#define CHAT_COL_ASSIST   0xFFA8E07Au
#define CHAT_COL_INPUT_BG 0xFF202830u
#define CHAT_COL_INPUT_FG 0xFFFFFFFFu

static ai_chat_t *g_chat = 0;

static void chat_memcpy(char *dst, const char *src, size_t n)
{
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}

static void chat_memset(void *p, uint8_t v, size_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = v;
}

static size_t chat_strlen(const char *s)
{
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void log_append(ai_chat_t *chat, const char *prefix, const char *text, size_t len)
{
    if (!chat || !text) return;
    size_t pfx_len = chat_strlen(prefix);
    size_t need = pfx_len + len + 1u;
    if (chat->log_len + need >= chat->log_size) return;

    char *dst = chat->log_buf + chat->log_len;
    if (pfx_len) {
        chat_memcpy(dst, prefix, pfx_len);
        dst += pfx_len;
        chat->log_len += pfx_len;
    }
    chat_memcpy(dst, text, len);
    dst[len] = '\n';
    chat->log_len += len + 1u;
}

static void chat_fill_rect(framebuffer_t *fb, int x, int y, int w, int h, uint32_t col)
{
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= (int)fb->height) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= (int)fb->width) continue;
            fb_put_pixel((uint32_t)xx, (uint32_t)yy, col);
        }
    }
}

static void chat_draw_char(framebuffer_t *fb,
                           const gui_font_t *font,
                           int x,
                           int y,
                           char c,
                           uint32_t fg,
                           uint32_t bg)
{
    font_draw_char(fb, font, (uint32_t)x, (uint32_t)y, c, fg, bg);
}

static void ai_chat_draw(gui_window_t *win, framebuffer_t *fb)
{
    ai_chat_t *chat = (ai_chat_t *)win->user_data;
    if (!chat || !fb) return;
    const gui_font_t *font = font_load_builtin();

    int x0 = win->x;
    int y0 = win->y + 20;
    int w = (int)win->width;
    int h = (int)win->height - 20;
    int input_y = y0 + h - CHAT_INPUT_H;

    chat_fill_rect(fb, x0 + 1, y0, w - 2, h, CHAT_COL_BG);
    chat_fill_rect(fb, x0 + 1, input_y, w - 2, CHAT_INPUT_H, CHAT_COL_INPUT_BG);

    int cur_x = x0 + CHAT_PAD;
    int cur_y = y0 + CHAT_PAD;
    for (size_t i = 0; i < chat->log_len; i++) {
        char c = chat->log_buf[i];
        if (c == '\n') {
            cur_x = x0 + CHAT_PAD;
            cur_y += CHAT_FONT_H;
            if (cur_y + CHAT_FONT_H >= input_y) break;
            continue;
        }
        chat_draw_char(fb, font, cur_x, cur_y, c, CHAT_COL_TEXT, CHAT_COL_BG);
        cur_x += CHAT_FONT_W;
        if (cur_x + CHAT_FONT_W >= x0 + w - CHAT_PAD) {
            cur_x = x0 + CHAT_PAD;
            cur_y += CHAT_FONT_H;
            if (cur_y + CHAT_FONT_H >= input_y) break;
        }
    }

    int tx = x0 + CHAT_PAD;
    int ty = input_y + (CHAT_INPUT_H - CHAT_FONT_H) / 2;
    for (size_t i = 0; i < chat->input_len; i++) {
        chat_draw_char(fb, font, tx, ty, chat->input_buf[i],
                       CHAT_COL_INPUT_FG, CHAT_COL_INPUT_BG);
        tx += CHAT_FONT_W;
    }
    chat_fill_rect(fb, tx, ty, 2, CHAT_FONT_H, CHAT_COL_ASSIST);
}

static void ai_chat_handle_event(gui_window_t *win, const gui_event_t *ev)
{
    ai_chat_t *chat = (ai_chat_t *)win->user_data;
    if (!chat || !ev || ev->type != GUI_EVENT_KEY_DOWN) return;

    uint8_t key = ev->keycode;
    if (key == '\b' || key == 0x0Eu) {
        if (chat->input_len > 0) chat->input_len--;
        return;
    }

    if (key == '\r' || key == '\n' || key == 0x1Cu) {
        if (chat->input_len == 0) return;
        log_append(chat, "You: ", chat->input_buf, chat->input_len);
        const char *reply = "AI: LLM backend disabled in this debug build; GUI input is working.";
        log_append(chat, "", reply, chat_strlen(reply));
        chat->input_len = 0;
        return;
    }

    if (key >= 0x20u && key < 0x7Fu && chat->input_len + 1u < chat->input_size) {
        chat->input_buf[chat->input_len++] = (char)key;
    }
}

ai_chat_t *ai_chat_open(void)
{
    if (g_chat) return g_chat;

    ai_chat_t *chat = (ai_chat_t *)kmalloc(sizeof(ai_chat_t));
    if (!chat) return 0;
    chat_memset(chat, 0, sizeof(ai_chat_t));

    chat->log_size = AI_CHAT_LOG_DEFAULT;
    chat->log_buf = (char *)kmalloc(chat->log_size);
    chat->input_size = AI_CHAT_INPUT_DEFAULT;
    chat->input_buf = (char *)kmalloc(chat->input_size);
    if (!chat->log_buf || !chat->input_buf) {
        if (chat->log_buf) kfree(chat->log_buf);
        if (chat->input_buf) kfree(chat->input_buf);
        kfree(chat);
        return 0;
    }
    chat_memset(chat->log_buf, 0, chat->log_size);
    chat_memset(chat->input_buf, 0, chat->input_size);

    gui_window_t *win = gui_create_window(150, 100, 540, 360,
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

    const char *hello = "AI Chat ready. Press Enter after typing.";
    log_append(chat, "", hello, chat_strlen(hello));
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

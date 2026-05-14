#include "gfx/font.h"
#include "gfx/colors.h"

/* 8x16 bitmap for ASCII 32..127. This is a very small, placeholder font. Each
 * character is 16 bytes (rows), each byte is 8 bits wide. We pack them into a
 * flat array indexed as: (ch - 32) * 16 + row.
 *
 * For brevity and to keep the kernel small, we define only a minimal subset
 * and fall back to blank glyphs for the rest.
 */

static const gui_font_t g_builtin_font = {
    .width  = 8,
    .height = 16,
};

/* Very small glyph table: only a few characters, rest blank. */
static const uint8_t g_font_bitmap[96][16] = {
    /* 0: ' ' (32) */
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    /* 1: '!' (33) */
    { 0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0,0,0,0,0 },
    /* 2: '"' (34) */
    { 0x6c,0x6c,0x24,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    /* 3: '#' (35) */
    { 0x24,0x24,0x7e,0x24,0x24,0x24,0x7e,0x24,0x24,0,0,0,0,0,0,0 },
    /* 4: '$' (36) */
    { 0x18,0x3c,0x66,0x06,0x1c,0x30,0x66,0x3c,0x18,0x18,0,0,0,0,0,0 },
    /* 5: '%' (37) */
    { 0x62,0x66,0x0c,0x18,0x30,0x60,0x66,0x46,0,0,0,0,0,0,0,0 },
    /* 6: '&' (38) */
    { 0x18,0x3c,0x3c,0x18,0x3a,0x6e,0x66,0x3b,0,0,0,0,0,0,0,0 },
    /* 7: '\'' (39) */
    { 0x18,0x18,0x10,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    /* 8: '(' (40) */
    { 0x0c,0x18,0x30,0x30,0x30,0x30,0x30,0x18,0x0c,0,0,0,0,0,0,0 },
    /* 9: ')' (41) */
    { 0x30,0x18,0x0c,0x0c,0x0c,0x0c,0x0c,0x18,0x30,0,0,0,0,0,0,0 },
    /* ... all other glyphs left as zero (blank) for now ... */
};

static const uint8_t *font_get_glyph(char ch) {
    if (ch < 32 || ch > 127) return g_font_bitmap[0];
    return g_font_bitmap[(int)(ch - 32)];
}

const gui_font_t *font_load_builtin(void) {
    return &g_builtin_font;
}

uint32_t font_measure_string(const gui_font_t *font, const char *s) {
    uint32_t len = 0;
    while (*s) {
        if (*s == '\n') break;
        len++;
        s++;
    }
    return (uint32_t)len * font->width;
}

void font_draw_char(framebuffer_t *fb,
                    const gui_font_t *font,
                    uint32_t x,
                    uint32_t y,
                    char ch,
                    uint32_t fg,
                    uint32_t bg)
{
    const uint8_t *glyph = font_get_glyph(ch);

    /* Fill background rectangle first. */
    fb_fill_rect(x, y, font->width, font->height, bg);

    for (uint32_t row = 0; row < font->height; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < font->width; col++) {
            if (bits & (0x80u >> col)) {
                fb_put_pixel(x + col, y + row, fg);
            }
        }
    }
}

void font_draw_string(framebuffer_t *fb,
                      const gui_font_t *font,
                      uint32_t x,
                      uint32_t y,
                      const char *s,
                      uint32_t fg,
                      uint32_t bg)
{
    uint32_t cx = x;
    while (*s) {
        char ch = *s++;
        if (ch == '\n') break;
        font_draw_char(fb, font, cx, y, ch, fg, bg);
        cx += font->width;
    }
}

static void font_truncate_with_ellipsis(char *buf, uint32_t buf_size)
{
    if (buf_size < 4) return;
    uint32_t len = 0;
    while (len < buf_size - 1 && buf[len]) len++;
    if (len < 3) return;
    buf[len - 3] = '.';
    buf[len - 2] = '.';
    buf[len - 1] = '.';
}

void font_draw_string_centered(framebuffer_t *fb,
                               const gui_font_t *font,
                               uint32_t region_x,
                               uint32_t region_y,
                               uint32_t region_w,
                               const char *s,
                               uint32_t fg,
                               uint32_t bg)
{
    /* Copy into a small local buffer so we can truncate. */
    char tmp[64];
    uint32_t i = 0;
    while (s[i] && i < (sizeof(tmp) - 1)) {
        tmp[i] = s[i];
        i++;
    }
    tmp[i] = '\0';

    uint32_t text_w = font_measure_string(font, tmp);
    if (text_w > region_w && region_w >= font->width * 3) {
        /* Rough truncation: keep only as many chars as fit, then ellipsis. */
        uint32_t max_chars = region_w / font->width;
        if (max_chars >= 3 && max_chars < (sizeof(tmp) - 1)) {
            tmp[max_chars] = '\0';
            font_truncate_with_ellipsis(tmp, max_chars + 1);
        }
        text_w = font_measure_string(font, tmp);
    }

    uint32_t start_x = region_x;
    if (region_w > text_w) {
        start_x = region_x + (region_w - text_w) / 2u;
    }

    font_draw_string(fb, font, start_x, region_y, tmp, fg, bg);
}

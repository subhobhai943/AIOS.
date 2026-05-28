#include "framebuffer.h"
#include "include/vmm.h"
#include "serial.h"

#ifndef PHYS_TO_VIRT
#  define PHYS_TO_VIRT(p) ((void *)(uintptr_t)(p))
#endif

#define MB2_TAG_TYPE_END         0
#define MB2_TAG_TYPE_FRAMEBUFFER 8
#define FB_PAGE_SIZE             4096u

typedef struct __attribute__((packed)) {
    uint32_t total_size;
    uint32_t _reserved;
} mb2_info_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
} mb2_tag_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
    uint64_t fb_addr;
    uint32_t fb_pitch;
    uint32_t fb_width;
    uint32_t fb_height;
    uint8_t  fb_bpp;
    uint8_t  fb_type;
    uint16_t _reserved;
} mb2_fb_tag_t;

static framebuffer_t g_fb;

framebuffer_t *fb_get(void) { return &g_fb; }

bool fb_init_from_multiboot(uint64_t mb2_phys) {
    const uint8_t *info = (const uint8_t *)PHYS_TO_VIRT(mb2_phys);
    const mb2_info_hdr_t *hdr = (const mb2_info_hdr_t *)info;
    uint32_t offset = 8;
    while (offset < hdr->total_size) {
        const mb2_tag_hdr_t *tag = (const mb2_tag_hdr_t *)(info + offset);
        if (tag->type == MB2_TAG_TYPE_END) break;
        if (tag->type == MB2_TAG_TYPE_FRAMEBUFFER) {
            const mb2_fb_tag_t *fb = (const mb2_fb_tag_t *)tag;
            g_fb.width  = fb->fb_width;
            g_fb.height = fb->fb_height;
            g_fb.pitch  = fb->fb_pitch;
            g_fb.bpp    = fb->fb_bpp;
            g_fb.type   = fb->fb_type;
            uint64_t phys      = fb->fb_addr;
            uint64_t page_base = phys & ~(uint64_t)(FB_PAGE_SIZE - 1u);
            uint64_t page_off  = phys - page_base;
            uint64_t bytes     = (uint64_t)fb->fb_pitch * (uint64_t)fb->fb_height;
            size_t   pages     = (size_t)((page_off + bytes + FB_PAGE_SIZE - 1u) / FB_PAGE_SIZE);
            vmm_map_mmio(page_base, pages);
            g_fb.addr = (uint8_t *)PHYS_TO_VIRT(phys);
            serial_puts(SERIAL_COM1, "[fb] framebuffer tag found\n");
            return true;
        }
        offset += (tag->size + 7u) & ~7u;
    }
    serial_puts(SERIAL_COM1, "[fb] no framebuffer tag\n");
    return false;
}

/* ── Basic primitives ─────────────────────────────────────────── */

void fb_clear(uint32_t color) {
    if (!g_fb.addr || g_fb.bpp != 32) return;
    uint32_t *row = (uint32_t *)g_fb.addr;
    uint32_t stride = g_fb.pitch / 4;
    for (uint32_t y = 0; y < g_fb.height; y++)
        for (uint32_t x = 0; x < g_fb.width; x++)
            row[y * stride + x] = color;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!g_fb.addr || g_fb.bpp != 32) return;
    if (x >= g_fb.width || y >= g_fb.height) return;
    ((uint32_t *)g_fb.addr)[y * (g_fb.pitch / 4) + x] = color;
}

/* Alpha-blend color over the existing pixel (alpha 0=transparent, 255=opaque) */
void fb_blend_pixel(uint32_t x, uint32_t y, uint32_t color, uint8_t alpha) {
    if (!g_fb.addr || g_fb.bpp != 32) return;
    if (x >= g_fb.width || y >= g_fb.height) return;
    uint32_t stride = g_fb.pitch / 4;
    uint32_t *dst   = (uint32_t *)g_fb.addr + y * stride + x;
    uint32_t bg     = *dst;
    uint32_t a      = (uint32_t)alpha;
    uint32_t inv_a  = 255u - a;
    uint32_t r = ((color >> 16 & 0xFF) * a + (bg >> 16 & 0xFF) * inv_a) / 255u;
    uint32_t g = ((color >>  8 & 0xFF) * a + (bg >>  8 & 0xFF) * inv_a) / 255u;
    uint32_t b = ((color       & 0xFF) * a + (bg       & 0xFF) * inv_a) / 255u;
    *dst = 0xFF000000u | (r << 16) | (g << 8) | b;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!g_fb.addr || g_fb.bpp != 32) return;
    if (x >= g_fb.width || y >= g_fb.height) return;
    if (x + w > g_fb.width)  w = g_fb.width  - x;
    if (y + h > g_fb.height) h = g_fb.height - y;
    uint32_t *base   = (uint32_t *)g_fb.addr;
    uint32_t  stride = g_fb.pitch / 4;
    for (uint32_t yy = 0; yy < h; yy++) {
        uint32_t *row = base + (y + yy) * stride + x;
        for (uint32_t xx = 0; xx < w; xx++) row[xx] = color;
    }
}

void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (w == 0 || h == 0) return;
    fb_fill_rect(x, y, w, 1, color);
    if (h > 1) {
        fb_fill_rect(x, y + h - 1, w, 1, color);
        if (h > 2) {
            fb_fill_rect(x, y + 1, 1, h - 2, color);
            if (w > 1) fb_fill_rect(x + w - 1, y + 1, 1, h - 2, color);
        }
    }
}

void fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *src) {
    if (!g_fb.addr || g_fb.bpp != 32 || !src) return;
    if (x >= g_fb.width || y >= g_fb.height) return;
    if (x + w > g_fb.width)  w = g_fb.width  - x;
    if (y + h > g_fb.height) h = g_fb.height - y;
    uint32_t *base   = (uint32_t *)g_fb.addr;
    uint32_t  stride = g_fb.pitch / 4;
    for (uint32_t yy = 0; yy < h; yy++) {
        uint32_t       *dst_row = base  + (y + yy) * stride + x;
        const uint32_t *src_row = src   + yy * w;
        for (uint32_t xx = 0; xx < w; xx++) dst_row[xx] = src_row[xx];
    }
}

/* ── Rounded rectangle ────────────────────────────────────────── */
/*
 * Uses a Bresenham mid-point circle algorithm to rasterise the four
 * corner arcs, then fills horizontal spans between them.
 */

static void _rrect_hspan(uint32_t x0, uint32_t x1, uint32_t y, uint32_t color)
{
    if (y >= g_fb.height) return;
    if (x0 > x1) { uint32_t t = x0; x0 = x1; x1 = t; }
    if (x1 >= g_fb.width) x1 = g_fb.width - 1;
    fb_fill_rect(x0, y, x1 - x0 + 1, 1, color);
}

void fb_fill_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t r, uint32_t color)
{
    if (w == 0 || h == 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r == 0) { fb_fill_rect(x, y, w, h, color); return; }

    /* Fill the three horizontal bands */
    /* Top band: y .. y+r-1  (corners cut) */
    /* Middle band: y+r .. y+h-r-1 (full width) */
    /* Bottom band: y+h-r .. y+h-1 (corners cut) */
    fb_fill_rect(x + r, y, w - 2*r, r, color);              /* top band centre */
    fb_fill_rect(x, y + r, w, h - 2*r, color);              /* middle full-width */
    fb_fill_rect(x + r, y + h - r, w - 2*r, r, color);      /* bottom band centre */

    /* Rasterise corner circles */
    int32_t cx0 = (int32_t)(x + r);
    int32_t cy0 = (int32_t)(y + r);
    int32_t cx1 = (int32_t)(x + w - r - 1);
    int32_t cy1 = (int32_t)(y + h - r - 1);

    int32_t px = (int32_t)r, py = 0, err = 1 - (int32_t)r;
    while (px >= py) {
        /* Fill horizontal spans across each corner arc octant */
        _rrect_hspan((uint32_t)(cx0 - px), (uint32_t)(cx1 + px),
                     (uint32_t)(cy0 - py), color);
        _rrect_hspan((uint32_t)(cx0 - px), (uint32_t)(cx1 + px),
                     (uint32_t)(cy1 + py), color);
        if (py != px) {
            _rrect_hspan((uint32_t)(cx0 - py), (uint32_t)(cx1 + py),
                         (uint32_t)(cy0 - px), color);
            _rrect_hspan((uint32_t)(cx0 - py), (uint32_t)(cx1 + py),
                         (uint32_t)(cy1 + px), color);
        }
        py++;
        if (err < 0) {
            err += 2 * py + 1;
        } else {
            px--;
            err += 2 * (py - px) + 1;
        }
    }
}

void fb_draw_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t r, uint32_t color)
{
    if (w == 0 || h == 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r == 0) { fb_draw_rect(x, y, w, h, color); return; }

    /* Straight edges */
    fb_fill_rect(x + r,     y,         w - 2*r, 1, color); /* top    */
    fb_fill_rect(x + r,     y + h - 1, w - 2*r, 1, color); /* bottom */
    fb_fill_rect(x,         y + r,     1, h - 2*r, color); /* left   */
    fb_fill_rect(x + w - 1, y + r,     1, h - 2*r, color); /* right  */

    /* Four corner arcs */
    int32_t cx0 = (int32_t)(x + r);
    int32_t cy0 = (int32_t)(y + r);
    int32_t cx1 = (int32_t)(x + w - r - 1);
    int32_t cy1 = (int32_t)(y + h - r - 1);

    int32_t px = (int32_t)r, py = 0, err = 1 - (int32_t)r;
    while (px >= py) {
        fb_put_pixel((uint32_t)(cx0 - px), (uint32_t)(cy0 - py), color);
        fb_put_pixel((uint32_t)(cx1 + px), (uint32_t)(cy0 - py), color);
        fb_put_pixel((uint32_t)(cx0 - px), (uint32_t)(cy1 + py), color);
        fb_put_pixel((uint32_t)(cx1 + px), (uint32_t)(cy1 + py), color);
        if (py != px) {
            fb_put_pixel((uint32_t)(cx0 - py), (uint32_t)(cy0 - px), color);
            fb_put_pixel((uint32_t)(cx1 + py), (uint32_t)(cy0 - px), color);
            fb_put_pixel((uint32_t)(cx0 - py), (uint32_t)(cy1 + px), color);
            fb_put_pixel((uint32_t)(cx1 + py), (uint32_t)(cy1 + px), color);
        }
        py++;
        if (err < 0) err += 2 * py + 1;
        else { px--; err += 2 * (py - px) + 1; }
    }
}

/* ── Circle primitives ────────────────────────────────────────── */

void fb_fill_circle(uint32_t cx, uint32_t cy, uint32_t radius, uint32_t color)
{
    int32_t px = (int32_t)radius, py = 0, err = 1 - (int32_t)radius;
    while (px >= py) {
        fb_fill_rect((uint32_t)(cx - px), (uint32_t)(cy - py),
                     (uint32_t)(2 * px + 1), 1, color);
        fb_fill_rect((uint32_t)(cx - px), (uint32_t)(cy + py),
                     (uint32_t)(2 * px + 1), 1, color);
        if (py != px) {
            fb_fill_rect((uint32_t)(cx - py), (uint32_t)(cy - px),
                         (uint32_t)(2 * py + 1), 1, color);
            fb_fill_rect((uint32_t)(cx - py), (uint32_t)(cy + px),
                         (uint32_t)(2 * py + 1), 1, color);
        }
        py++;
        if (err < 0) err += 2 * py + 1;
        else { px--; err += 2 * (py - px) + 1; }
    }
}

void fb_draw_circle(uint32_t cx, uint32_t cy, uint32_t radius, uint32_t color)
{
    int32_t px = (int32_t)radius, py = 0, err = 1 - (int32_t)radius;
    while (px >= py) {
        fb_put_pixel((uint32_t)(cx + px), (uint32_t)(cy - py), color);
        fb_put_pixel((uint32_t)(cx - px), (uint32_t)(cy - py), color);
        fb_put_pixel((uint32_t)(cx + px), (uint32_t)(cy + py), color);
        fb_put_pixel((uint32_t)(cx - px), (uint32_t)(cy + py), color);
        fb_put_pixel((uint32_t)(cx + py), (uint32_t)(cy - px), color);
        fb_put_pixel((uint32_t)(cx - py), (uint32_t)(cy - px), color);
        fb_put_pixel((uint32_t)(cx + py), (uint32_t)(cy + px), color);
        fb_put_pixel((uint32_t)(cx - py), (uint32_t)(cy + px), color);
        py++;
        if (err < 0) err += 2 * py + 1;
        else { px--; err += 2 * (py - px) + 1; }
    }
}

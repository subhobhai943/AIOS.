#include "framebuffer.h"
#include "serial.h"

#ifndef PHYS_TO_VIRT
#  define PHYS_TO_VIRT(p) ((void *)(uintptr_t)(p))
#endif

/* Multiboot2 framebuffer tag type */
#define MB2_TAG_TYPE_END        0
#define MB2_TAG_TYPE_FRAMEBUFFER 8

typedef struct __attribute__((packed)) {
    uint32_t total_size;
    uint32_t _reserved;
} mb2_info_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
} mb2_tag_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t type;          /* 8 */
    uint32_t size;
    uint64_t fb_addr;       /* physical address */
    uint32_t fb_pitch;      /* bytes per scanline */
    uint32_t fb_width;
    uint32_t fb_height;
    uint8_t  fb_bpp;
    uint8_t  fb_type;       /* 0: indexed, 1: RGB, 2: text */
    uint16_t _reserved;
} mb2_fb_tag_t;

static framebuffer_t g_fb;

framebuffer_t *fb_get(void) {
    return &g_fb;
}

bool fb_init_from_multiboot(uint64_t mb2_phys) {
    const uint8_t *info = (const uint8_t *)PHYS_TO_VIRT(mb2_phys);
    const mb2_info_hdr_t *hdr = (const mb2_info_hdr_t *)info;

    uint32_t offset = 8; /* skip fixed header */
    while (offset < hdr->total_size) {
        const mb2_tag_hdr_t *tag = (const mb2_tag_hdr_t *)(info + offset);
        if (tag->type == MB2_TAG_TYPE_END)
            break;

        if (tag->type == MB2_TAG_TYPE_FRAMEBUFFER) {
            const mb2_fb_tag_t *fb = (const mb2_fb_tag_t *)tag;

            g_fb.width  = fb->fb_width;
            g_fb.height = fb->fb_height;
            g_fb.pitch  = fb->fb_pitch;
            g_fb.bpp    = fb->fb_bpp;
            g_fb.type   = fb->fb_type;
            g_fb.addr   = (uint8_t *)PHYS_TO_VIRT(fb->fb_addr);

            serial_puts(SERIAL_COM1, "[fb] framebuffer tag found\n");
            return true;
        }

        uint32_t padded = (tag->size + 7u) & ~7u;
        offset += padded;
    }

    serial_puts(SERIAL_COM1, "[fb] no framebuffer tag in MB2 info; staying in VGA text mode\n");
    return false;
}

void fb_clear(uint32_t color) {
    if (!g_fb.addr || g_fb.bpp != 32)
        return;
    uint32_t *row = (uint32_t *)g_fb.addr;
    uint32_t stride_pixels = g_fb.pitch / 4;
    for (uint32_t y = 0; y < g_fb.height; y++) {
        for (uint32_t x = 0; x < g_fb.width; x++) {
            row[y * stride_pixels + x] = color;
        }
    }
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!g_fb.addr || g_fb.bpp != 32)
        return;
    if (x >= g_fb.width || y >= g_fb.height)
        return;
    uint32_t *row = (uint32_t *)g_fb.addr;
    uint32_t stride_pixels = g_fb.pitch / 4;
    row[y * stride_pixels + x] = color;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!g_fb.addr || g_fb.bpp != 32)
        return;
    if (x >= g_fb.width || y >= g_fb.height)
        return;
    if (x + w > g_fb.width)
        w = g_fb.width - x;
    if (y + h > g_fb.height)
        h = g_fb.height - y;

    uint32_t *base = (uint32_t *)g_fb.addr;
    uint32_t stride_pixels = g_fb.pitch / 4;
    for (uint32_t yy = 0; yy < h; yy++) {
        uint32_t *row = base + (y + yy) * stride_pixels + x;
        for (uint32_t xx = 0; xx < w; xx++) {
            row[xx] = color;
        }
    }
}

void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (w == 0 || h == 0) return;
    fb_fill_rect(x, y, w, 1, color);
    if (h > 1) {
        fb_fill_rect(x, y + h - 1, w, 1, color);
        if (h > 2) {
            fb_fill_rect(x, y + 1, 1, h - 2, color);
            if (w > 1)
                fb_fill_rect(x + w - 1, y + 1, 1, h - 2, color);
        }
    }
}

void fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *src) {
    if (!g_fb.addr || g_fb.bpp != 32 || !src)
        return;
    if (x >= g_fb.width || y >= g_fb.height)
        return;
    if (x + w > g_fb.width)
        w = g_fb.width - x;
    if (y + h > g_fb.height)
        h = g_fb.height - y;

    uint32_t *base = (uint32_t *)g_fb.addr;
    uint32_t stride_pixels = g_fb.pitch / 4;
    for (uint32_t yy = 0; yy < h; yy++) {
        uint32_t *dst_row = base + (y + yy) * stride_pixels + x;
        const uint32_t *src_row = src + yy * w;
        for (uint32_t xx = 0; xx < w; xx++) {
            dst_row[xx] = src_row[xx];
        }
    }
}

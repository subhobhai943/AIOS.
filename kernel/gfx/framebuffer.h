#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include <stdbool.h>

typedef struct framebuffer {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;   /* bytes per scanline */
    uint8_t  bpp;     /* bits per pixel */
    uint8_t  type;    /* Multiboot2 fb_type (0=indexed,1=RGB,2=text) */
    uint8_t *addr;    /* virtual address of framebuffer base */
} framebuffer_t;

/* Returns pointer to the global framebuffer descriptor. */
framebuffer_t *fb_get(void);

/* Initialise framebuffer descriptor from Multiboot2 info block. */
bool fb_init_from_multiboot(uint64_t mb2_phys);

/* ── Basic primitives ─────────────────────────────────────── */
void fb_clear(uint32_t color);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_blend_pixel(uint32_t x, uint32_t y, uint32_t color, uint8_t alpha);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *src);

/* ── Rounded-rectangle primitives (Win7-style soft corners) ── */
void fb_fill_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t r, uint32_t color);
void fb_draw_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t r, uint32_t color);

/* ── Circle primitives (taskbar start orb) ─────────────────── */
void fb_fill_circle(uint32_t cx, uint32_t cy, uint32_t radius, uint32_t color);
void fb_draw_circle(uint32_t cx, uint32_t cy, uint32_t radius, uint32_t color);

#endif /* FRAMEBUFFER_H */

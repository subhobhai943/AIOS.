/* ============================================================
 * AIOS — PS/2 Mouse Driver (IRQ12 / INT 0x2C)
 *
 * The i8042 PS/2 controller multiplexes keyboard (IRQ1) and
 * mouse (IRQ12, "auxiliary device") on the same two I/O ports.
 * We enable the aux port, set stream mode, and decode the
 * standard 3-byte packet.  A visible '*' cursor is drawn on
 * the VGA text buffer at the current mouse position.
 * ============================================================ */

#include "include/mouse.h"
#include "include/vga.h"
#include <stdint.h>

/* ── Screen dimensions (VGA 80×25) ───────────────────────── */
#define SCREEN_W  80
#define SCREEN_H  25

int mouse_x = SCREEN_W / 2;
int mouse_y = SCREEN_H / 2;

/* ── Previous cursor position (for erase) ────────────────── */
static int prev_x = SCREEN_W / 2;
static int prev_y = SCREEN_H / 2;

/* ── I/O helpers ─────────────────────────────────────────── */
static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* ── VGA framebuffer access ──────────────────────────────── */
/* VGA text buffer: 0xB8000, each cell = [char | attr] (16-bit) */
#define VGA_BASE  ((volatile uint16_t *)0xB8000)
#define VGA_ENTRY(ch, fg, bg)  ((uint16_t)((uint8_t)(ch)) | \
                                ((uint16_t)(((bg) << 4) | (fg)) << 8))
#define VGA_FG_WHITE   0x0F
#define VGA_FG_BLACK   0x00
#define VGA_BG_DGREY   0x08

static uint16_t saved_cell = 0;   /* cell under cursor before we drew it */
static uint8_t  cursor_drawn = 0; /* 1 if we have a cursor cell saved     */

/* Draw a single-character cursor at (cx, cy). */
static void cursor_draw(int cx, int cy)
{
    if (cx < 0 || cx >= SCREEN_W || cy < 0 || cy >= SCREEN_H) return;
    volatile uint16_t *cell = VGA_BASE + cy * SCREEN_W + cx;
    saved_cell    = *cell;                          /* save what was there */
    *cell         = VGA_ENTRY('*', VGA_FG_WHITE, VGA_BG_DGREY);
    cursor_drawn  = 1;
}

/* Erase the cursor from its previous position (restore saved cell). */
static void cursor_erase(int cx, int cy)
{
    if (!cursor_drawn) return;
    if (cx < 0 || cx >= SCREEN_W || cy < 0 || cy >= SCREEN_H) return;
    volatile uint16_t *cell = VGA_BASE + cy * SCREEN_W + cx;
    *cell        = saved_cell;
    cursor_drawn = 0;
}

/* ── Wait helpers ────────────────────────────────────────── */
static void mouse_wait_write(void)
{
    uint32_t timeout = 100000;
    while (--timeout && (inb(MOUSE_STATUS_PORT) & MOUSE_STATUS_IBF));
}
static void mouse_wait_read(void)
{
    uint32_t timeout = 100000;
    while (--timeout && !(inb(MOUSE_STATUS_PORT) & MOUSE_STATUS_OBF));
}

/* ── Low-level command helpers ───────────────────────────── */
static void mouse_write(uint8_t byte)
{
    mouse_wait_write();
    outb(MOUSE_CMD_PORT, 0xD4);   /* "next byte → aux device" */
    mouse_wait_write();
    outb(MOUSE_DATA_PORT, byte);
}
static uint8_t mouse_read(void)
{
    mouse_wait_read();
    return inb(MOUSE_DATA_PORT);
}

/* ── Packet state ────────────────────────────────────────── */
static uint8_t  pkt[3];      /* raw 3-byte packet            */
static uint8_t  pkt_idx = 0; /* which byte we are collecting */

/* ── Event ring buffer ───────────────────────────────────── */
static mouse_event_t buf[MOUSE_BUF_SIZE];
static volatile int  head = 0;
static volatile int  tail = 0;

/* ── Init ────────────────────────────────────────────────── */
void mouse_init(void)
{
    /* 1. Enable auxiliary PS/2 device */
    mouse_wait_write();
    outb(MOUSE_CMD_PORT, 0xA8);

    /* 2. Enable auxiliary interrupt in i8042 command byte */
    mouse_wait_write();
    outb(MOUSE_CMD_PORT, 0x20);          /* read command byte */
    uint8_t status = mouse_read();
    status |=  0x02;                     /* bit1 = aux IRQ enable  */
    status &= (uint8_t)~0x20;            /* bit5 = aux clock (0=on) */
    mouse_wait_write();
    outb(MOUSE_CMD_PORT, 0x60);          /* write command byte */
    mouse_wait_write();
    outb(MOUSE_DATA_PORT, status);

    /* 3. Set default parameters */
    mouse_write(0xF6);   /* set defaults */
    mouse_read();        /* ACK */

    /* 4. Enable packet streaming */
    mouse_write(0xF4);   /* enable */
    mouse_read();        /* ACK */

    /* Draw initial cursor at screen centre */
    cursor_draw(mouse_x, mouse_y);

    vga_puts("  [ OK ] Mouse (PS/2, IRQ12) ready — cursor shown at centre\n");
}

/* ── IRQ12 handler ───────────────────────────────────────── */
void mouse_handle_irq(void)
{
    /* Discard any byte that is NOT from the aux port */
    if (!(inb(MOUSE_STATUS_PORT) & MOUSE_STATUS_AUX)) return;

    uint8_t byte = inb(MOUSE_DATA_PORT);

    /* Byte 0 must have bit3 set (always-1 bit) — use as sync */
    if (pkt_idx == 0 && !(byte & 0x08)) return;

    pkt[pkt_idx++] = byte;

    if (pkt_idx < 3) return;   /* wait for full packet */
    pkt_idx = 0;

    /* Decode the packet ----------------------------------------
     * Byte 0: [Y-ovf|X-ovf|Y-sign|X-sign|1|Middle|Right|Left]
     * Byte 1: X movement
     * Byte 2: Y movement  (PS/2 convention: positive = up)
     * --------------------------------------------------------- */
    uint8_t flags = pkt[0];

    /* Discard overflow packets */
    if (flags & 0xC0) return;

    /* Sign-extend the 9-bit deltas */
    int16_t dx = (int16_t)pkt[1] - ((flags & 0x10) ? 256 : 0);
    int16_t dy = (int16_t)pkt[2] - ((flags & 0x20) ? 256 : 0);

    /* Invert Y (screen Y increases downward) */
    dy = -dy;

    /* Save previous position, erase old cursor */
    prev_x = mouse_x;
    prev_y = mouse_y;
    cursor_erase(prev_x, prev_y);

    /* Update absolute cursor position with clamping */
    mouse_x += dx;
    mouse_y += dy;
    if (mouse_x < 0)          mouse_x = 0;
    if (mouse_x >= SCREEN_W)  mouse_x = SCREEN_W - 1;
    if (mouse_y < 0)          mouse_y = 0;
    if (mouse_y >= SCREEN_H)  mouse_y = SCREEN_H - 1;

    /* Draw cursor at new position */
    cursor_draw(mouse_x, mouse_y);

    /* Push event into ring buffer */
    mouse_event_t ev;
    ev.dx      = dx;
    ev.dy      = dy;
    ev.buttons = flags & 0x07;

    int next = (head + 1) % MOUSE_BUF_SIZE;
    if (next != tail) {
        buf[head] = ev;
        head = next;
    }
}

/* ── Read ────────────────────────────────────────────────── */
int mouse_get_event(mouse_event_t *out)
{
    if (head == tail) return 0;
    *out = buf[tail];
    tail = (tail + 1) % MOUSE_BUF_SIZE;
    return 1;
}

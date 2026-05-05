#include "serial.h"
#include <stdint.h>
#include <stddef.h>

/* ─── FIXED PORT I/O (64-bit safe) ─────────────────── */
static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* ─── init ─────────────────────────────────────────── */
void serial_init(uint16_t port, uint32_t baud)
{
    uint16_t divisor = (uint16_t)(115200 / baud);

    outb(port + 1, 0x00);
    outb(port + 3, 0x80);

    outb(port + 0, divisor & 0xFF);
    outb(port + 1, (divisor >> 8) & 0xFF);

    outb(port + 3, 0x03);
    outb(port + 2, 0xC7);
    outb(port + 4, 0x0B);

    /* loopback test */
    outb(port + 4, 0x1E);
    outb(port + 0, 0xAE);

    if (inb(port + 0) != 0xAE)
        return;

    outb(port + 4, 0x0F);

    serial_puts(port, "[AIOS] Serial online @ ");
    serial_putdec(port, baud);
    serial_puts(port, " baud\r\n");
}

/* ─── helpers ──────────────────────────────────────── */
static int serial_tx_ready(uint16_t port)
{
    return inb(port + 5) & 0x20;
}

void serial_putchar(uint16_t port, char c)
{
    while (!serial_tx_ready(port));
    outb(port, (uint8_t)c);
}

void serial_puts(uint16_t port, const char *s)
{
    while (*s) {
        if (*s == '\n') serial_putchar(port, '\r');
        serial_putchar(port, *s++);
    }
}

/* ─── FIXED (no broken indexing garbage) ───────────── */
void serial_putdec(uint16_t port, uint64_t val)
{
    if (val == 0) {
        serial_putchar(port, '0');
        return;
    }

    char buf[20];
    int i = 0;

    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }

    while (i--)
        serial_putchar(port, buf[i]);
}

void serial_puthex(uint16_t port, uint64_t val)
{
    static const char hex[] = "0123456789ABCDEF";

    serial_puts(port, "0x");

    for (int i = 60; i >= 0; i -= 4)
        serial_putchar(port, hex[(val >> i) & 0xF]);
}

char serial_getchar(uint16_t port)
{
    while (!(inb(port + 5) & 0x01));
    return (char)inb(port);
}

int serial_available(uint16_t port)
{
    return inb(port + 5) & 0x01;
}

#include "pit.h"
#include "vga.h"

/* ─── basic types (freestanding) ───────────────────── */
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
/* ─── state ───────────────────────────────────────── */
static volatile uint64_t ticks = 0;
static uint32_t tick_hz = PIT_HZ;

/* ─── PORT I/O (FIXED) ───────────────────────────── */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* ─── init ───────────────────────────────────────── */
void pit_init(uint32_t hz)
{
    tick_hz = hz;
    uint32_t divisor = PIT_BASE_FREQ / hz;

    /* Channel 0, lo/hi byte, Mode 3 */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  [ OK ] PIT timer @ ");
    vga_putdec(hz);   // ← FIXED (no u64)
    vga_puts(" Hz\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ─── IRQ handler ─────────────────────────────────── */
void pit_handler(void)
{
    ticks++;

    /* send EOI to PIC */
    outb(0x20, 0x20);   // ← FIXED (no raw asm)
}

/* ─── timing ─────────────────────────────────────── */
uint64_t pit_get_ticks(void)
{
    return ticks;
}

uint64_t pit_get_ms(void)
{
    return (ticks * 1000) / tick_hz;
}

void pit_sleep_ms(uint64_t ms)
{
    uint64_t target = pit_get_ms() + ms;
    while (pit_get_ms() < target)
        __asm__ volatile ("hlt");
}

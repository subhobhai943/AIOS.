/* ============================================================
 * AIOS — PIT (Programmable Interval Timer) Driver
 * Phase 1.3 — Channel 0 at configurable Hz, IRQ0 tick counter,
 *              pit_sleep_ms() with proper busy-wait (no hlt).
 *
 * FIX (May 2026):
 *   - Use <stdint.h> instead of manual typedefs (no-libc is fine;
 *     stdint.h is explicitly allowed).
 *   - pit_sleep_ms(): replaced `hlt` in sleep loop with a pure
 *     busy-wait.  Using `hlt` stalls the CPU between interrupts
 *     and — if the APIC has not yet unmasked IRQ0 — the tick
 *     counter never advances, producing an infinite hang.
 *   - pit_handler() sends EOI to the LOCAL APIC (via
 *     apic_send_eoi()) rather than to the legacy PIC master,
 *     because Phase 1.2 disables the 8259 and routes IRQ0
 *     through the I/O APIC.
 * ============================================================ */

#include "include/pit.h"
#include "include/vga.h"
#include "apic.h"

#include <stdint.h>

/* ---- state ----------------------------------------------- */
volatile uint64_t g_ticks = 0;
static uint32_t          g_tick_hz = PIT_DEFAULT_HZ;

/* ---- port I/O -------------------------------------------- */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

/* ===========================================================
 * pit_init
 *
 * Programs PIT channel 0 in Mode 2 (rate generator) to fire
 * IRQ0 at the requested frequency.  The APIC must have already
 * routed IRQ0 to IDT vector 0x20 before interrupts are enabled.
 * =========================================================== */
void pit_init(uint32_t hz)
{
    if (hz == 0) hz = PIT_DEFAULT_HZ;
    g_tick_hz = hz;
    g_ticks   = 0;

    uint32_t divisor = PIT_BASE_FREQ / hz;
    if (divisor > 0xFFFFu) divisor = 0xFFFFu;
    if (divisor == 0)      divisor = 1;

    /* Channel 0 | lo+hi byte | Mode 2 (rate generator) | binary */
    outb(PIT_COMMAND,  0x34);
    outb(PIT_CHANNEL0, (uint8_t)( divisor        & 0xFFu));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8u) & 0xFFu));

    vga_puts_color("  [ OK ] PIT channel 0 @ ",
                   VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_putdec((uint64_t)hz);
    vga_puts_color(" Hz (divisor=", VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_putdec((uint64_t)divisor);
    vga_puts_color(")\n", VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ===========================================================
 * pit_handler  —  called from IDT vector 0x20 (IRQ0)
 *
 * MUST be registered in kernel_main via:
 *   idt_register_handler(0x20, pit_irq_handler_wrapper);
 *
 * The wrapper (defined in kernel_main.c) casts the frame
 * pointer and calls pit_handler() then apic_send_eoi().
 * =========================================================== */
void pit_tick(void)
{
    g_ticks++;
}

/* ===========================================================
 * Accessors
 * =========================================================== */
uint64_t pit_get_ticks(void)
{
    return g_ticks;
}

uint64_t pit_get_ms(void)
{
    /* g_ticks * 1000 / g_tick_hz  —  no 64-bit division lib needed;
     * both operands fit in a u64 and the compiler emits a div
     * instruction which is fine in kernel context. */
    return (g_ticks * 1000ULL) / (uint64_t)g_tick_hz;
}

/* ===========================================================
 * pit_sleep_ms
 *
 * Busy-waits until the tick-based millisecond counter advances
 * by at least `ms`.  Interrupts MUST be enabled (STI) before
 * calling this, otherwise the tick counter never advances.
 *
 * We use `pause` (SSE2 spin-loop hint) instead of `hlt`:
 *   - `hlt` halts the CPU until the NEXT interrupt — if the
 *     IRQ fires after the hlt but before we re-check, we miss
 *     the wake-up and stall one extra tick.
 *   - `pause` is a no-op on older CPUs but reduces power
 *     consumption and is safe in a polling loop.
 * =========================================================== */
void pit_sleep_ms(uint64_t ms)
{
    uint64_t deadline = pit_get_ms() + ms;
    while (pit_get_ms() < deadline)
        __asm__ volatile ("pause");
}

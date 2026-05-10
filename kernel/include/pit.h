#ifndef AIOS_PIT_H
#define AIOS_PIT_H

#include <stdint.h>

/* ---- PIT I/O ports --------------------------------------- */
#define PIT_CHANNEL0   0x40u
#define PIT_CHANNEL1   0x41u
#define PIT_CHANNEL2   0x42u
#define PIT_COMMAND    0x43u

/* ---- PIT oscillator frequency (Hz) ---------------------- */
#define PIT_BASE_FREQ  1193182UL

/* ---- Default tick rate ----------------------------------- */
#define PIT_DEFAULT_HZ 1000u

/* ---- Public API ----------------------------------------- */
void     pit_init(uint32_t hz);   /* call before STI          */
void     pit_tick(void);          /* call from IRQ0 handler   */
uint64_t pit_get_ticks(void);     /* raw tick counter         */
uint64_t pit_get_ms(void);        /* milliseconds since init  */
void     pit_sleep_ms(uint64_t ms); /* blocking delay — needs STI */

#endif /* AIOS_PIT_H */

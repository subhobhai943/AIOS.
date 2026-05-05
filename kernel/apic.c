#include "apic.h"
#include "vga.h"

/* ─── basic types (freestanding) ───────────────────── */
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;

/* ─── PORT I/O ─────────────────────────────────────── */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ─── MMIO helpers ─────────────────────────────────── */
static inline uint32_t lapic_read(uint32_t offset)
{
    return *((volatile uint32_t *)(LAPIC_BASE + offset));
}

static inline void lapic_write(uint32_t offset, uint32_t val)
{
    *((volatile uint32_t *)(LAPIC_BASE + offset)) = val;
}

static inline void ioapic_write(uint8_t reg, uint32_t val)
{
    *((volatile uint32_t *)(IOAPIC_BASE + IOAPIC_REGSEL)) = reg;
    *((volatile uint32_t *)(IOAPIC_BASE + IOAPIC_IOWIN))  = val;
}

static inline uint32_t ioapic_read(uint8_t reg)
{
    *((volatile uint32_t *)(IOAPIC_BASE + IOAPIC_REGSEL)) = reg;
    return *((volatile uint32_t *)(IOAPIC_BASE + IOAPIC_IOWIN));
}

/* ─── Disable legacy PIC ───────────────────────────── */
static void pic_disable(void)
{
    outb(0xA1, 0xFF);
    outb(0x21, 0xFF);
}

/* ─── apic_init ────────────────────────────────────── */
void apic_init(void)
{
    /* 1. Disable PIC */
    pic_disable();

    /* 2. Enable Local APIC via MSR (FIXED) */
    uint32_t eax, edx;

    /* read MSR */
    __asm__ volatile(
        "rdmsr"
        : "=a"(eax), "=d"(edx)
        : "c"(0x1B)
    );

    /* set enable bit */
    eax |= 0x800;

    /* write back */
    __asm__ volatile(
        "wrmsr"
        :
        : "a"(eax), "d"(edx), "c"(0x1B)
    );

    /* 3. Enable LAPIC */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | 0xFF);

    /* 4. Accept all interrupts */
    lapic_write(LAPIC_TPR, 0);

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  [ OK ] LAPIC enabled (ID=");
    vga_putdec(apic_get_id());
    vga_puts(")\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ─── EOI ─────────────────────────────────────────── */
void apic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

/* ─── Get APIC ID ─────────────────────────────────── */
uint32_t apic_get_id(void)
{
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

/* ─── Send IPI ────────────────────────────────────── */
void apic_send_ipi(uint8_t dest_apic_id, uint8_t vector)
{
    lapic_write(LAPIC_ICR_HIGH, (uint32_t)dest_apic_id << 24);
    lapic_write(LAPIC_ICR_LOW,  (1 << 14) | vector);
}

/* ─── Timer ───────────────────────────────────────── */
void apic_timer_init(uint32_t ms_interval)
{
    lapic_write(LAPIC_TIMER_DIVIDE, 0x03);
    lapic_write(LAPIC_TIMER_LVT,    LAPIC_TIMER_PERIODIC | 0x20);
    lapic_write(LAPIC_TIMER_INIT,   ms_interval * 100000);
}

/* ─── IOAPIC routing ─────────────────────────────── */
void ioapic_route(uint8_t irq, uint8_t vector, uint8_t dest_apic_id)
{
    uint8_t reg_lo = IOAPIC_REDTBL_BASE + irq * 2;
    uint8_t reg_hi = reg_lo + 1;

    ioapic_write(reg_hi, (uint32_t)dest_apic_id << 24);
    ioapic_write(reg_lo, vector);
}

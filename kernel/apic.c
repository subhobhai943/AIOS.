/* ============================================================
 * AIOS - APIC Driver
 * Phase 1.2 - Legacy PIC remap + disable, Local APIC init,
 *             I/O APIC IRQ0->vector 32 routing, apic_send_eoi()
 *
 * FIX (May 2026) - #PF on first LAPIC register write
 * -----------------------------------------------------------
 * LAPIC MMIO is at 0xFEE00000 and IOAPIC at 0xFEC00000.
 * Both are at the top of the 32-bit address space (~4 GB).
 * The kernel VMM identity-maps only the first 64 MB at boot
 * (0x000000 - 0x3FFFFFF), so any access to these addresses
 * fired a Page Fault (#PF, vector 0xE, error 0x2 = write to
 * non-present page) before a single APIC register was touched.
 *
 * Fix: call vmm_map_mmio() for BOTH regions at the very top
 * of apic_init(), before pic_remap_and_disable() or any
 * lapic_read/lapic_write/ioapic_read_reg/ioapic_write_reg
 * helper is called.
 *
 * PAGE_NOCACHE is mandatory for MMIO - it sets PCD (bit 4)
 * which prevents the CPU from caching device register reads
 * or coalescing device register writes.
 * ============================================================ */

#include "apic.h"
#include "include/vga.h"
#include "include/vmm.h"

#include <stdint.h>

/* ============================================================
 * PORT I/O helpers
 * ============================================================ */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void)
{
    outb(0x80, 0);
}

/* ============================================================
 * MMIO helpers - Local APIC
 * ============================================================ */

static inline uint32_t lapic_read(uint32_t off)
{
    return *((volatile uint32_t *)(uintptr_t)(LAPIC_BASE + off));
}

static inline void lapic_write(uint32_t off, uint32_t val)
{
    *((volatile uint32_t *)(uintptr_t)(LAPIC_BASE + off)) = val;
}

/* ============================================================
 * MMIO helpers - I/O APIC
 * ============================================================ */

static inline void ioapic_write_reg(uint8_t reg, uint32_t val)
{
    *((volatile uint32_t *)(uintptr_t)(IOAPIC_BASE + IOAPIC_REGSEL)) = reg;
    *((volatile uint32_t *)(uintptr_t)(IOAPIC_BASE + IOAPIC_IOWIN))  = val;
}

static inline uint32_t ioapic_read_reg(uint8_t reg)
{
    *((volatile uint32_t *)(uintptr_t)(IOAPIC_BASE + IOAPIC_REGSEL)) = reg;
    return *((volatile uint32_t *)(uintptr_t)(IOAPIC_BASE + IOAPIC_IOWIN));
}

/* ============================================================
 * MSR helpers
 * ============================================================ */

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile (
        "rdmsr"
        : "=a"(lo), "=d"(hi)
        : "c"(msr)
    );
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile (
        "wrmsr"
        ::
        "c"(msr),
        "a"((uint32_t)(val & 0xFFFFFFFF)),
        "d"((uint32_t)(val >> 32))
    );
}

/* ============================================================
 * Step 1 - Remap legacy 8259 PIC then mask all IRQs
 * ============================================================ */

static void pic_remap_and_disable(void)
{
    outb(PIC1_CMD,  0x11);             io_wait();
    outb(PIC1_DATA, PIC_REMAP_MASTER); io_wait();
    outb(PIC1_DATA, 0x04);             io_wait();
    outb(PIC1_DATA, 0x01);             io_wait();

    outb(PIC2_CMD,  0x11);             io_wait();
    outb(PIC2_DATA, PIC_REMAP_SLAVE);  io_wait();
    outb(PIC2_DATA, 0x02);             io_wait();
    outb(PIC2_DATA, 0x01);             io_wait();

    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

/* ============================================================
 * Step 2 - Enable the Local APIC via IA32_APIC_BASE MSR
 * ============================================================ */

static void lapic_enable(void)
{
    uint64_t msr = rdmsr(IA32_APIC_BASE_MSR);
    msr |= IA32_APIC_BASE_EN;
    wrmsr(IA32_APIC_BASE_MSR, msr);

    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SVR_SPURIOUS);
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LINT0_LVT, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LINT1_LVT, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_ERROR_LVT, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_EOI, 0);
}

/* ============================================================
 * Step 3 - Route IRQ0 (PIT timer) via the I/O APIC
 * ============================================================ */

static void ioapic_route_irq0(void)
{
    uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL_BASE + 0 * 2);
    uint8_t reg_hi = (uint8_t)(IOAPIC_REDTBL_BASE + 0 * 2 + 1);

    ioapic_write_reg(reg_hi, (uint32_t)(0u << 24));
    ioapic_write_reg(reg_lo,
        IOAPIC_DELMOD_FIXED |
        IOAPIC_DESTMOD_PHYS |
        0x20u
    );
}

/* ============================================================
 * apic_init - full Phase 1.2 initialisation
 *
 * FIRST ACTION: map the LAPIC and IOAPIC MMIO pages.
 * Nothing that touches a device register may run before this.
 * ============================================================ */

void apic_init(void)
{
    /* ----------------------------------------------------------
     * 0. Map LAPIC and IOAPIC MMIO regions into the page tables.
     *
     * LAPIC:  0xFEE00000 - 0xFEE00FFF  (1 page, 4 KB)
     * IOAPIC: 0xFEC00000 - 0xFEC00FFF  (1 page, 4 KB)
     *
     * vmm_map_mmio() uses PAGE_PRESENT | PAGE_WRITE | PAGE_NOCACHE
     * (PCD bit set) which is the correct attribute for all MMIO.
     *
     * These calls must come before ANY lapic_read/write or
     * ioapic_read_reg/ioapic_write_reg - including CERTAINLY
     * before lapic_enable() or ioapic_route_irq0().
     * The PIC remap below uses port I/O so it is always safe.
     * ---------------------------------------------------------- */
    vmm_map_mmio(LAPIC_BASE,  1);   /* Local APIC  @ 0xFEE00000 */
    vmm_map_mmio(IOAPIC_BASE, 1);   /* I/O APIC    @ 0xFEC00000 */

    vga_puts_color(
        "  [ OK ] APIC MMIO mapped (LAPIC=0xFEE00000, IOAPIC=0xFEC00000)\n",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK
    );

    /* 1. Remap legacy 8259 PIC then mask every IRQ line. */
    pic_remap_and_disable();
    vga_puts_color(
        "  [ OK ] Legacy PIC remapped (0xA0-0xAF) and fully masked\n",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK
    );

    /* 2. Enable Local APIC via MSR. */
    lapic_enable();
    vga_puts_color(
        "  [ OK ] Local APIC enabled via MSR, SVR=0xFF, TPR=0\n",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK
    );
    vga_puts_color("  [ OK ] Local APIC ID = ",
                   VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_putdec(apic_get_id());
    vga_putchar('\n');

    /* 3. Route IRQ0 (PIT timer) -> vector 0x20 on BSP. */
    ioapic_route_irq0();
    vga_puts_color(
        "  [ OK ] I/O APIC: IRQ0 (PIT) -> vector 0x20 on BSP\n",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK
    );

    uint32_t ioapic_ver = ioapic_read_reg(IOAPIC_REG_VER);
    uint8_t  max_redir  = (uint8_t)((ioapic_ver >> 16) & 0xFF);
    vga_puts_color("  [ OK ] I/O APIC version=0x",
                   VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puthex((uint64_t)(ioapic_ver & 0xFF));
    vga_puts_color("  max_redir=", VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_putdec(max_redir + 1u);
    vga_puts_color(" entries\n", VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ============================================================
 * apic_send_eoi
 * ============================================================ */

void apic_send_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

/* ============================================================
 * apic_get_id
 * ============================================================ */

uint32_t apic_get_id(void)
{
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

/* ============================================================
 * apic_send_ipi
 * ============================================================ */

void apic_send_ipi(uint8_t dest_apic_id, uint8_t vector)
{
    lapic_write(LAPIC_ICR_HIGH, (uint32_t)dest_apic_id << 24);
    lapic_write(LAPIC_ICR_LOW,  (1u << 14) | (uint32_t)vector);
}

/* ============================================================
 * apic_timer_init
 * ============================================================ */

void apic_timer_init(uint32_t initial_count)
{
    lapic_write(LAPIC_TIMER_DIVIDE, 0x03);
    lapic_write(LAPIC_TIMER_LVT,    LAPIC_TIMER_PERIODIC | 0x20);
    lapic_write(LAPIC_TIMER_INIT,   initial_count);
}

/* ============================================================
 * ioapic_route - generic (any IRQ, any vector, any destination)
 * ============================================================ */

void ioapic_route(uint8_t irq, uint8_t vector, uint8_t dest_apic_id)
{
    uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL_BASE + (uint8_t)(irq * 2u));
    uint8_t reg_hi = (uint8_t)(reg_lo + 1u);

    ioapic_write_reg(reg_hi, (uint32_t)dest_apic_id << 24);
    ioapic_write_reg(reg_lo,
        IOAPIC_DELMOD_FIXED |
        IOAPIC_DESTMOD_PHYS |
        (uint32_t)vector
    );
}

/* ============================================================
 * ioapic_mask / ioapic_unmask
 * ============================================================ */

void ioapic_mask(uint8_t irq)
{
    uint8_t  reg_lo = (uint8_t)(IOAPIC_REDTBL_BASE + (uint8_t)(irq * 2u));
    uint32_t val    = ioapic_read_reg(reg_lo);
    ioapic_write_reg(reg_lo, val | IOAPIC_MASKED);
}

void ioapic_unmask(uint8_t irq)
{
    uint8_t  reg_lo = (uint8_t)(IOAPIC_REDTBL_BASE + (uint8_t)(irq * 2u));
    uint32_t val    = ioapic_read_reg(reg_lo);
    ioapic_write_reg(reg_lo, val & ~IOAPIC_MASKED);
}

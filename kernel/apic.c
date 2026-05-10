/* ============================================================
 * AIOS - APIC Driver
 * Phase 1.2 - Legacy PIC remap + disable, Local APIC init,
 *             apic_send_eoi()
 *
 * FIX 1 (May 2026) - #PF on first LAPIC register write
 * -----------------------------------------------------------
 * LAPIC MMIO is at 0xFEE00000 and IOAPIC at 0xFEC00000.
 * The kernel VMM identity-maps only the first 64 MB at boot,
 * so both regions must be explicitly mapped via vmm_map_mmio()
 * before any register access.
 *
 * FIX 2 (May 2026) - IRQ0 hang after STI
 * -----------------------------------------------------------
 * The original apic_init() called ioapic_route_irq0() to route
 * IRQ0 -> vector 0x20 BEFORE kernel_main had registered the IDT
 * handler at 0x20.  The very first PIT tick (which fires as soon
 * as STI is executed) was delivered to the default unhandled stub
 * which does NOT call apic_send_eoi().  The Local APIC then marks
 * vector 0x20 as In-Service (ISR bit set) permanently, blocking
 * ALL further interrupts -- the tick counter never increments and
 * pit_sleep_ms() hangs forever.
 *
 * Fix: remove ioapic_route_irq0() from apic_init() entirely.
 * IRQ0 is now routed in kernel_main via ioapic_route(0, 0x20, 0)
 * AFTER idt_register_handler(0x20, pit_irq_handler) is called,
 * exactly the same pattern used for keyboard (IRQ1) and mouse
 * (IRQ12).
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
 * apic_init
 *
 * Responsibilities:
 *   1. Map LAPIC + IOAPIC MMIO pages (must be first)
 *   2. Remap + disable legacy 8259 PIC
 *   3. Enable Local APIC via MSR
 *
 * NOT done here: routing individual IRQs via the I/O APIC.
 * Each IRQ must be routed in kernel_main AFTER its IDT handler
 * is registered with idt_register_handler(), so that the first
 * delivered interrupt always finds a valid handler that calls
 * apic_send_eoi().  Routing before handler registration leaves
 * a window where the APIC can deliver to an unhandled stub,
 * causing a permanent ISR stall (no EOI -> all IRQs blocked).
 *
 * Use ioapic_route(irq, vector, dest) from kernel_main.
 * ============================================================ */

void apic_init(void)
{
    /* 0. Map LAPIC and IOAPIC MMIO pages. */
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

    /* Print I/O APIC version info (read-only, safe any time after MMIO map). */
    uint32_t ioapic_ver = ioapic_read_reg(IOAPIC_REG_VER);
    uint8_t  max_redir  = (uint8_t)((ioapic_ver >> 16) & 0xFF);
    vga_puts_color("  [ OK ] I/O APIC version=0x",
                   VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puthex((uint64_t)(ioapic_ver & 0xFF));
    vga_puts_color("  max_redir=", VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_putdec(max_redir + 1u);
    vga_puts_color(" entries\n", VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    /* NOTE: No ioapic_route() calls here.
     * IRQ routing is done in kernel_main after IDT handlers are
     * registered.  See kernel_main.c for the correct order. */
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
 * ioapic_route - route one IRQ to an IDT vector on a given CPU.
 *
 * Call this from kernel_main AFTER registering the IDT handler
 * for `vector` via idt_register_handler().  Never call before
 * the handler is in place.
 * ============================================================ */

void ioapic_route(uint8_t irq, uint8_t vector, uint8_t dest_apic_id)
{
    uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL_BASE + (uint8_t)(irq * 2u));
    uint8_t reg_hi = (uint8_t)(reg_lo + 1u);

    /* Write hi word first (destination), then lo word (vector + flags).
     * Writing lo last atomically enables delivery. */
    ioapic_write_reg(reg_hi, (uint32_t)dest_apic_id << 24);
    ioapic_write_reg(reg_lo,
        IOAPIC_DELMOD_FIXED |
        IOAPIC_DESTMOD_PHYS |
        (uint32_t)vector
        /* IOAPIC_MASKED bit NOT set -> entry is live immediately */
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

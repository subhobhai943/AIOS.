/* ============================================================
 * AIOS — APIC Driver
 * Phase 1.2 — Legacy PIC remap + disable, Local APIC init,
 *              I/O APIC IRQ0→vector 32 routing, apic_send_eoi()
 * ============================================================
 *
 * Assumptions (QEMU default configuration)
 * -----------------------------------------
 *  • Local APIC MMIO at 0xFEE00000 (IA32_APIC_BASE MSR default)
 *  • I/O APIC MMIO at 0xFEC00000 (QEMU default)
 *  • BSP LAPIC ID = 0
 *  • These addresses are inside the 64 MB identity-mapped window
 *    set up by vmm_init(), so no extra mapping is needed.
 *
 * When ACPI parsing lands (Phase 5.3) the real addresses will be
 * read from the MADT table; for now constants suffice for QEMU.
 * ============================================================ */

#include "apic.h"
#include "include/vga.h"

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

/* small I/O delay — needed after some 8259 writes */
static inline void io_wait(void)
{
    outb(0x80, 0);   /* write to a normally-unused port */
}

/* ============================================================
 * MMIO helpers — Local APIC
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
 * MMIO helpers — I/O APIC indirect register access
 * The IOAPIC has a 2-register window: write index to REGSEL,
 * then read/write data through IOWIN.
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
 * Step 1 — Remap legacy 8259 PIC, then mask all IRQs
 *
 * Why remap before masking?
 * When the system boots the 8259 PIC has IRQ0-7 mapped to CPU
 * exception vectors 8-15 (reserved by Intel).  Even if we mask
 * all IRQs immediately, a spurious pulse on IRQ7 or IRQ15 can
 * fire before the mask takes effect and land on an exception
 * handler, causing a spurious triple-fault.
 *
 * We remap the PIC to 0xA0-0xAF (well above APIC vectors 0x20-
 * 0xFF and Intel reserved exceptions 0-31), then mask every
 * IRQ line.  That way any stray PIC pulse is harmless.
 * ============================================================ */

static void pic_remap_and_disable(void)
{
    /* --- Initialise PIC1 --------------------------------------- */
    outb(PIC1_CMD,  0x11);          /* ICW1: init, ICW4 required   */
    io_wait();
    outb(PIC1_DATA, PIC_REMAP_MASTER); /* ICW2: base vector 0xA0  */
    io_wait();
    outb(PIC1_DATA, 0x04);          /* ICW3: slave on IRQ2         */
    io_wait();
    outb(PIC1_DATA, 0x01);          /* ICW4: 8086 mode             */
    io_wait();

    /* --- Initialise PIC2 --------------------------------------- */
    outb(PIC2_CMD,  0x11);
    io_wait();
    outb(PIC2_DATA, PIC_REMAP_SLAVE);  /* ICW2: base vector 0xA8  */
    io_wait();
    outb(PIC2_DATA, 0x02);          /* ICW3: cascade identity      */
    io_wait();
    outb(PIC2_DATA, 0x01);          /* ICW4: 8086 mode             */
    io_wait();

    /* --- Mask all IRQ lines on both PICs ----------------------- */
    outb(PIC1_DATA, 0xFF);          /* mask IRQ0–7                */
    outb(PIC2_DATA, 0xFF);          /* mask IRQ8–15               */
}

/* ============================================================
 * Step 2 — Enable the Local APIC via IA32_APIC_BASE MSR
 *
 * The MSR also contains the APIC base address in bits 12-51.
 * We keep the address field as-is (0xFEE00000 by default) and
 * just set bit 11 (global enable).
 * ============================================================ */

static void lapic_enable(void)
{
    uint64_t msr = rdmsr(IA32_APIC_BASE_MSR);
    msr |= IA32_APIC_BASE_EN;           /* set global enable bit    */
    wrmsr(IA32_APIC_BASE_MSR, msr);

    /* Software-enable the APIC: write SVR with enable bit set and
     * the spurious vector = 0xFF.  The spurious IRQ will fire on
     * vector 0xFF if the LAPIC raises an interrupt that nobody
     * claimed; IDT vector 255 is wired to a do-nothing stub. */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SVR_SPURIOUS);

    /* Set Task Priority Register to 0 — accept all priorities. */
    lapic_write(LAPIC_TPR, 0);

    /* Mask the LINT0/LINT1 lines (they carry 8259 signals on some
     * older boards; we’re using the I/O APIC so silence them). */
    lapic_write(LAPIC_LINT0_LVT, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LINT1_LVT, LAPIC_LVT_MASKED);

    /* Mask the LAPIC error LVT for now (un-masked in Phase 4). */
    lapic_write(LAPIC_ERROR_LVT, LAPIC_LVT_MASKED);

    /* Send an initial EOI to clear any stale interrupt. */
    lapic_write(LAPIC_EOI, 0);
}

/* ============================================================
 * Step 3 — Route IRQ0 (PIT timer) via the I/O APIC
 *
 * I/O APIC redirection table entry (64-bit, stored as two 32-bit
 * registers at IOREDTBL[2*irq] and IOREDTBL[2*irq+1]):
 *
 *  Bits 63:56  Destination field (physical LAPIC ID when DESTMOD=0)
 *  Bits 55:17  Reserved
 *  Bit  16     Interrupt mask (1=masked)
 *  Bit  15     Trigger mode   (0=edge, 1=level)
 *  Bit  14     Remote IRR     (read-only)
 *  Bit  13     Interrupt pin polarity (0=active-high)
 *  Bit  12     Delivery status (read-only)
 *  Bit  11     Destination mode (0=physical, 1=logical)
 *  Bits 10:8   Delivery mode (000=fixed)
 *  Bits  7:0   Interrupt vector (0x10–0xFE)
 * ============================================================ */

static void ioapic_route_irq0(void)
{
    /* Register indices for IRQ0: base 0x10, two regs per entry */
    uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL_BASE + 0 * 2);
    uint8_t reg_hi = (uint8_t)(IOAPIC_REDTBL_BASE + 0 * 2 + 1);

    /* High 32 bits: destination = LAPIC ID 0 (BSP) in bits 31:24 */
    ioapic_write_reg(reg_hi, (uint32_t)(0u << 24));

    /* Low 32 bits:
     *   vector    = 0x20 (32)  → IDT entry 32 = IRQ0 handler
     *   delmod    = 000  (fixed delivery)
     *   destmod   = 0    (physical)
     *   polarity  = 0    (active high)
     *   trigger   = 0    (edge)
     *   mask      = 0    (unmasked — IRQ0 is live)
     */
    ioapic_write_reg(reg_lo,
        IOAPIC_DELMOD_FIXED   |     /* bits 10:8 = 000 */
        IOAPIC_DESTMOD_PHYS   |     /* bit  11   = 0   */
        0x20u                       /* vector 32       */
    );
}

/* ============================================================
 * apic_init — full Phase 1.2 initialisation sequence
 * ============================================================ */

void apic_init(void)
{
    /* 1. Remap legacy 8259 PIC then mask every IRQ line.
     *    After this, the 8259 is neutered — it can still generate
     *    a spurious IRQ7/15 but those now land on 0xA7/0xAF which
     *    have do-nothing ISR stubs via idt_init(). */
    pic_remap_and_disable();
    vga_puts_color(
        "  [ OK ] Legacy PIC remapped (0xA0-0xAF) and fully masked\n",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK
    );

    /* 2. Enable Local APIC via IA32_APIC_BASE MSR.  Sets SVR,
     *    TPR, masks LINT0/LINT1/Error LVTs. */
    lapic_enable();
    vga_puts_color(
        "  [ OK ] Local APIC enabled via MSR, SVR=0xFF, TPR=0\n",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK
    );
    vga_puts_color("  [ OK ] Local APIC ID = ", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_putdec(apic_get_id());
    vga_putchar('\n');

    /* 3. Route IRQ0 (PIT / system timer) via I/O APIC:
     *    IRQ0 → vector 0x20 (32) → IDT[32] on BSP (LAPIC ID 0). */
    ioapic_route_irq0();
    vga_puts_color(
        "  [ OK ] I/O APIC: IRQ0 (PIT) → vector 0x20 on BSP\n",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK
    );

    /* Log the I/O APIC version for debugging */
    uint32_t ioapic_ver = ioapic_read_reg(IOAPIC_REG_VER);
    uint8_t  max_redir  = (uint8_t)((ioapic_ver >> 16) & 0xFF);
    vga_puts_color("  [ OK ] I/O APIC version=0x", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puthex((uint64_t)(ioapic_ver & 0xFF));
    vga_puts_color("  max_redir=", VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_putdec(max_redir + 1u);
    vga_puts_color(" entries\n", VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ============================================================
 * apic_send_eoi
 *
 * Must be called at the END of every interrupt handler that was
 * delivered through the Local APIC (i.e., all vectors >= 0x20
 * when the APIC is active).  Writing any value to LAPIC_EOI
 * clears the highest-priority in-service bit.
 * ============================================================ */

void apic_send_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

/* ============================================================
 * apic_get_id — return current core’s Local APIC ID
 * ============================================================ */

uint32_t apic_get_id(void)
{
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

/* ============================================================
 * apic_send_ipi — send an IPI to another core (SMP, Phase 4+)
 *
 * Write the destination LAPIC ID to ICR_HIGH first (bits 31:24),
 * then write the vector + delivery mode to ICR_LOW.  Writing
 * ICR_LOW triggers the IPI.
 * ============================================================ */

void apic_send_ipi(uint8_t dest_apic_id, uint8_t vector)
{
    lapic_write(LAPIC_ICR_HIGH, (uint32_t)dest_apic_id << 24);
    /* Delivery mode = 000 (fixed), level = 1, assert */
    lapic_write(LAPIC_ICR_LOW, (1u << 14) | (uint32_t)vector);
}

/* ============================================================
 * apic_timer_init — start LAPIC timer in periodic mode
 *
 * The LAPIC timer runs at the bus/core crystal frequency
 * (varies per CPU, must be calibrated with PIT in Phase 1.3).
 * For now this function takes a raw initial_count; Phase 1.3
 * will compute the correct value for a 1 ms tick.
 *
 * Timer LVT:
 *   bits 17:16 = 01  (periodic mode)
 *   bits  7:0  = 0x20 (vector 32 = same IRQ0 handler as PIT)
 * Divide config: divide by 16 (0x03)
 * ============================================================ */

void apic_timer_init(uint32_t initial_count)
{
    lapic_write(LAPIC_TIMER_DIVIDE, 0x03);                       /* /16 */
    lapic_write(LAPIC_TIMER_LVT,    LAPIC_TIMER_PERIODIC | 0x20); /* vec 32, periodic */
    lapic_write(LAPIC_TIMER_INIT,   initial_count);
}

/* ============================================================
 * ioapic_route — generic public version (any IRQ, any vector)
 * ============================================================ */

void ioapic_route(uint8_t irq, uint8_t vector, uint8_t dest_apic_id)
{
    uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL_BASE + (uint8_t)(irq * 2u));
    uint8_t reg_hi = (uint8_t)(reg_lo + 1u);

    /* Write high word first (destination), then low (vector + ctrl) */
    ioapic_write_reg(reg_hi, (uint32_t)dest_apic_id << 24);
    ioapic_write_reg(reg_lo,
        IOAPIC_DELMOD_FIXED |
        IOAPIC_DESTMOD_PHYS |
        (uint32_t)vector
    );
}

/* ============================================================
 * ioapic_mask / ioapic_unmask — toggle an IRQ line
 * ============================================================ */

void ioapic_mask(uint8_t irq)
{
    uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL_BASE + (uint8_t)(irq * 2u));
    uint32_t val = ioapic_read_reg(reg_lo);
    ioapic_write_reg(reg_lo, val | IOAPIC_MASKED);
}

void ioapic_unmask(uint8_t irq)
{
    uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL_BASE + (uint8_t)(irq * 2u));
    uint32_t val = ioapic_read_reg(reg_lo);
    ioapic_write_reg(reg_lo, val & ~IOAPIC_MASKED);
}

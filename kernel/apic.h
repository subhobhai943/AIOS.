#ifndef APIC_H
#define APIC_H

#include <stdint.h>

/* ============================================================
 * AIOS — APIC (Advanced Programmable Interrupt Controller)
 *
 * Strategy used in AIOS:
 *   • Legacy 8259 PIC is remapped to vectors 0xA0-0xAF then fully
 *     masked (disabled) — its IRQ lines are dead after apic_init().
 *   • Local APIC is enabled via IA32_APIC_BASE MSR (bit 11).
 *     Spurious vector = 0xFF.
 *   • I/O APIC is mapped at IOAPIC_BASE.  IRQ0 (PIT timer) is
 *     routed to vector 0x20 (32) on the BSP (Local APIC ID 0).
 *   • All IRQ handlers must call apic_send_eoi() at the end.
 * ============================================================ */

/* ---------------------------------------------------------------
 * Local APIC MMIO base
 * Default physical address; may be relocated by BIOS — for now
 * we use the default.  Phase 4 will read the real address from the
 * IA32_APIC_BASE MSR.
 * --------------------------------------------------------------- */
#define LAPIC_BASE          0xFEE00000ULL

/* Local APIC register offsets (byte offsets from LAPIC_BASE) */
#define LAPIC_ID            0x020   /* APIC ID register              */
#define LAPIC_VERSION       0x030   /* APIC version                  */
#define LAPIC_TPR           0x080   /* Task Priority Register        */
#define LAPIC_EOI           0x0B0   /* End-Of-Interrupt (write 0)    */
#define LAPIC_LDR           0x0D0   /* Logical Destination Register  */
#define LAPIC_SVR           0x0F0   /* Spurious Vector Register      */
#define LAPIC_ISR0          0x100   /* In-Service Register (8x32b)   */
#define LAPIC_ICR_LOW       0x300   /* Interrupt Command Register lo */
#define LAPIC_ICR_HIGH      0x310   /* Interrupt Command Register hi */
#define LAPIC_TIMER_LVT     0x320   /* Timer LVT entry               */
#define LAPIC_LINT0_LVT     0x350   /* Local INT0 LVT entry          */
#define LAPIC_LINT1_LVT     0x360   /* Local INT1 LVT entry          */
#define LAPIC_ERROR_LVT     0x370   /* Error LVT entry               */
#define LAPIC_TIMER_INIT    0x380   /* Timer Initial Count           */
#define LAPIC_TIMER_CURRENT 0x390   /* Timer Current Count           */
#define LAPIC_TIMER_DIVIDE  0x3E0   /* Timer Divide Configuration    */

/* SVR bits */
#define LAPIC_SVR_ENABLE    (1u << 8)   /* software-enable APIC      */
#define LAPIC_SVR_SPURIOUS  0xFFu       /* spurious IRQ vector       */

/* LVT mask bit (set = masked / disabled) */
#define LAPIC_LVT_MASKED    (1u << 16)

/* Timer LVT mode */
#define LAPIC_TIMER_ONESHOT  0u
#define LAPIC_TIMER_PERIODIC (1u << 17)

/* ---------------------------------------------------------------
 * I/O APIC MMIO base (default physical address)
 * --------------------------------------------------------------- */
#define IOAPIC_BASE         0xFEC00000ULL

/* I/O APIC indirect register access */
#define IOAPIC_REGSEL       0x00u   /* register select (write byte) */
#define IOAPIC_IOWIN        0x10u   /* data window (read/write)     */

/* I/O APIC named registers */
#define IOAPIC_REG_ID       0x00u
#define IOAPIC_REG_VER      0x01u
#define IOAPIC_REDTBL_BASE  0x10u   /* redirection table base (2 regs per entry) */

/* Redirection entry flags */
#define IOAPIC_DELMOD_FIXED 0u          /* fixed delivery */
#define IOAPIC_DESTMOD_PHYS 0u          /* physical APIC ID in dest field */
#define IOAPIC_MASKED       (1u << 16)  /* mask = route disabled */

/* ---------------------------------------------------------------
 * IA32_APIC_BASE MSR
 * --------------------------------------------------------------- */
#define IA32_APIC_BASE_MSR  0x1Bu
#define IA32_APIC_BASE_EN   (1u << 11)  /* global APIC enable bit */

/* ---------------------------------------------------------------
 * PIC (8259) constants — used during remap + disable sequence
 * --------------------------------------------------------------- */
#define PIC1_CMD   0x20u
#define PIC1_DATA  0x21u
#define PIC2_CMD   0xA0u
#define PIC2_DATA  0xA1u
#define PIC_EOI    0x20u

/* Remap offsets: push PIC IRQs to 0xA0–0xAF (above any CPU exception
 * or APIC vector we use, so stray PIC pulses don't misfire). */
#define PIC_REMAP_MASTER    0xA0u
#define PIC_REMAP_SLAVE     0xA8u

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */

/* apic_init — disable legacy PIC, enable Local APIC, route IRQ0
 *             (PIT) via I/O APIC to vector 0x20 on BSP. */
void apic_init(void);

/* apic_send_eoi — signal End-Of-Interrupt to the Local APIC.
 *   MUST be called at the end of every LAPIC-delivered IRQ handler. */
void apic_send_eoi(void);

/* apic_get_id — returns the Local APIC ID of the current CPU. */
uint32_t apic_get_id(void);

/* apic_send_ipi — send an Inter-Processor Interrupt.
 *   dest_apic_id : physical LAPIC ID of the target core
 *   vector       : interrupt vector to deliver (0x20–0xFE) */
void apic_send_ipi(uint8_t dest_apic_id, uint8_t vector);

/* apic_timer_init — start the LAPIC timer in periodic mode.
 *   initial_count : raw count value loaded into TIMER_INIT.
 *   Call after calibrating with the PIT (Phase 1.3). */
void apic_timer_init(uint32_t initial_count);

/* ioapic_route — program one I/O APIC redirection table entry.
 *   irq          : ISA IRQ number (0–15)
 *   vector       : IDT vector to deliver (0x20–0xFE)
 *   dest_apic_id : physical LAPIC ID of the destination core */
void ioapic_route(uint8_t irq, uint8_t vector, uint8_t dest_apic_id);

/* ioapic_mask   — mask (silence) an IRQ line */
void ioapic_mask(uint8_t irq);

/* ioapic_unmask — unmask (enable) an IRQ line */
void ioapic_unmask(uint8_t irq);

#endif /* APIC_H */

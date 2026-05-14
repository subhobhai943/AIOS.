/*
 * kernel/acpi.c  —  AIOS Phase 5.3
 *
 * ACPI driver: RSDP scan → RSDT/XSDT walk → FADT parse
 * Implements acpi_shutdown() and acpi_reboot().
 *
 * Rules:
 *   - No libc.  Only <stdint.h>, <stddef.h>, <stdbool.h>.
 *   - No kmalloc needed: all state in static globals.
 *   - Compiler: x86_64-elf-gcc -ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "acpi.h"
#include "serial.h"    /* klog() */
#include "panic.h"     /* kernel_panic() */

/* -----------------------------------------------------------------------
 * Low-level I/O helpers (no ioport.h assumed yet)
 * ----------------------------------------------------------------------- */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
/* memory-barrier after MMIO write */
static inline void io_wait(void) {
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

/* -----------------------------------------------------------------------
 * kmemcmp / kmemcpy stubs (acpi.c is self-contained)
 * ----------------------------------------------------------------------- */
static int acpi_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
static bool     g_acpi_ready  = false;
static uint16_t g_pm1a_cnt    = 0;   /* I/O port */
static uint16_t g_pm1b_cnt    = 0;   /* 0 if not present */
static uint16_t g_slp_typa    = 0;   /* SLP_TYPa value for S5 */
static uint16_t g_slp_typb    = 0;   /* SLP_TYPb value for S5 */
static bool     g_have_slpb   = false;

/* Reset register from FADT */
static uint8_t  g_reset_value = 0;
static uint8_t  g_reset_space = 0;   /* 0=memory, 1=I/O, 2=PCI-config */
static uint64_t g_reset_addr  = 0;
static bool     g_have_reset  = false;

/* -----------------------------------------------------------------------
 * Checksum validation
 * ----------------------------------------------------------------------- */
static bool acpi_checksum(const void *ptr, size_t len) {
    const uint8_t *p = (const uint8_t *)ptr;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += p[i];
    return sum == 0;
}

/* -----------------------------------------------------------------------
 * RSDP scan
 *
 * Search order (ACPI spec):
 *   1. EBDA first kilobyte  (address at *0x40E << 4)
 *   2. BIOS read-only region 0x000E0000–0x000FFFFF
 *
 * Signature is "RSD PTR " (8 bytes) on 16-byte boundary.
 *
 * In our identity-mapped kernel the physical addresses are directly
 * accessible as pointers (identity-map covers first 64 MB).
 * ----------------------------------------------------------------------- */
static acpi_rsdp_v1_t *acpi_find_rsdp(void) {
    /* 1. EBDA */
    uint16_t ebda_seg = *(volatile uint16_t *)0x40E;
    uintptr_t ebda    = (uintptr_t)ebda_seg << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000) {
        for (uintptr_t addr = ebda; addr < ebda + 1024; addr += 16) {
            if (acpi_memcmp((void *)addr, "RSD PTR ", 8) == 0) {
                acpi_rsdp_v1_t *r = (acpi_rsdp_v1_t *)addr;
                size_t len = (r->revision >= 2)
                    ? sizeof(acpi_rsdp_v2_t)
                    : sizeof(acpi_rsdp_v1_t);
                if (acpi_checksum(r, len)) return r;
            }
        }
    }
    /* 2. BIOS ROM area */
    for (uintptr_t addr = 0x000E0000; addr < 0x00100000; addr += 16) {
        if (acpi_memcmp((void *)addr, "RSD PTR ", 8) == 0) {
            acpi_rsdp_v1_t *r = (acpi_rsdp_v1_t *)addr;
            size_t len = (r->revision >= 2)
                ? sizeof(acpi_rsdp_v2_t)
                : sizeof(acpi_rsdp_v1_t);
            if (acpi_checksum(r, len)) return r;
        }
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * SDT table lookup  (via RSDT = 32-bit ptrs, or XSDT = 64-bit ptrs)
 * ----------------------------------------------------------------------- */
static acpi_sdt_header_t *acpi_find_table_rsdt(acpi_sdt_header_t *rsdt,
                                                const char sig[4]) {
    uint32_t n = (rsdt->length - sizeof(acpi_sdt_header_t)) / 4;
    uint32_t *entries = (uint32_t *)((uintptr_t)rsdt + sizeof(acpi_sdt_header_t));
    for (uint32_t i = 0; i < n; i++) {
        acpi_sdt_header_t *h = (acpi_sdt_header_t *)(uintptr_t)entries[i];
        if (acpi_memcmp(h->signature, sig, 4) == 0) {
            if (acpi_checksum(h, h->length)) return h;
        }
    }
    return NULL;
}

static acpi_sdt_header_t *acpi_find_table_xsdt(acpi_sdt_header_t *xsdt,
                                                const char sig[4]) {
    uint32_t n = (xsdt->length - sizeof(acpi_sdt_header_t)) / 8;
    uint64_t *entries = (uint64_t *)((uintptr_t)xsdt + sizeof(acpi_sdt_header_t));
    for (uint32_t i = 0; i < n; i++) {
        acpi_sdt_header_t *h = (acpi_sdt_header_t *)(uintptr_t)entries[i];
        if (acpi_memcmp(h->signature, sig, 4) == 0) {
            if (acpi_checksum(h, h->length)) return h;
        }
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * _S5_ AML object parser
 *
 * In the DSDT, the S5 sleep state is encoded as AML:
 *   Name (_S5_, Package () { SLP_TYPa, SLP_TYPb, ... })
 *
 * Byte pattern to search for (simplified, matches most BIOSes):
 *   0x08  (AML NameOp)  '_' 'S' '5' '_'  0x12 (PackageOp)
 *   then skip package length, then two bytes: SLP_TYPa, SLP_TYPb
 *
 * We scan the raw AML byte stream in the DSDT body.
 * ----------------------------------------------------------------------- */
static bool acpi_parse_s5(acpi_sdt_header_t *dsdt,
                           uint16_t *slp_typa, uint16_t *slp_typb) {
    uint8_t *aml   = (uint8_t *)dsdt + sizeof(acpi_sdt_header_t);
    uint32_t aml_len = dsdt->length - sizeof(acpi_sdt_header_t);

    for (uint32_t i = 0; i + 7 < aml_len; i++) {
        /* Look for NameOp + '_S5_' */
        if (aml[i]   == 0x08 &&
            aml[i+1] == '_' && aml[i+2] == 'S' &&
            aml[i+3] == '5' && aml[i+4] == '_') {
            /* Next byte should be PackageOp (0x12) */
            uint32_t j = i + 5;
            if (j >= aml_len) break;
            if (aml[j] != 0x12) continue;
            j++;
            if (j >= aml_len) break;
            /* Decode AML package length (1-4 bytes, variable encoding) */
            uint8_t pkg_lead = aml[j++];
            uint8_t extra_bytes = (pkg_lead >> 6) & 0x3;
            j += extra_bytes;  /* skip extra length bytes */
            if (j >= aml_len) break;
            /* Next byte: NumElements (should be >= 2) */
            j++;  /* skip NumElements */
            if (j + 1 >= aml_len) break;
            /* Read SLP_TYPa — may be BytePrefix (0x0A) + value, or just value */
            uint16_t typa = 0, typb = 0;
            if (aml[j] == 0x0A) { j++; }  /* BytePrefix optional */
            if (j >= aml_len) break;
            typa = aml[j++];
            if (aml[j] == 0x0A) { j++; }  /* BytePrefix optional */
            if (j >= aml_len) break;
            typb = aml[j];
            *slp_typa = typa;
            *slp_typb = typb;
            return true;
        }
    }
    return false;
}

/* -----------------------------------------------------------------------
 * acpi_init  — public entry point
 * ----------------------------------------------------------------------- */
bool acpi_init(void) {
    acpi_rsdp_v1_t *rsdp = acpi_find_rsdp();
    if (!rsdp) {
        klog("[ACPI] RSDP not found\n");
        return false;
    }
    klog("[ACPI] RSDP found, revision=");
    klog(rsdp->revision >= 2 ? "2.0+\n" : "1.0\n");

    acpi_sdt_header_t *fadt_hdr = NULL;
    if (rsdp->revision >= 2) {
        acpi_rsdp_v2_t *rsdp2 = (acpi_rsdp_v2_t *)rsdp;
        acpi_sdt_header_t *xsdt =
            (acpi_sdt_header_t *)(uintptr_t)rsdp2->xsdt_address;
        if (acpi_checksum(xsdt, xsdt->length)) {
            klog("[ACPI] Using XSDT\n");
            fadt_hdr = acpi_find_table_xsdt(xsdt, "FACP");
        }
    }
    if (!fadt_hdr) {
        acpi_sdt_header_t *rsdt =
            (acpi_sdt_header_t *)(uintptr_t)rsdp->rsdt_address;
        if (!acpi_checksum(rsdt, rsdt->length)) {
            klog("[ACPI] RSDT checksum bad\n");
            return false;
        }
        klog("[ACPI] Using RSDT\n");
        fadt_hdr = acpi_find_table_rsdt(rsdt, "FACP");
    }
    if (!fadt_hdr) {
        klog("[ACPI] FADT not found\n");
        return false;
    }
    klog("[ACPI] FADT found\n");

    acpi_fadt_t *fadt = (acpi_fadt_t *)fadt_hdr;

    /* PM1a control block — prefer 64-bit extended address if ACPI 2+ */
    if (fadt_hdr->revision >= 3 && fadt->x_pm1a_cnt_blk.address != 0 &&
        fadt->x_pm1a_cnt_blk.address_space == 1 /* I/O */) {
        g_pm1a_cnt = (uint16_t)fadt->x_pm1a_cnt_blk.address;
    } else {
        g_pm1a_cnt = (uint16_t)fadt->pm1a_cnt_blk;
    }

    if (fadt_hdr->revision >= 3 && fadt->x_pm1b_cnt_blk.address != 0 &&
        fadt->x_pm1b_cnt_blk.address_space == 1) {
        g_pm1b_cnt    = (uint16_t)fadt->x_pm1b_cnt_blk.address;
        g_have_slpb   = true;
    } else if (fadt->pm1b_cnt_blk != 0) {
        g_pm1b_cnt  = (uint16_t)fadt->pm1b_cnt_blk;
        g_have_slpb = true;
    }

    /* Reset register */
    if (fadt->flags & (1u << 10) /* RESET_REG_SUP */) {
        g_reset_addr  = fadt->reset_reg.address;
        g_reset_space = fadt->reset_reg.address_space;
        g_reset_value = fadt->reset_value;
        g_have_reset  = true;
        klog("[ACPI] Reset register available\n");
    }

    /* Locate DSDT and parse _S5_ */
    uintptr_t dsdt_phys = 0;
    if (fadt_hdr->revision >= 3 && fadt->x_dsdt != 0)
        dsdt_phys = (uintptr_t)fadt->x_dsdt;
    else
        dsdt_phys = (uintptr_t)fadt->dsdt;

    acpi_sdt_header_t *dsdt = (acpi_sdt_header_t *)dsdt_phys;
    if (!acpi_checksum(dsdt, dsdt->length)) {
        klog("[ACPI] DSDT checksum bad — shutdown may use fallback\n");
    } else {
        uint16_t typa = 0, typb = 0;
        if (acpi_parse_s5(dsdt, &typa, &typb)) {
            g_slp_typa  = (uint16_t)((typa & 0x7) << 10);
            g_slp_typb  = (uint16_t)((typb & 0x7) << 10);
            klog("[ACPI] _S5_ found\n");
        } else {
            klog("[ACPI] _S5_ not found in DSDT, using default SLP_TYP\n");
            /* QEMU default: S5 = type 0; value 0x2000 works for QEMU i440fx */
            g_slp_typa = 0x2000;
            g_slp_typb = 0x2000;
        }
    }

    g_acpi_ready = true;
    klog("[ACPI] init OK\n");
    return true;
}

/* -----------------------------------------------------------------------
 * acpi_shutdown
 *
 * Write SLP_TYPa | SLP_EN to PM1a_CNT (and PM1b_CNT if present).
 * SLP_EN = bit 13 (0x2000).
 *
 * Falls back to the QEMU ACPI I/O trap ports if FADT path didn't work:
 *   0x604 (QEMU fw_cfg ACPI shutdown)  — QEMU ≥ 1.1
 *   0xB004 (Bochs/old QEMU poweroff)
 * ----------------------------------------------------------------------- */
void acpi_shutdown(void) {
    if (g_acpi_ready && g_pm1a_cnt != 0) {
        uint16_t val = g_slp_typa | 0x2000; /* SLP_EN */
        outw(g_pm1a_cnt, val);
        io_wait();
        if (g_have_slpb && g_pm1b_cnt != 0) {
            outw(g_pm1b_cnt, g_slp_typb | 0x2000);
            io_wait();
        }
    }
    /* Fallback: QEMU / Bochs magic ports */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    /* ISA bus power-off (very old BIOSes) */
    outw(0x600, 0x34);
    /* Should not reach here */
    __asm__ volatile ("cli; hlt");
    for (;;) __asm__ volatile ("hlt");
}

/* -----------------------------------------------------------------------
 * acpi_reboot
 *
 * 1. FADT RESET_REG (ACPI 2.0+)  — write reset_value to reset_reg
 * 2. PS/2 keyboard controller pulse  (port 0x64, command 0xFE)
 * 3. Triple-fault via null IDT       (last resort)
 * ----------------------------------------------------------------------- */
void acpi_reboot(void) {
    if (g_have_reset && g_reset_addr != 0) {
        if (g_reset_space == 0 /* system memory */) {
            volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)g_reset_addr;
            *p = g_reset_value;
            io_wait();
        } else if (g_reset_space == 1 /* I/O */) {
            outb((uint16_t)g_reset_addr, g_reset_value);
            io_wait();
        }
    }
    /* PS/2 controller pulse line — works on almost every x86 machine */
    /* Wait for PS/2 input buffer to be empty */
    uint32_t timeout = 0x100000;
    while (timeout-- && (inb(0x64) & 0x02)) {
        io_wait();
    }
    outb(0x64, 0xFE);  /* Pulse output line (includes RESET) */
    io_wait();
    io_wait();

    /* Triple-fault: load a null IDT and trigger an interrupt */
    __asm__ volatile (
        "cli\n"
        "lidt %0\n"
        "int $0x03\n"
        : : "m"((uint64_t){0})
    );
    for (;;) __asm__ volatile ("hlt");
}

/* -----------------------------------------------------------------------
 * acpi_dump_info  — debug helper, prints to serial
 * ----------------------------------------------------------------------- */
void acpi_dump_info(void) {
    if (!g_acpi_ready) {
        klog("[ACPI] not initialised\n");
        return;
    }
    klog("[ACPI] pm1a_cnt=0x"); klog_hex(g_pm1a_cnt);
    klog(" pm1b_cnt=0x");        klog_hex(g_pm1b_cnt);
    klog(" slp_typa=0x");        klog_hex(g_slp_typa);
    klog(" slp_typb=0x");        klog_hex(g_slp_typb);
    klog("\n");
    if (g_have_reset) {
        klog("[ACPI] reset_space="); klog_dec(g_reset_space);
        klog(" reset_addr=0x");     klog_hex((uint32_t)g_reset_addr);
        klog(" reset_value=0x");    klog_hex(g_reset_value);
        klog("\n");
    }
}

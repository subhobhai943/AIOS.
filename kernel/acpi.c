/* ===========================================================
   kernel/acpi.c — Phase 5.3 ACPI Power Management
   Implements: acpi_init, acpi_shutdown, acpi_reboot
   No libc.  Only <stdint.h>, <stddef.h>, <stdbool.h>.
   =========================================================== */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "acpi.h"
#include "serial.h"   /* klog() */
#include "panic.h"    /* kernel_panic() */

/* ---- low-level I/O helpers ---- */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline void outb(uint8_t port_val, uint16_t port) {
    __asm__ volatile("outb %0, %1" : : "a"(port_val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ---- write to a GAS (Generic Address Structure) ----
   We handle System Memory (0) and System I/O (1) only. */
static void gas_write8(const acpi_gas_t *gas, uint8_t val) {
    if (gas->address_space == 1) {          /* I/O space */
        outb(val, (uint16_t)gas->address);
    } else {                                /* Memory space */
        volatile uint8_t *ptr = (volatile uint8_t *)(uintptr_t)gas->address;
        *ptr = val;
    }
}

/* ===========================================================
   Internal state
   =========================================================== */
static acpi_rsdp_t  *g_rsdp  = NULL;
static acpi_fadt_t  *g_fadt  = NULL;
static uint16_t      g_slp_typ_s5  = 0;   /* SLP_TYPa bits for S5 (power off) */
static bool          g_s5_valid    = false;
static bool          g_reset_valid = false;

/* ===========================================================
   Checksum helpers
   =========================================================== */
static bool sdt_checksum_ok(const acpi_sdt_header_t *hdr) {
    const uint8_t *p = (const uint8_t *)hdr;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < hdr->length; i++) sum += p[i];
    return (sum == 0);
}

static bool rsdp_checksum_ok(const acpi_rsdp_t *r) {
    /* ACPI 1.0 checksum covers first 20 bytes */
    const uint8_t *p = (const uint8_t *)r;
    uint8_t sum = 0;
    for (int i = 0; i < 20; i++) sum += p[i];
    return (sum == 0);
}

/* ===========================================================
   RSDP search
   Scan EBDA (first 1 KB) then 0xE0000–0xFFFFF (BIOS area).
   The RSDP signature is "RSD PTR " (8 bytes) on a 16-byte boundary.
   =========================================================== */
static acpi_rsdp_t *find_rsdp(void) {
    /* helper: scan [start, start+len) */
#define SCAN_REGION(start, len) do {                                        \
    uintptr_t _s = (start);                                                 \
    uintptr_t _e = _s + (len);                                              \
    for (uintptr_t _a = _s; _a < _e; _a += 16) {                           \
        const char *_c = (const char *)_a;                                  \
        if (_c[0]=='R'&&_c[1]=='S'&&_c[2]=='D'&&_c[3]==' '&&              \
            _c[4]=='P'&&_c[5]=='T'&&_c[6]=='R'&&_c[7]==' ') {              \
            acpi_rsdp_t *_r = (acpi_rsdp_t *)_a;                           \
            if (rsdp_checksum_ok(_r)) return _r;                            \
        }                                                                   \
    }                                                                       \
} while(0)

    /* 1. EBDA: address at 0x040E (physical), scan first 1 KiB */
    uint16_t ebda_seg = *(volatile uint16_t *)0x040E;
    uintptr_t ebda = (uintptr_t)ebda_seg << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000)
        SCAN_REGION(ebda, 0x400);

    /* 2. BIOS ROM area 0xE0000–0xFFFFF */
    SCAN_REGION(0xE0000, 0x20000);

#undef SCAN_REGION
    return NULL;
}

/* ===========================================================
   SDT table lookup by 4-char signature
   Supports both RSDT (32-bit pointers) and XSDT (64-bit).
   =========================================================== */
static acpi_sdt_header_t *find_table(const char sig[4]) {
    if (!g_rsdp) return NULL;

    /* Prefer XSDT on ACPI 2.0+ */
    if (g_rsdp->revision >= 2 && g_rsdp->xsdt_address != 0) {
        acpi_xsdt_t *xsdt = (acpi_xsdt_t *)(uintptr_t)g_rsdp->xsdt_address;
        if (xsdt->hdr.signature[0]!='X'||xsdt->hdr.signature[1]!='S'||
            xsdt->hdr.signature[2]!='D'||xsdt->hdr.signature[3]!='T') goto try_rsdt;
        uint32_t n = (xsdt->hdr.length - sizeof(acpi_sdt_header_t)) / 8;
        for (uint32_t i = 0; i < n; i++) {
            acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)(uintptr_t)xsdt->tables[i];
            if (!hdr) continue;
            if (hdr->signature[0]==sig[0] && hdr->signature[1]==sig[1] &&
                hdr->signature[2]==sig[2] && hdr->signature[3]==sig[3]) {
                if (sdt_checksum_ok(hdr)) return hdr;
            }
        }
        return NULL;
    }

try_rsdt:
    {
        acpi_rsdt_t *rsdt = (acpi_rsdt_t *)(uintptr_t)g_rsdp->rsdt_address;
        if (!rsdt) return NULL;
        if (rsdt->hdr.signature[0]!='R'||rsdt->hdr.signature[1]!='S'||
            rsdt->hdr.signature[2]!='D'||rsdt->hdr.signature[3]!='T') return NULL;
        uint32_t n = (rsdt->hdr.length - sizeof(acpi_sdt_header_t)) / 4;
        for (uint32_t i = 0; i < n; i++) {
            acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)(uintptr_t)rsdt->tables[i];
            if (!hdr) continue;
            if (hdr->signature[0]==sig[0] && hdr->signature[1]==sig[1] &&
                hdr->signature[2]==sig[2] && hdr->signature[3]==sig[3]) {
                if (sdt_checksum_ok(hdr)) return hdr;
            }
        }
    }
    return NULL;
}

/* ===========================================================
   AML: parse S5 package from DSDT to get SLP_TYPa

   The S5 object in AML looks like:
       Name(_S5_,Package(){0x0,0x0,0x0,0x0})
   Encoded as:
       08 5F 53 35 5F 12 04 04 0A <SLP_TYPa> 0A <SLP_TYPb> ...
     ^ DefName  ^^ Package   ^^NumElements   ^ BytePrefix

   We search for the byte sequence: '_','S','5','_'
   then walk forward to the package element.
   =========================================================== */
static bool parse_s5_from_dsdt(void) {
    if (!g_fadt) return false;

    uintptr_t dsdt_addr = (uintptr_t)g_fadt->dsdt;
    if (!dsdt_addr) return false;

    acpi_sdt_header_t *dsdt = (acpi_sdt_header_t *)dsdt_addr;
    if (dsdt->signature[0]!='D'||dsdt->signature[1]!='S'||
        dsdt->signature[2]!='D'||dsdt->signature[3]!='T') return false;

    const uint8_t *aml   = (const uint8_t *)(dsdt_addr + sizeof(acpi_sdt_header_t));
    uint32_t       alen  = dsdt->length - sizeof(acpi_sdt_header_t);

    for (uint32_t i = 0; i + 7 < alen; i++) {
        /* look for DefName opcode (0x08) followed by "_S5_" */
        if (aml[i]   == 0x08 &&
            aml[i+1] == '_'  &&
            aml[i+2] == 'S'  &&
            aml[i+3] == '5'  &&
            aml[i+4] == '_') {
            /* next byte should be Package opcode 0x12 */
            uint32_t j = i + 5;
            /* skip optional PkgLength varlen encoding */
            if (aml[j] != 0x12) continue;  /* not a package — skip */
            j++;                           /* skip Package opcode */
            /* PkgLength: 1–4 bytes depending on top-2 bits */
            uint8_t pkglen_lead = aml[j];
            uint8_t extra_bytes = (pkglen_lead >> 6) & 0x03;
            j += 1 + extra_bytes;          /* skip PkgLength */
            /* NumElements byte */
            if (j >= alen) continue;
            j++;                           /* skip NumElements */
            /* First element = SLP_TYPa, may be prefixed with 0x0A (BytePrefix) */
            if (j >= alen) continue;
            uint8_t slp_typa;
            if (aml[j] == 0x0A) {         /* BytePrefix */
                j++;
                if (j >= alen) continue;
                slp_typa = aml[j];
            } else if (aml[j] <= 0x0F) {  /* ZeroOp / OneOp / small int */
                slp_typa = aml[j];
            } else {
                continue;
            }
            /* SLP_TYP is stored in bits [12:10] of PM1a_CNT */
            g_slp_typ_s5  = (uint16_t)(slp_typa & 0x07) << 10;
            g_s5_valid    = true;
            klog("[ACPI] S5 SLP_TYPa found\n");
            return true;
        }
    }
    klog("[ACPI] WARN: _S5_ not found in DSDT; will use QEMU fallback\n");
    return false;
}

/* ===========================================================
   acpi_init
   =========================================================== */
bool acpi_init(void) {
    klog("[ACPI] Searching for RSDP...\n");
    g_rsdp = find_rsdp();
    if (!g_rsdp) {
        klog("[ACPI] WARN: RSDP not found\n");
        return false;
    }
    klog("[ACPI] RSDP found; revision=");
    klog(g_rsdp->revision >= 2 ? "2+\n" : "1\n");

    acpi_sdt_header_t *fadt_hdr = find_table("FACP");  /* FADT signature is "FACP" */
    if (!fadt_hdr) {
        klog("[ACPI] WARN: FADT not found\n");
        return false;
    }
    g_fadt = (acpi_fadt_t *)fadt_hdr;
    klog("[ACPI] FADT found\n");

    /* Validate reset register */
    if (g_fadt->hdr.revision >= 2 &&
        (g_fadt->flags & (1u << 10)) &&   /* bit 10 = RESET_REG_SUP */
        g_fadt->reset_reg.address != 0) {
        g_reset_valid = true;
        klog("[ACPI] FADT reset register present\n");
    }

    /* Parse S5 from DSDT */
    parse_s5_from_dsdt();

    klog("[ACPI] Init complete\n");
    return true;
}

/* ===========================================================
   acpi_shutdown
   Write SLP_TYPa | SLP_EN (bit 13) to PM1a_CNT_BLK.
   Fallback: QEMU-specific I/O ports.
   =========================================================== */
void acpi_shutdown(void) {
    __asm__ volatile("cli");

    if (g_fadt && g_fadt->pm1a_cnt_blk && g_s5_valid) {
        uint16_t pm1a = (uint16_t)g_fadt->pm1a_cnt_blk;
        uint16_t val  = g_slp_typ_s5 | (1u << 13); /* SLP_EN */
        klog("[ACPI] Shutting down via FADT PM1a_CNT\n");
        outw(pm1a, val);
        /* also PM1b if present */
        if (g_fadt->pm1b_cnt_blk) {
            outw((uint16_t)g_fadt->pm1b_cnt_blk, val);
        }
    }

    /* Fallback: QEMU ACPI ports (tried in boot order) */
    klog("[ACPI] Fallback: QEMU ACPI shutdown ports\n");
    outw(0x604, 0x2000);   /* QEMU ≤ 2.x */
    outw(0xB004, 0x2000);  /* Bochs / older QEMU */
    outw(0x600,  0x34);    /* QEMU 4.x+ */

    /* If we get here the hardware ignored us */
    klog("[ACPI] Shutdown failed — halting\n");
    for (;;) __asm__ volatile("cli; hlt");
}

/* ===========================================================
   acpi_reboot
   Use FADT reset register if available.
   Fallback: PS/2 controller (0x64/0xFE) then triple-fault.
   =========================================================== */
void acpi_reboot(void) {
    __asm__ volatile("cli");

    /* 1. FADT reset register (ACPI 2.0+) */
    if (g_reset_valid) {
        klog("[ACPI] Rebooting via FADT reset register\n");
        gas_write8(&g_fadt->reset_reg, g_fadt->reset_value);
    }

    /* 2. PS/2 controller reset pulse */
    klog("[ACPI] Fallback: PS/2 controller reset\n");
    /* wait for PS/2 input buffer empty */
    for (int i = 0; i < 0x10000; i++) {
        if (!(inb(0x64) & 0x02)) break;
    }
    outb(0xFE, 0x64);   /* pulse reset line */

    /* 3. Triple-fault: load a zero-length IDT and trigger an interrupt */
    klog("[ACPI] Fallback: triple fault\n");
    __asm__ volatile(
        "lidt %[idt]\n"
        "int $0x03\n"
        : : [idt] "m" ((struct { uint16_t lim; uint64_t base; }){ 0, 0 })
    );

    for (;;) __asm__ volatile("cli; hlt");
}

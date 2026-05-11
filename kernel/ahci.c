/* ============================================================
 * AIOS — AHCI / SATA Driver  (Phase 3.2)
 *
 * Implements:
 *   ahci_init()                     — find HBA, init ports
 *   ahci_read_sectors(port,lba,n,buf)
 *   ahci_write_sectors(port,lba,n,buf)
 *
 * Constraints:
 *   - freestanding C, no libc
 *   - only <stdint.h>, <stddef.h>, <stdbool.h>
 *   - BAR5 physical address is <64 MB → already identity-mapped
 *     by VMM; no extra mapping needed at this stage
 *   - Static aligned buffers for command lists and FIS regions
 *     (heap is available but 32-byte alignment from kmalloc_aligned
 *      is correct; we use static bufs to keep this self-contained)
 * ============================================================ */

#include "ahci.h"
#include "pci.h"
#include "include/vga.h"
#include "serial.h"
#include "include/pit.h"
#include "include/panic.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ─── Compile-time helpers ───────────────────────────────── */
#define MMIO32(addr)        (*((volatile uint32_t *)(uintptr_t)(addr)))
#define PORT_BASE(hba, p)   ((hba) + 0x100 + (uint32_t)(p) * 0x80)

/* Port register offsets (relative to PORT_BASE) */
#define PxCLB    0x00   /* Command List Base (low 32) */
#define PxCLBU   0x04   /* Command List Base (high 32) */
#define PxFB     0x08   /* FIS Base (low 32) */
#define PxFBU    0x0C   /* FIS Base (high 32) */
#define PxIS     0x10   /* Interrupt Status */
#define PxIE     0x14   /* Interrupt Enable */
#define PxCMD    0x18   /* Command & Status */
#define PxTFD    0x20   /* Task File Data */
#define PxSIG    0x24   /* Signature */
#define PxSSTS   0x28   /* SATA Status */
#define PxSCTL   0x2C   /* SATA Control */
#define PxSERR   0x30   /* SATA Error */
#define PxSACT   0x34   /* SATA Active */
#define PxCI     0x38   /* Command Issue */

/* PxCMD bits */
#define CMD_ST   (1u <<  0)   /* Start */
#define CMD_FRE  (1u <<  4)   /* FIS Receive Enable */
#define CMD_FR   (1u << 14)   /* FIS Receive Running */
#define CMD_CR   (1u << 15)   /* Command List Running */

/* TFD bits */
#define TFD_BSY  (1u <<  7)
#define TFD_DRQ  (1u <<  3)

/* ATA commands */
#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35

/* FIS type */
#define FIS_TYPE_REG_H2D  0x27

/* ─── Static aligned buffers ─────────────────────────────── */
/*
 * Command list: 32 entries x 32 bytes = 1024 bytes per port.
 * FIS receive area: 256 bytes per port.
 * Command table: 128-byte header + PRDT.  One per port (we use slot 0).
 *
 * We support up to AHCI_MAX_PORTS ports.
 */
#define AHCI_MAX_PORTS  4

/* 32-byte command header */
typedef struct __attribute__((packed)) {
    uint8_t  cfl_atapi_write_prefetch; /* bits[4:0]=CFL, [6]=ATAPI, [7]=Write */
    uint8_t  reset_bist_clear_busy;    /* [2]=Reset, [3]=BIST, [4]=C */
    uint16_t prdtl;                    /* PRDT length in entries */
    uint32_t prdbc;                    /* PRD byte count (written by HBA) */
    uint32_t ctba;                     /* Command Table Base Address (low) */
    uint32_t ctbau;                    /* Command Table Base Address (high) */
    uint32_t reserved[4];
} ahci_cmd_header_t;

/* PRDT entry */
typedef struct __attribute__((packed)) {
    uint32_t dba;    /* Data Base Address (low) */
    uint32_t dbau;   /* Data Base Address (high) */
    uint32_t reserved;
    uint32_t dbc_i;  /* Byte count (bits[21:0], 0-based) | interrupt[31] */
} ahci_prdt_entry_t;

/* H2D Register FIS (20 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  fis_type;      /* 0x27 */
    uint8_t  pm_c;          /* bit7=C (update command register) */
    uint8_t  command;       /* ATA command */
    uint8_t  featurel;
    uint8_t  lba0, lba1, lba2;
    uint8_t  device;        /* bit6=LBA mode */
    uint8_t  lba3, lba4, lba5;
    uint8_t  featureh;
    uint16_t count;         /* sector count */
    uint8_t  icc;
    uint8_t  control;
    uint8_t  aux[4];
} h2d_fis_t;

/* Command table: FIS area (64 bytes) + ATAPI command (16) + reserved (48) +
 * one PRDT entry.  We only ever use one PRDT entry per command. */
typedef struct __attribute__((packed)) {
    uint8_t         cfis[64];    /* Command FIS */
    uint8_t         acmd[16];    /* ATAPI command (unused) */
    uint8_t         rsv[48];     /* Reserved */
    ahci_prdt_entry_t prdt[1];   /* One PRDT entry */
} ahci_cmd_table_t;

/* Aligned static buffers for all ports */
static ahci_cmd_header_t cmd_list[AHCI_MAX_PORTS][32]
    __attribute__((aligned(1024)));
static uint8_t           fis_buf[AHCI_MAX_PORTS][256]
    __attribute__((aligned(256)));
static ahci_cmd_table_t  cmd_table[AHCI_MAX_PORTS]
    __attribute__((aligned(128)));

/* ─── Driver state ───────────────────────────────────────── */
static uint32_t hba_base  = 0;   /* MMIO base of HBA (BAR5 physical) */
static uint32_t port_mask = 0;   /* bitmask of active (drive present) ports */

/* ─── Small helpers ──────────────────────────────────────── */
static void memzero(void *dst, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = 0;
}

/* Spin-wait up to `ms` milliseconds for condition; return false on timeout. */
static bool wait_clear(volatile uint32_t *reg, uint32_t mask, uint32_t ms)
{
    uint64_t deadline = pit_get_ticks() + (uint64_t)ms;
    while (*reg & mask) {
        if (pit_get_ticks() >= deadline) return false;
    }
    return true;
}
static bool wait_set(volatile uint32_t *reg, uint32_t mask, uint32_t ms)
{
    uint64_t deadline = pit_get_ticks() + (uint64_t)ms;
    while (!(*reg & mask)) {
        if (pit_get_ticks() >= deadline) return false;
    }
    return true;
}

/* ─── Port start / stop helpers ───────────────────────────── */
static void port_stop(int p)
{
    uint32_t pb = PORT_BASE(hba_base, p);
    /* Clear ST and FRE, wait for CR and FR to clear */
    MMIO32(pb + PxCMD) &= ~CMD_ST;
    wait_clear((volatile uint32_t *)(uintptr_t)(pb + PxCMD), CMD_CR, 500);
    MMIO32(pb + PxCMD) &= ~CMD_FRE;
    wait_clear((volatile uint32_t *)(uintptr_t)(pb + PxCMD), CMD_FR, 500);
}

static void port_start(int p)
{
    uint32_t pb = PORT_BASE(hba_base, p);
    /* Wait for CR to clear before setting ST */
    wait_clear((volatile uint32_t *)(uintptr_t)(pb + PxCMD), CMD_CR, 500);
    MMIO32(pb + PxCMD) |= CMD_FRE;
    MMIO32(pb + PxCMD) |= CMD_ST;
}

/* ─── ahci_init ───────────────────────────────────────────── */
int ahci_init(void)
{
    /* 1. Find AHCI HBA via PCI (Class 0x01, Subclass 0x06) */
    pci_device_t *hba_dev = pci_find_device(0x01, 0x06);
    if (!hba_dev) {
        vga_puts_color("[AHCI] No AHCI controller found on PCI bus.\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
        klog("[AHCI] No AHCI controller found.\r\n");
        return -1;
    }

    /* BAR5 holds the AHCI HBA Memory Registers (ABAR) */
    hba_base = hba_dev->bar[5] & ~0xFu;   /* mask off lower 4 bits */
    if (!hba_base) {
        vga_puts_color("[AHCI] BAR5 is zero — HBA MMIO not mapped.\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
        return -1;
    }

    klog("[AHCI] HBA at BAR5=0x");
    klog_hex(hba_base);
    klog("\r\n");

    /* 2. Enable AHCI mode in GHC */
    MMIO32(hba_base + AHCI_HBA_GHC) |= AHCI_GHC_AHCI_ENABLE;

    /* 3. HBA reset (optional but recommended for clean state) */
    MMIO32(hba_base + AHCI_HBA_GHC) |= AHCI_GHC_RESET;
    if (!wait_clear((volatile uint32_t *)(uintptr_t)(hba_base + AHCI_HBA_GHC),
                    AHCI_GHC_RESET, 1000)) {
        vga_puts_color("[AHCI] HBA reset timeout.\n",
                       VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return -1;
    }
    /* Re-enable AHCI after reset */
    MMIO32(hba_base + AHCI_HBA_GHC) |= AHCI_GHC_AHCI_ENABLE;

    /* 4. Identify implemented ports */
    uint32_t pi = MMIO32(hba_base + AHCI_HBA_PI);
    port_mask = 0;

    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;

        uint32_t pb   = PORT_BASE(hba_base, p);
        uint32_t ssts = MMIO32(pb + PxSSTS);
        uint8_t  det  = ssts & 0x0F;
        uint8_t  ipm  = (ssts >> 8) & 0x0F;

        /* DET==3 means device present and PHY communication established */
        /* IPM==1 means interface is active */
        if (det != 3 || ipm != 1) continue;

        /* Skip ATAPI (signature 0xEB140101) — only plain SATA for now */
        if (MMIO32(pb + PxSIG) == 0xEB140101) continue;

        port_mask |= (1u << p);

        /* 5. Stop port before reconfiguring */
        port_stop(p);

        /* 6. Point CLB and FB to our static buffers */
        uint32_t clb_phys = (uint32_t)(uintptr_t)cmd_list[p];
        uint32_t fb_phys  = (uint32_t)(uintptr_t)fis_buf[p];

        MMIO32(pb + PxCLBU) = 0;
        MMIO32(pb + PxCLB)  = clb_phys;
        MMIO32(pb + PxFBU)  = 0;
        MMIO32(pb + PxFB)   = fb_phys;

        /* Zero command headers */
        memzero(cmd_list[p], sizeof(cmd_list[p]));
        memzero(fis_buf[p],  sizeof(fis_buf[p]));

        /* 7. Clear error and interrupt status */
        MMIO32(pb + PxSERR) = (uint32_t)-1;
        MMIO32(pb + PxIS)   = (uint32_t)-1;

        /* 8. Start port */
        port_start(p);

        vga_puts_color("  [ OK ] AHCI port ",
                       VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_putdec(p);
        vga_puts_color(" SATA device detected\n",
                       VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        klog("[AHCI] port ");
        klog_dec(p);
        klog(" SATA ready\r\n");
    }

    if (!port_mask) {
        vga_puts_color("[AHCI] No SATA drives found (all ports empty).\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
        return 0;   /* not fatal — system still boots */
    }

    return 0;
}

/* ─── ahci_port_available ─────────────────────────────────── */
bool ahci_port_available(int port)
{
    return (port >= 0 && port < 32) && !!(port_mask & (1u << port));
}

/* ─── Internal: issue one DMA command ──────────────────────── */
static int ahci_do_rw(int port, uint64_t lba, uint32_t count,
                      void *buf, bool write)
{
    if (!ahci_port_available(port)) return -1;
    if (!count || count > 128)      return -1;   /* max 128 sectors at once */

    uint32_t pb = PORT_BASE(hba_base, port);

    /* ── Wait for port to be idle ──────────────────────────── */
    if (!wait_clear((volatile uint32_t *)(uintptr_t)(pb + PxTFD),
                    TFD_BSY | TFD_DRQ, 500)) {
        klog("[AHCI] port busy timeout before command\r\n");
        return -1;
    }

    /* ── Build Command Table in slot 0 ─────────────────────── */
    ahci_cmd_table_t *ct = &cmd_table[port];
    memzero(ct, sizeof(*ct));

    /* H2D Register FIS */
    h2d_fis_t *fis = (h2d_fis_t *)ct->cfis;
    fis->fis_type  = FIS_TYPE_REG_H2D;
    fis->pm_c      = 0x80;                        /* C=1: update command reg */
    fis->command   = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    fis->device    = 1 << 6;                       /* LBA mode */
    fis->lba0      = (uint8_t)(lba);
    fis->lba1      = (uint8_t)(lba >>  8);
    fis->lba2      = (uint8_t)(lba >> 16);
    fis->lba3      = (uint8_t)(lba >> 24);
    fis->lba4      = (uint8_t)(lba >> 32);
    fis->lba5      = (uint8_t)(lba >> 40);
    fis->count     = (uint16_t)count;

    /* PRDT entry: point to caller's buffer */
    ct->prdt[0].dba   = (uint32_t)(uintptr_t)buf;
    ct->prdt[0].dbau  = 0;   /* buffers are in low 4 GB */
    ct->prdt[0].dbc_i = (count * 512 - 1) & 0x3FFFFF;  /* 0-based byte count */

    /* ── Fill command header for slot 0 ────────────────────── */
    ahci_cmd_header_t *hdr = &cmd_list[port][0];
    memzero(hdr, sizeof(*hdr));
    /* CFL = 5 DWORDs (20 bytes / 4); write flag in bit6 of first byte */
    hdr->cfl_atapi_write_prefetch = (5u) | (write ? (1u << 6) : 0);
    hdr->prdtl = 1;
    hdr->ctba  = (uint32_t)(uintptr_t)ct;
    hdr->ctbau = 0;

    /* ── Issue command in slot 0 ──────────────────────────── */
    MMIO32(pb + PxIS)  = (uint32_t)-1;    /* clear pending interrupts */
    MMIO32(pb + PxCI)  = 1;               /* set bit0 = slot 0 */

    /* ── Poll for completion (500 ms timeout) ───────────────── */
    uint64_t deadline = pit_get_ticks() + 500ULL;
    while (1) {
        if (!(MMIO32(pb + PxCI) & 1)) break;   /* slot 0 cleared = done */
        if (pit_get_ticks() >= deadline) {
            klog("[AHCI] command timeout\r\n");
            return -1;
        }
        /* Check for fatal errors */
        uint32_t tfd = MMIO32(pb + PxTFD);
        if (tfd & (1u << 0)) {   /* ERR bit in status */
            klog("[AHCI] device reported error, TFD=0x");
            klog_hex(tfd);
            klog("\r\n");
            return -1;
        }
    }

    return 0;   /* success */
}

/* ─── Public API ─────────────────────────────────────────── */
int ahci_read_sectors(int port, uint64_t lba, uint32_t count, void *buf)
{
    return ahci_do_rw(port, lba, count, buf, false);
}

int ahci_write_sectors(int port, uint64_t lba, uint32_t count, const void *buf)
{
    return ahci_do_rw(port, lba, count, (void *)buf, true);
}

/* ─── Sector-0 smoke test ─────────────────────────────────── */
void ahci_sector0_test(int port)
{
    static uint8_t sector_buf[512] __attribute__((aligned(512)));

    klog("[AHCI] Reading sector 0 from port ");
    klog_dec(port);
    klog("...\r\n");

    if (ahci_read_sectors(port, 0, 1, sector_buf) != 0) {
        vga_puts_color("[AHCI] Sector-0 read FAILED\n",
                       VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        klog("[AHCI] Sector-0 read FAILED\r\n");
        return;
    }

    /* Print first 16 bytes to serial */
    klog("[AHCI] Sector 0 first 16 bytes:\r\n  ");
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) {
        uint8_t b = sector_buf[i];
        char s[4] = { hex[b >> 4], hex[b & 0xF], ' ', '\0' };
        klog(s);
    }
    klog("\r\n");

    /* Check MBR boot signature (bytes 510–511 = 0x55 0xAA) */
    if (sector_buf[510] == 0x55 && sector_buf[511] == 0xAA) {
        vga_puts_color("  [ OK ] AHCI sector-0: MBR signature 0x55AA found\n",
                       VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        klog("[AHCI] MBR signature 0x55AA OK\r\n");
    } else {
        vga_puts_color("  [WARN] AHCI sector-0: no MBR signature (raw disk or GPT)\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
        klog("[AHCI] no MBR signature at bytes 510-511\r\n");
    }
}

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
#define PORT_BASE(hba, p)   ((hba) + 0x100u + (uint32_t)(p) * 0x80u)

/* Re-use the port register offsets from ahci.h */
/* Re-use cmd/status bit macros from ahci.h     */

/* Local aliases to match original code style */
#define CMD_ST   PXCMD_ST
#define CMD_FRE  PXCMD_FRE
#define CMD_FR   PXCMD_FR
#define CMD_CR   PXCMD_CR
#define TFD_BSY  PXTFD_BSY
#define TFD_DRQ  PXTFD_DRQ

/* ─── Static aligned buffers ─────────────────────────────── */

/* 32-byte command header */
typedef struct __attribute__((packed)) {
    uint8_t  cfl_atapi_write_prefetch;
    uint8_t  reset_bist_clear_busy;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} ahci_cmd_header_t;

/* PRDT entry */
typedef struct __attribute__((packed)) {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc_i;
} ahci_prdt_entry_t;

/* H2D Register FIS (20 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  fis_type;
    uint8_t  pm_c;
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0, lba1, lba2;
    uint8_t  device;
    uint8_t  lba3, lba4, lba5;
    uint8_t  featureh;
    uint16_t count;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  aux[4];
} h2d_fis_t;

/* Command table */
typedef struct __attribute__((packed)) {
    uint8_t           cfis[64];
    uint8_t           acmd[16];
    uint8_t           rsv[48];
    ahci_prdt_entry_t prdt[1];
} ahci_cmd_table_t;

static ahci_cmd_header_t cmd_list[AHCI_MAX_PORTS][32]
    __attribute__((aligned(1024)));
static uint8_t           fis_buf[AHCI_MAX_PORTS][256]
    __attribute__((aligned(256)));
static ahci_cmd_table_t  cmd_table[AHCI_MAX_PORTS]
    __attribute__((aligned(128)));

/* ─── Driver state ───────────────────────────────────────── */
static uint32_t hba_base  = 0;
static uint32_t port_mask = 0;

/* ─── Small helpers ──────────────────────────────────────── */
static void memzero(void *dst, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = 0;
}

/*
 * Spin-wait up to `ms` milliseconds for a bitmask to clear.
 * Returns false on timeout.
 */
static bool wait_clear(volatile uint32_t *reg, uint32_t mask, uint32_t ms)
{
    uint64_t deadline = pit_get_ticks() + (uint64_t)ms;
    while (*reg & mask) {
        if (pit_get_ticks() >= deadline) return false;
    }
    return true;
}

/*
 * wait_set: kept for future interrupt-driven path.
 * Suppress unused-function warning with __attribute__((unused)).
 */
static bool __attribute__((unused))
wait_set(volatile uint32_t *reg, uint32_t mask, uint32_t ms)
{
    uint64_t deadline = pit_get_ticks() + (uint64_t)ms;
    while (!(*reg & mask)) {
        if (pit_get_ticks() >= deadline) return false;
    }
    return true;
}

/* ─── Port start / stop ─────────────────────────────────── */
static void port_stop(int p)
{
    uint32_t pb = PORT_BASE(hba_base, p);
    MMIO32(pb + PxCMD) &= ~CMD_ST;
    wait_clear((volatile uint32_t *)(uintptr_t)(pb + PxCMD), CMD_CR, 500);
    MMIO32(pb + PxCMD) &= ~CMD_FRE;
    wait_clear((volatile uint32_t *)(uintptr_t)(pb + PxCMD), CMD_FR, 500);
}

static void port_start(int p)
{
    uint32_t pb = PORT_BASE(hba_base, p);
    wait_clear((volatile uint32_t *)(uintptr_t)(pb + PxCMD), CMD_CR, 500);
    MMIO32(pb + PxCMD) |= CMD_FRE;
    MMIO32(pb + PxCMD) |= CMD_ST;
}

/* ─── ahci_init ───────────────────────────────────────────── */
int ahci_init(void)
{
    pci_device_t *hba_dev = pci_find_device(0x01, 0x06);
    if (!hba_dev) {
        vga_puts_color("[AHCI] No AHCI controller found on PCI bus.\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
        klog("[AHCI] No AHCI controller found.\r\n");
        return -1;
    }

    hba_base = hba_dev->bar[5] & ~0xFu;
    if (!hba_base) {
        vga_puts_color("[AHCI] BAR5 is zero — HBA MMIO not mapped.\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
        return -1;
    }

    klog("[AHCI] HBA at BAR5=0x");
    klog_hex(hba_base);
    klog("\r\n");

    /* Enable AHCI mode */
    MMIO32(hba_base + AHCI_HBA_GHC) |= AHCI_GHC_AHCI_ENABLE;

    /* HBA reset */
    MMIO32(hba_base + AHCI_HBA_GHC) |= AHCI_GHC_RESET;
    if (!wait_clear((volatile uint32_t *)(uintptr_t)(hba_base + AHCI_HBA_GHC),
                    AHCI_GHC_RESET, 1000)) {
        vga_puts_color("[AHCI] HBA reset timeout.\n",
                       VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return -1;
    }
    MMIO32(hba_base + AHCI_HBA_GHC) |= AHCI_GHC_AHCI_ENABLE;

    /* Iterate implemented ports */
    uint32_t pi = MMIO32(hba_base + AHCI_HBA_PI);
    port_mask = 0;

    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;

        uint32_t pb   = PORT_BASE(hba_base, p);
        uint32_t ssts = MMIO32(pb + PxSSTS);
        uint8_t  det  = (uint8_t)(ssts & 0x0F);
        uint8_t  ipm  = (uint8_t)((ssts >> 8) & 0x0F);

        if (det != SSTS_DET_PRESENT || ipm != SSTS_IPM_ACTIVE) continue;
        if (MMIO32(pb + PxSIG) == AHCI_SIG_ATAPI)              continue;

        port_mask |= (1u << p);

        port_stop(p);

        uint32_t clb_phys = (uint32_t)(uintptr_t)cmd_list[p];
        uint32_t fb_phys  = (uint32_t)(uintptr_t)fis_buf[p];

        MMIO32(pb + PxCLBU) = 0;
        MMIO32(pb + PxCLB)  = clb_phys;
        MMIO32(pb + PxFBU)  = 0;
        MMIO32(pb + PxFB)   = fb_phys;

        memzero(cmd_list[p], sizeof(cmd_list[p]));
        memzero(fis_buf[p],  sizeof(fis_buf[p]));

        MMIO32(pb + PxSERR) = (uint32_t)-1;
        MMIO32(pb + PxIS)   = (uint32_t)-1;

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
        vga_puts_color("[AHCI] No SATA drives found.\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
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
    if (!count || count > 128)      return -1;

    uint32_t pb = PORT_BASE(hba_base, port);

    if (!wait_clear((volatile uint32_t *)(uintptr_t)(pb + PxTFD),
                    TFD_BSY | TFD_DRQ, 500)) {
        klog("[AHCI] port busy timeout before command\r\n");
        return -1;
    }

    ahci_cmd_table_t *ct = &cmd_table[port];
    memzero(ct, sizeof(*ct));

    h2d_fis_t *fis = (h2d_fis_t *)ct->cfis;
    fis->fis_type  = FIS_TYPE_REG_H2D;
    fis->pm_c      = 0x80;
    fis->command   = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    fis->device    = 1u << 6;
    fis->lba0      = (uint8_t)(lba);
    fis->lba1      = (uint8_t)(lba >>  8);
    fis->lba2      = (uint8_t)(lba >> 16);
    fis->lba3      = (uint8_t)(lba >> 24);
    fis->lba4      = (uint8_t)(lba >> 32);
    fis->lba5      = (uint8_t)(lba >> 40);
    fis->count     = (uint16_t)count;

    ct->prdt[0].dba   = (uint32_t)(uintptr_t)buf;
    ct->prdt[0].dbau  = 0;
    ct->prdt[0].dbc_i = (count * 512u - 1u) & 0x3FFFFFu;

    ahci_cmd_header_t *hdr = &cmd_list[port][0];
    memzero(hdr, sizeof(*hdr));
    hdr->cfl_atapi_write_prefetch = 5u | (write ? (1u << 6) : 0u);
    hdr->prdtl = 1;
    hdr->ctba  = (uint32_t)(uintptr_t)ct;
    hdr->ctbau = 0;

    MMIO32(pb + PxIS)  = (uint32_t)-1;
    MMIO32(pb + PxCI)  = 1u;

    uint64_t deadline = pit_get_ticks() + 500ULL;
    while (1) {
        if (!(MMIO32(pb + PxCI) & 1u)) break;
        if (pit_get_ticks() >= deadline) {
            klog("[AHCI] command timeout\r\n");
            return -1;
        }
        uint32_t tfd = MMIO32(pb + PxTFD);
        if (tfd & PXTFD_ERR) {
            klog("[AHCI] device error TFD=0x");
            klog_hex(tfd);
            klog("\r\n");
            return -1;
        }
    }

    return 0;
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

    static const char hex[] = "0123456789ABCDEF";
    klog("[AHCI] Sector 0 bytes 0-15:\r\n  ");
    for (int i = 0; i < 16; i++) {
        uint8_t b = sector_buf[i];
        char s[4] = { hex[b >> 4], hex[b & 0xF], ' ', '\0' };
        klog(s);
    }
    klog("\r\n");

    if (sector_buf[510] == 0x55 && sector_buf[511] == 0xAA) {
        vga_puts_color("  [ OK ] AHCI sector-0: MBR signature 0x55AA found\n",
                       VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        klog("[AHCI] MBR 0x55AA OK\r\n");
    } else {
        vga_puts_color("  [WARN] AHCI sector-0: no MBR signature\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
        klog("[AHCI] no MBR signature\r\n");
    }
}

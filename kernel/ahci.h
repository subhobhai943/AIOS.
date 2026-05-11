#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* AHCI — Advanced Host Controller Interface (SATA)
 * Provides read/write access to SATA disks via HBA MMIO registers.
 * PCI Class 0x01, Subclass 0x06.
 *
 * AIOS Specific: required for loading model weight files from disk.
 */

/* ----------------------------------------------------------------
 * HBA Global Registers (offsets from ABAR / BAR5)
 * ---------------------------------------------------------------- */
#define AHCI_HBA_CAP     0x00  /* Host Capabilities               */
#define AHCI_HBA_GHC     0x04  /* Global HBA Control              */
#define AHCI_HBA_IS      0x08  /* Interrupt Status                */
#define AHCI_HBA_PI      0x0C  /* Ports Implemented (bitmask)     */
#define AHCI_HBA_VS      0x10  /* Version                         */
#define AHCI_HBA_CCC_CTL 0x14  /* CCC Control                     */
#define AHCI_HBA_CCC_PTS 0x18  /* CCC Ports                       */
#define AHCI_HBA_EM_LOC  0x1C  /* Enclosure Management Location   */
#define AHCI_HBA_EM_CTL  0x20  /* Enclosure Management Control    */
#define AHCI_HBA_CAP2    0x24  /* Host Capabilities Extended      */
#define AHCI_HBA_BOHC    0x28  /* BIOS/OS Handoff Control         */

/* GHC register bits */
#define AHCI_GHC_AHCI_ENABLE  (1u << 31)  /* Enable AHCI mode    */
#define AHCI_GHC_INTR_ENABLE  (1u <<  1)  /* Global interrupt en */
#define AHCI_GHC_RESET        (1u <<  0)  /* HBA reset (self-cl) */

/* ----------------------------------------------------------------
 * Per-Port Register Offsets
 * Port N base = ABAR + 0x100 + N*0x80
 * ---------------------------------------------------------------- */
#define PxCLB    0x00   /* Command List Base Address (low 32)  */
#define PxCLBU   0x04   /* Command List Base Address (high 32) */
#define PxFB     0x08   /* FIS Base Address (low 32)           */
#define PxFBU    0x0C   /* FIS Base Address (high 32)          */
#define PxIS     0x10   /* Interrupt Status                    */
#define PxIE     0x14   /* Interrupt Enable                    */
#define PxCMD    0x18   /* Command and Status                  */
#define PxTFD    0x20   /* Task File Data                      */
#define PxSIG    0x24   /* Signature                           */
#define PxSSTS   0x28   /* SATA Status (SCR0: SStatus)         */
#define PxSCTL   0x2C   /* SATA Control (SCR2: SControl)       */
#define PxSERR   0x30   /* SATA Error   (SCR1: SError)         */
#define PxSACT   0x34   /* SATA Active  (NCQ only)             */
#define PxCI     0x38   /* Command Issue                       */
#define PxSNTF   0x3C   /* SATA Notification                   */
#define PxFBS    0x40   /* FIS-Based Switching Control         */

/* PxCMD bits */
#define PXCMD_ST   (1u <<  0)   /* Start                        */
#define PXCMD_SUD  (1u <<  1)   /* Spin-Up Device               */
#define PXCMD_POD  (1u <<  2)   /* Power On Device              */
#define PXCMD_CLO  (1u <<  3)   /* Command List Override        */
#define PXCMD_FRE  (1u <<  4)   /* FIS Receive Enable           */
#define PXCMD_FR   (1u << 14)   /* FIS Receive Running          */
#define PXCMD_CR   (1u << 15)   /* Command List Running         */

/* PxTFD status bits */
#define PXTFD_ERR  (1u <<  0)   /* Error bit in ATA status      */
#define PXTFD_DRQ  (1u <<  3)   /* Data transfer requested      */
#define PXTFD_BSY  (1u <<  7)   /* Interface is busy            */

/* PxSSTS DET field (bits 3:0) */
#define SSTS_DET_PRESENT  0x3   /* Device present, PHY comms OK */

/* PxSSTS IPM field (bits 11:8) */
#define SSTS_IPM_ACTIVE   0x1   /* Interface in active state    */

/* ATAPI signature (indicates ATAPI / CDROM device) */
#define AHCI_SIG_ATAPI   0xEB140101u
#define AHCI_SIG_ATA     0x00000101u

/* ATA commands used by this driver */
#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35
#define ATA_CMD_IDENTIFY       0xEC

/* H2D FIS type */
#define FIS_TYPE_REG_H2D  0x27

/* Max ports / devices this driver tracks */
#define AHCI_MAX_PORTS    4

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

/*
 * ahci_init() — locate HBA via PCI, reset, detect ports, start them.
 * Returns 0 on success, -1 if no AHCI controller found or reset fails.
 * Returns 0 (not -1) if controller found but all ports are empty;
 * that is not fatal — the system still boots.
 */
int  ahci_init(void);

/*
 * ahci_port_available(port) — true if port index is valid and a
 * SATA drive was detected during ahci_init().
 */
bool ahci_port_available(int port);

/*
 * ahci_read_sectors(port, lba, count, buf)
 * Read `count` 512-byte sectors starting at LBA `lba` from `port`
 * into the caller-supplied buffer `buf` (must be 4-byte aligned,
 * physically contiguous, in the low 4 GB).
 * Returns 0 on success, -1 on error or timeout.
 * Maximum `count` per call: 128 sectors (64 KB).
 */
int  ahci_read_sectors(int port, uint64_t lba, uint32_t count, void *buf);

/*
 * ahci_write_sectors(port, lba, count, buf)
 * Write `count` 512-byte sectors from `buf` to disk.
 * Same constraints as ahci_read_sectors.
 */
int  ahci_write_sectors(int port, uint64_t lba, uint32_t count,
                        const void *buf);

/*
 * ahci_sector0_test(port)
 * Diagnostic: read LBA 0, print first 16 bytes to serial,
 * check MBR 0x55AA signature.
 */
void ahci_sector0_test(int port);

#endif /* AHCI_H */

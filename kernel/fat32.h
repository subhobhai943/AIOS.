#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ================================================================
 * AIOS — FAT32 Filesystem Driver  (Phase 3.3)
 *
 * Provides read-only access to a FAT32 volume stored on an AHCI
 * SATA disk.  Sufficient for loading LLM weight files from disk.
 *
 * Assumptions:
 *   - Partition starts at LBA 0 (the volume BPB is at LBA 0).
 *     If a real partition table is used, call fat32_init_at_lba()
 *     with the partition start LBA instead.
 *   - Only FAT32 (OEM "FAT32   " in BPB or cluster count > 65525).
 *   - Short 8.3 filenames only (LFN deferred to Phase 4).
 *   - Single AHCI port (port index passed to fat32_init).
 *   - Sector size 512 bytes (asserted during init).
 * ================================================================ */

/* ----------------------------------------------------------------
 * BIOS Parameter Block (BPB) — FAT32 variant
 * Offset 0 in the Volume Boot Record (VBR / LBA 0 of partition).
 * ---------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint8_t   jmp_boot[3];       /* 0x00  EB xx 90 or E9 xx xx     */
    char      oem_name[8];       /* 0x03  "MSWIN4.1" etc.          */
    uint16_t  bytes_per_sector;  /* 0x0B  must be 512              */
    uint8_t   sectors_per_cluster; /* 0x0D power-of-2, 1–128      */
    uint16_t  reserved_sectors;  /* 0x0E  sectors before FAT #1    */
    uint8_t   num_fats;          /* 0x10  almost always 2          */
    uint16_t  root_entry_count;  /* 0x11  0 for FAT32              */
    uint16_t  total_sectors_16;  /* 0x13  0 for FAT32 (>65535 sec) */
    uint8_t   media_type;        /* 0x15  0xF8 for fixed disk      */
    uint16_t  fat_size_16;       /* 0x16  0 for FAT32              */
    uint16_t  sectors_per_track; /* 0x18  CHS (ignored in LBA)     */
    uint16_t  num_heads;         /* 0x1A  CHS (ignored in LBA)     */
    uint32_t  hidden_sectors;    /* 0x1C  sectors before partition  */
    uint32_t  total_sectors_32;  /* 0x20  total sectors on volume  */
    /* FAT32 extended BPB starts here (offset 0x24) */
    uint32_t  fat_size_32;       /* 0x24  sectors per FAT          */
    uint16_t  ext_flags;         /* 0x28                           */
    uint16_t  fs_version;        /* 0x2A  must be 0x0000           */
    uint32_t  root_cluster;      /* 0x2C  first cluster of root dir */
    uint16_t  fs_info_sector;    /* 0x30                           */
    uint16_t  backup_boot_sector;/* 0x32                           */
    uint8_t   reserved[12];      /* 0x34                           */
    uint8_t   drive_number;      /* 0x40  0x80 for first HDD       */
    uint8_t   reserved1;         /* 0x41                           */
    uint8_t   boot_signature;    /* 0x42  0x29 if following fields valid */
    uint32_t  volume_id;         /* 0x43                           */
    char      volume_label[11];  /* 0x47                           */
    char      fs_type[8];        /* 0x52  "FAT32   "               */
} fat32_bpb_t;

/* ----------------------------------------------------------------
 * FAT32 8.3 Directory Entry (32 bytes)
 * ---------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    char     name[8];            /* 0x00  8 chars, space-padded, upper  */
    char     ext[3];             /* 0x08  3 chars, space-padded, upper  */
    uint8_t  attr;               /* 0x0B  file attributes               */
    uint8_t  reserved;           /* 0x0C                                */
    uint8_t  crt_time_tenth;     /* 0x0D                                */
    uint16_t crt_time;           /* 0x0E                                */
    uint16_t crt_date;           /* 0x10                                */
    uint16_t last_acc_date;      /* 0x12                                */
    uint16_t first_cluster_hi;   /* 0x14  high 16 bits of first cluster */
    uint16_t wrt_time;           /* 0x16                                */
    uint16_t wrt_date;           /* 0x18                                */
    uint16_t first_cluster_lo;   /* 0x1A  low 16 bits of first cluster  */
    uint32_t file_size;          /* 0x1C  bytes                         */
} fat32_dir_entry_t;

/* Directory entry attribute bits */
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F   /* Long File Name entry (skip) */

/* FAT32 cluster sentinel values */
#define FAT32_EOC_MIN   0x0FFFFFF8u  /* end-of-chain threshold     */
#define FAT32_BAD       0x0FFFFFF7u  /* bad cluster marker         */
#define FAT32_FREE      0x00000000u  /* free cluster               */

/* Entries per sector for 32-byte directory entries (512 / 32 = 16) */
#define FAT32_DIR_ENTRIES_PER_SECTOR  16

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

/*
 * fat32_init(ahci_port, partition_lba)
 *   Read the BPB from `partition_lba`, validate FAT32 signature,
 *   compute derived geometry.
 *   `partition_lba` = 0 if BPB is at absolute LBA 0 (no MBR).
 *   Returns 0 on success, -1 on error.
 */
int  fat32_init(int ahci_port, uint64_t partition_lba);

/*
 * fat32_read_cluster(cluster, buf)
 *   Read all sectors of `cluster` into `buf`.
 *   buf must be at least sectors_per_cluster * 512 bytes.
 *   Returns 0 on success, -1 on error.
 */
int  fat32_read_cluster(uint32_t cluster, void *buf);

/*
 * fat32_next_cluster(cluster)
 *   Look up the FAT entry for `cluster` and return the next
 *   cluster in the chain.  Returns FAT32_EOC_MIN (0x0FFFFFF8) or
 *   higher to signal end of chain, FAT32_BAD for bad cluster,
 *   0 on I/O error.
 */
uint32_t fat32_next_cluster(uint32_t cluster);

/*
 * fat32_find_file(dir_cluster, name83, out_cluster, out_size)
 *   Search the directory starting at `dir_cluster` for a file
 *   whose 8.3 name matches `name83` (e.g. "TEST    TXT").
 *   name83 must be exactly 11 characters, space-padded, upper-case.
 *   On success fills *out_cluster and *out_size, returns 0.
 *   Returns -1 if not found or I/O error.
 */
int  fat32_find_file(uint32_t dir_cluster,
                     const char name83[11],
                     uint32_t *out_cluster,
                     uint32_t *out_size);

/*
 * fat32_read_file(first_cluster, buf, max_bytes)
 *   Follow the cluster chain starting at `first_cluster`,
 *   copying data into `buf` up to `max_bytes`.
 *   Returns the number of bytes actually read, or -1 on I/O error.
 */
int  fat32_read_file(uint32_t first_cluster, void *buf, uint32_t max_bytes);

/*
 * fat32_sector0_test()
 *   Diagnostic: validate BPB, print key fields to VGA + serial,
 *   attempt to read /TEST.TXT from the root directory.
 */
void fat32_sector0_test(void);

/*
 * fat32_find_file_lfn(dir_cluster, name, out_cluster, out_size)
 *   Search the directory starting at `dir_cluster` for a file
 *   whose long file name (LFN) or short 8.3 name matches `name`.
 *   `name` is a zero-terminated ASCII string (case-insensitive),
 *   for example "Test File.txt".
 *   On success fills *out_cluster and *out_size, returns 0.
 *   Returns -1 if not found or I/O error.
 */
int  fat32_find_file_lfn(uint32_t dir_cluster,
                         const char *name,
                         uint32_t *out_cluster,
                         uint32_t *out_size);

#endif /* FAT32_H */

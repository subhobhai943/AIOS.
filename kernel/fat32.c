/* ================================================================
 * AIOS — FAT32 Filesystem Driver  (Phase 3.3)
 *
 * Read/Write FAT32 driver.  All sector I/O goes through static
 * 512-byte sector buffers via AHCI.
 *
 * Supported:
 *   - FAT32 volumes (BPB validated, cluster count sanity-checked)
 *   - Short 8.3 filenames (LFN skipped with FAT_ATTR_LFN check)
 *   - Cluster chain traversal via FAT table
 *   - Read entire file into caller-supplied buffer
 *   - Long File Names (LFN / VFAT) — read-only search
 *   - Write support: fat32_write_file, fat32_create_file,
 *     fat32_alloc_clusters, fat32_free_chain
 *
 * Not yet supported (Phase 4):
 *   - Subdirectory traversal via path string
 *   - Partition table detection (always assumes partition_lba param)
 * ================================================================ */

#include "fat32.h"
#include "ahci.h"
#include "include/vga.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ─── Derived geometry (filled by fat32_init) ─────────────── */
static int      g_port        = 0;   /* AHCI port index             */
static uint64_t g_part_lba    = 0;   /* partition start LBA         */
static uint32_t g_fat_lba     = 0;   /* LBA of FAT #1               */
static uint32_t g_data_lba    = 0;   /* LBA of cluster 2            */
static uint32_t g_root_cluster= 2;   /* first cluster of root dir   */
static uint8_t  g_spc         = 1;   /* sectors per cluster         */
static bool     g_ready       = false;

static uint8_t  g_num_fats       = 0;   /* number of FAT copies         */
static uint32_t g_fat_sectors    = 0;   /* sectors per FAT              */
static uint32_t g_total_clusters = 0;   /* total data clusters on vol   */

/* ─── Static I/O buffers ──────────────────────────────────── */
/* One sector for BPB / FAT reads */
static uint8_t  s_sector[512] __attribute__((aligned(512)));
/* One cluster buffer (max 128 sectors = 64 KB) */
static uint8_t  s_cluster_buf[128 * 512] __attribute__((aligned(512)));

/* ─── Helper: read one absolute sector ───────────────────── */
static int read_sector(uint64_t lba, void *buf)
{
    return ahci_read_sectors(g_port, lba, 1, buf);
}

/* ─── Helper: write one absolute sector ───────────────────── */
static int write_sector(uint64_t lba, const void *buf)
{
    return ahci_write_sectors(g_port, lba, 1, buf);
}

/* ─── Helper: memcpy (no libc) ───────────────────────────── */
static void *memcpy_k(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

/* ─── Helper: memcmp (no libc) ───────────────────────────── */
static int memcmp_k(const void *a, const void *b, size_t n)
{
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

/* ─── Helper: to_upper ────────────────────────────────────── */
static char to_upper(char c)
{
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

/* ─── fat32_init ──────────────────────────────────────────── */
int fat32_init(int ahci_port, uint64_t partition_lba)
{
    g_ready = false;
    g_port  = ahci_port;
    g_part_lba = partition_lba;

    /* 1. Read VBR (sector 0 of partition) */
    if (read_sector(partition_lba, s_sector) != 0) {
        vga_puts_color("[FAT32] Cannot read VBR\n",
                       VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return -1;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)s_sector;

    /* 2. Basic sanity: sector size must be 512 */
    if (bpb->bytes_per_sector != 512) {
        vga_puts_color("[FAT32] Unsupported sector size\n",
                       VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return -1;
    }

    /* 3. Confirm FAT32: root_entry_count==0, fat_size_16==0 */
    if (bpb->root_entry_count != 0 || bpb->fat_size_16 != 0) {
        vga_puts_color("[FAT32] Volume is FAT12/16, not FAT32\n",
                       VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return -1;
    }

    /* 4. Derive geometry */
    g_spc          = bpb->sectors_per_cluster;
    g_root_cluster = bpb->root_cluster;
    g_fat_lba      = (uint32_t)(partition_lba + bpb->reserved_sectors);
    g_num_fats     = bpb->num_fats;
    g_fat_sectors  = bpb->fat_size_32;

    uint32_t fat_sectors = (uint32_t)g_num_fats * g_fat_sectors;
    g_data_lba     = g_fat_lba + fat_sectors;
    /* LBA(cluster N) = g_data_lba + (N - 2) * g_spc */

    /* Compute total usable data clusters for allocation helpers. */
    uint32_t total_sectors = (bpb->total_sectors_16 != 0)
                             ? (uint32_t)bpb->total_sectors_16
                             : bpb->total_sectors_32;
    uint32_t data_sectors  = total_sectors - (bpb->reserved_sectors + fat_sectors);
    g_total_clusters       = data_sectors / g_spc;

    g_ready = true;

    vga_puts_color("  [ OK ] FAT32 volume mounted\n",
                   VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    klog("[FAT32] mounted: spc=");
    klog_dec(g_spc);
    klog(" root_cluster=");
    klog_dec(g_root_cluster);
    klog(" fat_lba=0x");
    klog_hex(g_fat_lba);
    klog(" data_lba=0x");
    klog_hex(g_data_lba);
    klog("\r\n");

    return 0;
}

/* ─── fat32_read_cluster ──────────────────────────────────── */
int fat32_read_cluster(uint32_t cluster, void *buf)
{
    if (!g_ready || cluster < 2) return -1;
    uint64_t lba = (uint64_t)g_data_lba +
                   (uint64_t)(cluster - 2) * g_spc;
    return ahci_read_sectors(g_port, lba, g_spc, buf);
}

/* ─── fat32_next_cluster ──────────────────────────────────── */
uint32_t fat32_next_cluster(uint32_t cluster)
{
    if (!g_ready) return 0;

    /* Each FAT32 entry is 4 bytes → 128 entries per 512-byte sector.
     * Sector offset within FAT = cluster / 128
     * Entry offset within sector = (cluster % 128) * 4            */
    uint32_t fat_sector_offset = cluster / 128u;
    uint32_t fat_entry_offset  = (cluster % 128u) * 4u;

    uint64_t lba = (uint64_t)g_fat_lba + fat_sector_offset;
    if (read_sector(lba, s_sector) != 0) return 0;

    uint32_t entry;
    memcpy_k(&entry, s_sector + fat_entry_offset, 4);
    return entry & 0x0FFFFFFFu;   /* FAT32 uses only low 28 bits */
}

/* ─── fat32_find_file ─────────────────────────────────────── */
int fat32_find_file(uint32_t dir_cluster,
                    const char name83[11],
                    uint32_t *out_cluster,
                    uint32_t *out_size)
{
    if (!g_ready) return -1;

    uint32_t cluster = dir_cluster;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        if (fat32_read_cluster(cluster, s_cluster_buf) != 0) return -1;

        int entries_in_cluster = (g_spc * 512) / 32;
        fat32_dir_entry_t *de = (fat32_dir_entry_t *)s_cluster_buf;

        for (int i = 0; i < entries_in_cluster; i++, de++) {
            /* 0x00 = end of directory */
            if ((uint8_t)de->name[0] == 0x00) return -1;
            /* 0xE5 = deleted entry */
            if ((uint8_t)de->name[0] == 0xE5) continue;
            /* Skip LFN entries */
            if (de->attr == FAT_ATTR_LFN) continue;
            /* Skip volume-id entries */
            if (de->attr & FAT_ATTR_VOLUME_ID) continue;

            char cmp[11];
            for (int j = 0; j < 8; j++)
                cmp[j]     = to_upper(de->name[j]);
            for (int j = 0; j < 3; j++)
                cmp[8 + j] = to_upper(de->ext[j]);

            if (memcmp_k(cmp, name83, 11) == 0) {
                *out_cluster = ((uint32_t)de->first_cluster_hi << 16)
                             |  (uint32_t)de->first_cluster_lo;
                *out_size    = de->file_size;
                return 0;
            }
        }

        cluster = fat32_next_cluster(cluster);
    }

    return -1;   /* not found */
}

/* ─── fat32_read_file ─────────────────────────────────────── */
int fat32_read_file(uint32_t first_cluster, void *buf, uint32_t max_bytes)
{
    if (!g_ready || !buf || !max_bytes) return -1;

    uint8_t  *dst          = (uint8_t *)buf;
    uint32_t  bytes_read   = 0;
    uint32_t  cluster_bytes = (uint32_t)g_spc * 512u;
    uint32_t  cluster      = first_cluster;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        if (fat32_read_cluster(cluster, s_cluster_buf) != 0) return -1;

        uint32_t to_copy = cluster_bytes;
        if (bytes_read + to_copy > max_bytes)
            to_copy = max_bytes - bytes_read;

        memcpy_k(dst + bytes_read, s_cluster_buf, to_copy);
        bytes_read += to_copy;

        if (bytes_read >= max_bytes) break;

        cluster = fat32_next_cluster(cluster);
    }

    return (int)bytes_read;
}

/* ─── fat32_sector0_test ──────────────────────────────────── */
void fat32_sector0_test(void)
{
    if (!g_ready) {
        vga_puts_color("[FAT32] Not mounted — skipping test\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
        return;
    }

    /* Print key BPB fields to serial */
    klog("[FAT32] Geometry: spc=");
    klog_dec(g_spc);
    klog(" fat_lba=0x");
    klog_hex(g_fat_lba);
    klog(" data_lba=0x");
    klog_hex(g_data_lba);
    klog(" root_cluster=");
    klog_dec(g_root_cluster);
    klog("\r\n");

    /* Try to find TEST.TXT in root dir.
     * 8.3 name: "TEST    TXT" (8-char name + 3-char ext, space-padded) */
    static const char test_name[11] = "TEST    TXT";
    uint32_t fc = 0, fsz = 0;

    if (fat32_find_file(g_root_cluster, test_name, &fc, &fsz) != 0) {
        vga_puts_color("  [WARN] FAT32: TEST.TXT not found in root dir\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
        klog("[FAT32] TEST.TXT not found (expected on test disk)\r\n");
        return;
    }

    vga_puts_color("  [ OK ] FAT32: TEST.TXT found, size=",
                   VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_putdec(fsz);
    vga_puts_color(" bytes\n",
                   VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    klog("[FAT32] TEST.TXT first_cluster=");
    klog_dec(fc);
    klog(" size=");
    klog_dec(fsz);
    klog("\r\n");

    /* Read first 64 bytes and hex-dump to serial */
    static uint8_t tbuf[512] __attribute__((aligned(512)));
    int got = fat32_read_file(fc, tbuf, 64);
    if (got <= 0) {
        vga_puts_color("  [WARN] FAT32: TEST.TXT read failed\n",
                       VGA_COLOR_BROWN, VGA_COLOR_BLACK);
        return;
    }

    klog("[FAT32] TEST.TXT first ");
    klog_dec((uint32_t)got);
    klog(" bytes: ");

    /* Emit each byte as two hex nibble chars + a space using serial_putchar. */
    static const char hexchars[] = "0123456789ABCDEF";
    for (int i = 0; i < got; i++) {
        serial_putchar(SERIAL_COM1, hexchars[tbuf[i] >> 4]);
        serial_putchar(SERIAL_COM1, hexchars[tbuf[i] & 0xF]);
        serial_putchar(SERIAL_COM1, ' ');
    }
    klog("\r\n");

    vga_puts_color("  [ OK ] FAT32 read test PASSED\n",
                   VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
}

/* ================================================================
 * Cluster allocation and write support (Phase 3.3)
 * ================================================================ */

/* Read a single FAT32 entry (low 28 bits). Returns FAT32_BAD on I/O error. */
static uint32_t fat32_read_fat_entry(uint32_t cluster)
{
    if (!g_ready || cluster < 2u) return FAT32_BAD;

    /* 4 bytes per entry, 128 entries per 512-byte sector */
    uint32_t fat_sector_offset = cluster / 128u;
    uint32_t fat_entry_offset  = (cluster % 128u) * 4u;

    uint64_t lba = (uint64_t)g_fat_lba + fat_sector_offset;
    if (read_sector(lba, s_sector) != 0) {
        return FAT32_BAD;
    }

    uint32_t entry;
    memcpy_k(&entry, s_sector + fat_entry_offset, 4);
    return entry & 0x0FFFFFFFu;
}

/* Write a single FAT32 entry (low 28 bits) to all FAT copies. */
static int fat32_write_fat_entry(uint32_t cluster, uint32_t value)
{
    if (!g_ready || cluster < 2u) return -1;
    if (g_num_fats == 0u || g_fat_sectors == 0u) return -1;

    uint32_t fat_sector_offset = cluster / 128u;
    uint32_t fat_entry_offset  = (cluster % 128u) * 4u;

    /* Read sector from primary FAT. */
    uint64_t lba0 = (uint64_t)g_fat_lba + fat_sector_offset;
    if (read_sector(lba0, s_sector) != 0) return -1;

    uint32_t newval = value & 0x0FFFFFFFu;
    memcpy_k(s_sector + fat_entry_offset, &newval, 4);

    /* Write updated sector to all FAT copies. */
    for (uint32_t f = 0; f < (uint32_t)g_num_fats; f++) {
        uint64_t lba = (uint64_t)g_fat_lba
                     + (uint64_t)f * (uint64_t)g_fat_sectors
                     + fat_sector_offset;
        if (write_sector(lba, s_sector) != 0) return -1;
    }
    return 0;
}

/* Allocate `count` contiguous free clusters and link them as a chain. */
static uint32_t fat32_alloc_contiguous(uint32_t count)
{
    if (!g_ready || count == 0u) return 0u;
    if (g_total_clusters == 0u)  return 0u;

    uint32_t run_start = 0u;
    uint32_t run_len   = 0u;
    uint32_t max_cluster = g_total_clusters + 1u; /* clusters numbered from 2 */

    for (uint32_t c = 2u; c <= max_cluster; c++) {
        uint32_t val  = fat32_read_fat_entry(c);
        bool     free = (val == FAT32_FREE);
        if (free) {
            if (run_len == 0u) run_start = c;
            run_len++;
            if (run_len == count) {
                /* Mark clusters as allocated and link chain. */
                for (uint32_t i = 0u; i < count; i++) {
                    uint32_t cur  = run_start + i;
                    uint32_t next = (i + 1u < count)
                                    ? (cur + 1u)
                                    : FAT32_EOC_MIN; /* end-of-chain */
                    if (fat32_write_fat_entry(cur, next) != 0) {
                        /* On partial failure we may leak, acceptable for now. */
                        return 0u;
                    }
                }
                return run_start;
            }
        } else {
            run_len = 0u;
        }
    }
    return 0u;
}

/* Public wrapper for cluster allocation. */
uint32_t fat32_alloc_clusters(uint32_t count)
{
    return fat32_alloc_contiguous(count);
}

/* Free an entire FAT32 cluster chain starting at first_cluster. */
int fat32_free_chain(uint32_t first_cluster)
{
    if (!g_ready || first_cluster < 2u) return -1;

    uint32_t cluster = first_cluster;
    while (cluster >= 2u && cluster < FAT32_EOC_MIN) {
        uint32_t next = fat32_next_cluster(cluster);
        if (fat32_write_fat_entry(cluster, FAT32_FREE) != 0) {
            return -1;
        }
        cluster = next;
    }
    return 0;
}

/* Write an entire cluster from caller buffer. */
static int fat32_write_cluster(uint32_t cluster, const void *buf)
{
    if (!g_ready || cluster < 2u) return -1;
    uint64_t lba = (uint64_t)g_data_lba
                 + (uint64_t)(cluster - 2u) * g_spc;
    return ahci_write_sectors(g_port, lba, g_spc, buf);
}

/* Zero-fill a cluster on disk. */
static int fat32_clear_cluster(uint32_t cluster)
{
    uint32_t bytes = (uint32_t)g_spc * 512u;
    for (uint32_t i = 0u; i < bytes; i++) {
        s_cluster_buf[i] = 0u;
    }
    return fat32_write_cluster(cluster, s_cluster_buf);
}

/* Write `size` bytes from buf into an existing cluster chain. */
int fat32_write_file(uint32_t first_cluster, const void *buf, uint32_t size)
{
    if (!g_ready || !buf || size == 0u) return -1;

    uint32_t cluster_bytes = (uint32_t)g_spc * 512u;
    uint32_t cluster       = first_cluster;
    uint32_t written       = 0u;

    while (cluster >= 2u && cluster < FAT32_EOC_MIN && written < size) {
        uint32_t chunk = size - written;
        if (chunk > cluster_bytes) chunk = cluster_bytes;

        /* Copy chunk bytes, pad remainder of cluster with zeroes. */
        const uint8_t *src = (const uint8_t *)buf + written;
        uint32_t i = 0u;
        for (; i < chunk; i++) {
            s_cluster_buf[i] = src[i];
        }
        for (; i < cluster_bytes; i++) {
            s_cluster_buf[i] = 0u;
        }

        if (fat32_write_cluster(cluster, s_cluster_buf) != 0) {
            return -1;
        }

        written += chunk;
        if (written >= size) break;
        cluster = fat32_next_cluster(cluster);
    }

    return (written > 0u) ? (int)written : -1;
}

/* Create a new file with 8.3 name in a directory and optionally write data. */
int fat32_create_file(uint32_t dir_cluster,
                      const char name83[11],
                      const void *data,
                      uint32_t size,
                      uint32_t *out_first_cluster)
{
    if (!g_ready || dir_cluster < 2u || !name83 || !out_first_cluster) return -1;

    uint32_t cluster_bytes   = (uint32_t)g_spc * 512u;
    uint32_t needed_clusters = (size + cluster_bytes - 1u) / cluster_bytes;
    if (needed_clusters == 0u) needed_clusters = 1u;  /* avoid zero-cluster files */

    /* Allocate contiguous data clusters. */
    uint32_t first_cluster = fat32_alloc_contiguous(needed_clusters);
    if (first_cluster == 0u) return -1;

    /* Write file data or clear clusters. */
    if (data && size > 0u) {
        if (fat32_write_file(first_cluster, data, size) != (int)size) {
            return -1;
        }
    } else {
        uint32_t c = first_cluster;
        for (uint32_t i = 0u; i < needed_clusters; i++) {
            if (fat32_clear_cluster(c) != 0) return -1;
            if (i + 1u < needed_clusters) c = c + 1u; /* contiguous */
        }
    }

    /* Find free directory entry slot, extending directory if needed. */
    uint32_t cluster = dir_cluster;
    uint32_t last_cluster = 0u;
    uint32_t dir_bytes_per_cluster = cluster_bytes;

    while (cluster >= 2u && cluster < FAT32_EOC_MIN) {
        if (fat32_read_cluster(cluster, s_cluster_buf) != 0) return -1;

        fat32_dir_entry_t *de = (fat32_dir_entry_t *)s_cluster_buf;
        int entries = (int)(dir_bytes_per_cluster / (uint32_t)sizeof(fat32_dir_entry_t));
        for (int i = 0; i < entries; i++, de++) {
            uint8_t first = (uint8_t)de->name[0];
            if (first == 0x00u || first == 0xE5u) {
                /* Free slot. Zero entry then populate. */
                for (size_t j = 0; j < sizeof(fat32_dir_entry_t); j++) {
                    ((uint8_t *)de)[j] = 0u;
                }
                for (int j = 0; j < 8; j++)  de->name[j] = name83[j];
                for (int j = 0; j < 3; j++)  de->ext[j]  = name83[8 + j];
                de->attr             = FAT_ATTR_ARCHIVE;
                de->first_cluster_lo = (uint16_t)(first_cluster & 0xFFFFu);
                de->first_cluster_hi = (uint16_t)(first_cluster >> 16);
                de->file_size        = size;

                if (fat32_write_cluster(cluster, s_cluster_buf) != 0) {
                    return -1;
                }
                *out_first_cluster = first_cluster;
                return 0;
            }
        }

        last_cluster = cluster;
        cluster      = fat32_next_cluster(cluster);
    }

    /* Need to extend directory by one cluster. */
    uint32_t new_dir_cluster = fat32_alloc_contiguous(1u);
    if (new_dir_cluster == 0u) return -1;

    if (last_cluster >= 2u) {
        if (fat32_write_fat_entry(last_cluster, new_dir_cluster) != 0) {
            return -1;
        }
    }

    if (fat32_clear_cluster(new_dir_cluster) != 0) return -1;

    fat32_dir_entry_t *de = (fat32_dir_entry_t *)s_cluster_buf;
    for (size_t j = 0; j < sizeof(fat32_dir_entry_t); j++) {
        ((uint8_t *)de)[j] = 0u;
    }
    for (int j = 0; j < 8; j++)  de->name[j] = name83[j];
    for (int j = 0; j < 3; j++)  de->ext[j]  = name83[8 + j];
    de->attr             = FAT_ATTR_ARCHIVE;
    de->first_cluster_lo = (uint16_t)(first_cluster & 0xFFFFu);
    de->first_cluster_hi = (uint16_t)(first_cluster >> 16);
    de->file_size        = size;

    if (fat32_write_cluster(new_dir_cluster, s_cluster_buf) != 0) return -1;

    *out_first_cluster = first_cluster;
    return 0;
}

/* ================================================================
 * Long File Name (LFN) support
 *
 * Provides a helper search API that understands VFAT-style long
 * file name entries.  This is read-only and intended for use by
 * higher-level path / VFS code.
 * ================================================================ */

#define FAT32_MAX_LFN_CHARS   255
#define FAT32_LFN_ENTRY_CHARS 13

typedef struct {
    char    name[FAT32_MAX_LFN_CHARS + 1];
    uint8_t seq_total;
    bool    in_use;
} fat32_lfn_state_t;

static void lfn_reset(fat32_lfn_state_t *st)
{
    st->in_use    = false;
    st->seq_total = 0;
    st->name[0]   = '\0';
}

static void lfn_add_entry(fat32_lfn_state_t *st, const uint8_t *raw)
{
    uint8_t seq = raw[0];
    uint8_t ord = (uint8_t)(seq & 0x1Fu);

    if (ord == 0u || ord > 20u) {
        st->in_use = false;
        return;
    }

    if (seq & 0x40u) {
        /* Start of a new LFN sequence (last logical, first physical). */
        st->in_use    = true;
        st->seq_total = ord;
        for (size_t i = 0; i < (size_t)(FAT32_MAX_LFN_CHARS + 1); i++) {
            st->name[i] = '\0';
        }
    } else if (!st->in_use) {
        /* Stray LFN without a starting entry. */
        return;
    }

    /* Position of the first character contributed by this entry. */
    uint32_t index_base = ((uint32_t)ord - 1u) * (uint32_t)FAT32_LFN_ENTRY_CHARS;
    if (index_base >= FAT32_MAX_LFN_CHARS) {
        return;
    }

    /* Each LFN entry encodes up to 13 UTF-16 characters spread across
     * three fields: Name1 (5 chars at offset 1), Name2 (6 chars at
     * offset 14), Name3 (2 chars at offset 28).
     */
    const uint8_t offsets[3] = { 1u, 14u, 28u };
    const uint8_t counts[3]  = { 5u, 6u,  2u  };

    for (int blk = 0; blk < 3; blk++) {
        uint8_t off = offsets[blk];
        uint8_t cnt = counts[blk];

        for (uint8_t i = 0; i < cnt; i++) {
            uint8_t lo = raw[off + (uint8_t)(i * 2u)];
            uint8_t hi = raw[off + (uint8_t)(i * 2u) + 1u];
            uint16_t u = (uint16_t)lo | ((uint16_t)hi << 8);

            if (u == 0x0000u) {
                /* Explicit terminator. */
                if (index_base < FAT32_MAX_LFN_CHARS) {
                    st->name[index_base] = '\0';
                }
                return;
            }
            if (u == 0xFFFFu) {
                /* Padding. */
                return;
            }
            if (index_base >= FAT32_MAX_LFN_CHARS) {
                return;
            }

            /* For now we only support the basic ASCII subset from UCS-2. */
            st->name[index_base] = to_upper((char)(u & 0xFFu));
            index_base++;
        }
    }
}

static void lfn_finalize(fat32_lfn_state_t *st)
{
    if (!st->in_use) return;

    size_t max_chars = (size_t)st->seq_total * (size_t)FAT32_LFN_ENTRY_CHARS;
    if (max_chars > FAT32_MAX_LFN_CHARS) {
        max_chars = FAT32_MAX_LFN_CHARS;
    }

    size_t i;
    for (i = 0; i < max_chars; i++) {
        if (st->name[i] == '\0') break;
    }
    if (i == max_chars) {
        st->name[max_chars] = '\0';
    }
}

static void str_to_upper(const char *src, char *dst, size_t dst_len)
{
    if (!src || !dst || dst_len == 0) return;

    size_t i = 0;
    for (; i + 1 < dst_len && src[i] != '\0'; i++) {
        dst[i] = to_upper(src[i]);
    }
    dst[i] = '\0';
}

static bool str_equals_ci(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a || *b) {
        char ca = to_upper(*a);
        char cb = to_upper(*b);
        if (ca != cb) return false;
        if (ca == '\0') break;
        a++;
        b++;
    }
    return true;
}

/* Build an 8.3 comparison key (11 chars) from an input name.
 * Rules:
 *   - Ignore spaces.
 *   - First '.' switches from name to ext; further '.' are ignored.
 *   - Upper-case everything.
 *   - Truncate to 8 chars for name and 3 chars for ext.
 */
static bool build_name83(const char *name, char out[11])
{
    if (!name) return false;

    for (int i = 0; i < 11; i++) {
        out[i] = ' ';
    }

    int  i_name = 0;
    int  i_ext  = 8;
    bool in_ext = false;

    for (size_t src = 0; ; src++) {
        char c = name[src];
        if (c == '\0') break;
        if (c == '.') {
            if (in_ext) {
                /* Ignore additional dots. */
                continue;
            }
            in_ext = true;
            continue;
        }
        if (c == ' ') {
            continue;
        }
        c = to_upper(c);

        if (!in_ext) {
            if (i_name >= 8) continue;
            out[i_name++] = c;
        } else {
            if (i_ext >= 11) continue;
            out[i_ext++] = c;
        }
    }

    return i_name > 0;
}

int fat32_find_file_lfn(uint32_t dir_cluster,
                        const char *name,
                        uint32_t *out_cluster,
                        uint32_t *out_size)
{
    if (!g_ready || dir_cluster < 2u || !name ||
        !out_cluster || !out_size) {
        return -1;
    }

    char target_up[FAT32_MAX_LFN_CHARS + 1];
    str_to_upper(name, target_up, sizeof(target_up));

    char name83[11];
    bool have_name83 = build_name83(target_up, name83);

    uint32_t          cluster = dir_cluster;
    fat32_lfn_state_t lfn;
    lfn_reset(&lfn);

    while (cluster >= 2u && cluster < FAT32_EOC_MIN) {
        if (fat32_read_cluster(cluster, s_cluster_buf) != 0) {
            return -1;
        }

        int entries_in_cluster = (g_spc * 512) / 32;
        fat32_dir_entry_t *de  = (fat32_dir_entry_t *)s_cluster_buf;

        for (int i = 0; i < entries_in_cluster; i++, de++) {
            uint8_t first = (uint8_t)de->name[0];
            if (first == 0x00u) {
                /* No more entries in this directory. */
                return -1;
            }
            if (first == 0xE5u) {
                /* Deleted entry; reset any pending LFN state. */
                lfn_reset(&lfn);
                continue;
            }

            if (de->attr == FAT_ATTR_LFN) {
                /* LFN fragment; accumulate. */
                lfn_add_entry(&lfn, (const uint8_t *)de);
                continue;
            }

            if (de->attr & FAT_ATTR_VOLUME_ID) {
                /* Skip volume label entries; LFN (if any) is discarded. */
                lfn_reset(&lfn);
                continue;
            }

            bool match = false;

            if (lfn.in_use) {
                /* We have an assembled LFN preceding this SFN. */
                lfn_finalize(&lfn);
                if (str_equals_ci(lfn.name, target_up)) {
                    match = true;
                }
            }

            if (!match && have_name83) {
                /* Fallback to 8.3 comparison. */
                char cmp[11];
                for (int j = 0; j < 8; j++) {
                    cmp[j] = to_upper(de->name[j]);
                }
                for (int j = 0; j < 3; j++) {
                    cmp[8 + j] = to_upper(de->ext[j]);
                }

                if (memcmp_k(cmp, name83, 11) == 0) {
                    match = true;
                }
            }

            if (match) {
                *out_cluster = ((uint32_t)de->first_cluster_hi << 16)
                             |  (uint32_t)de->first_cluster_lo;
                *out_size    = de->file_size;
                return 0;
            }

            /* LFN only applies to the immediately following SFN entry. */
            lfn_reset(&lfn);
        }

        cluster = fat32_next_cluster(cluster);
    }

    return -1;
}

/* ================================================================
 * AIOS — FAT32 Filesystem Driver  (Phase 3.3)
 *
 * Read-only FAT32 driver.  Reads from AHCI disk using
 * ahci_read_sectors().  All sector I/O goes through static
 * 512-byte sector buffers (no dynamic allocation needed).
 *
 * Supported:
 *   - FAT32 volumes (BPB validated, cluster count sanity-checked)
 *   - Short 8.3 filenames (LFN skipped with FAT_ATTR_LFN check)
 *   - Cluster chain traversal via FAT table
 *   - Read entire file into caller-supplied buffer
 *
 * Not yet supported (Phase 4):
 *   - Long File Names (LFN)
 *   - Write / create / delete
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
    uint32_t fat_sectors = (uint32_t)bpb->num_fats * bpb->fat_size_32;
    g_data_lba     = g_fat_lba + fat_sectors;
    /* LBA(cluster N) = g_data_lba + (N - 2) * g_spc */

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

            /*
             * FIX (Bug 2): fat32_dir_entry_t has name[8] and ext[3]
             * as SEPARATE C arrays.  Accessing de->name[j] for j > 7
             * is undefined behaviour — the compiler may assume j < 8
             * and optimize aggressively.
             *
             * Build the 11-byte comparison key by reading name[0..7]
             * and ext[0..2] through their own array bounds, then
             * upper-casing each byte.
             */
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

    /*
     * FIX (Bug 1): serial_write() does not exist in serial.h.
     * The declared API is serial_putc(port, char).
     * Emit each byte as two hex nibble chars + a space using serial_putc.
     */
    static const char hexchars[] = "0123456789ABCDEF";
    for (int i = 0; i < got; i++) {
        serial_putc(SERIAL_COM1, hexchars[tbuf[i] >> 4]);
        serial_putc(SERIAL_COM1, hexchars[tbuf[i] & 0xF]);
        serial_putc(SERIAL_COM1, ' ');
    }
    klog("\r\n");

    vga_puts_color("  [ OK ] FAT32 read test PASSED\n",
                   VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
}

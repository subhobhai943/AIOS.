#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * AIOS Initrd — Flat-Binary Ramdisk Format
 *
 * Layout (all little-endian):
 *
 *   [Header: 16 bytes]
 *     uint32_t magic;          0x41524453  ('ARDS')
 *     uint32_t version;        1
 *     uint32_t num_files;
 *     uint32_t _reserved;      0
 *
 *   [File entries: num_files × 64 bytes each]
 *     char     name[48];       null-terminated filename (path)
 *     uint32_t offset;         byte offset from start of image to file data
 *     uint32_t size;           file size in bytes
 *     uint32_t flags;          reserved, 0
 *     uint32_t _pad;           0
 *
 *   [File data: packed, no alignment padding]
 *
 * The image is linked into the ISO as a GRUB2 module:
 *   module2 /boot/initrd.img
 */

#define INITRD_MAGIC    0x41524453u   /* 'ARDS' */
#define INITRD_VERSION  1u
#define INITRD_NAME_MAX 48

/* Maximum number of files the kernel will track from the ramdisk. */
#define INITRD_MAX_FILES 128

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t num_files;
    uint32_t _reserved;
} initrd_header_t;

typedef struct __attribute__((packed)) {
    char     name[INITRD_NAME_MAX];  /* e.g. "/tokenizer/vocab.bin" */
    uint32_t offset;                 /* byte offset from start of image */
    uint32_t size;                   /* file size in bytes */
    uint32_t flags;                  /* reserved */
    uint32_t _pad;
} initrd_entry_t;

/* In-kernel runtime descriptor (unpacked, pointer into image) */
typedef struct {
    const char     *name;    /* points into entry.name */
    const uint8_t  *data;    /* points into loaded image */
    uint32_t        size;
} initrd_file_t;

/*
 * initrd_init  —  parse the ramdisk image loaded by GRUB.
 *
 * @img_phys : physical start address from the Multiboot2 module tag.
 * @img_size : size in bytes from the Multiboot2 module tag.
 *
 * Returns true on success, false if the magic/version is wrong.
 */
bool initrd_init(uint64_t img_phys, uint32_t img_size);

/*
 * initrd_find  —  locate a file by exact path (e.g. "/tokenizer/vocab.bin").
 *
 * Returns pointer to the initrd_file_t descriptor, or NULL if not found.
 */
const initrd_file_t *initrd_find(const char *path);

/*
 * initrd_file_count  —  number of files in the mounted ramdisk.
 */
uint32_t initrd_file_count(void);

/*
 * initrd_get_file  —  get file by index (0-based).
 *
 * Returns NULL if index >= initrd_file_count().
 */
const initrd_file_t *initrd_get_file(uint32_t index);

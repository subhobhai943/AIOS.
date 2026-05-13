#pragma once
#include <stdbool.h>

/*
 * VFS backend: Initrd (ramdisk)
 *
 * Registers the ramdisk as an additional read-only mount point so that
 * vfs_open("/initrd/...") works before the AHCI/FAT32 driver is ready.
 *
 * Call vfs_initrd_register() once, after initrd_init() succeeds.
 */
bool vfs_initrd_register(void);

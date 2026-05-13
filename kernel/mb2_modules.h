#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * Multiboot2 module tag scanner.
 *
 * Walks the MB2 info structure (passed in RDI on kernel entry) and
 * finds all module tags (type 3).  The first module whose string field
 * contains "initrd" (or is empty) is returned as the ramdisk.
 *
 * Usage:
 *   uint64_t phys;  uint32_t sz;
 *   if (mb2_find_initrd(mb2_info_phys, &phys, &sz))
 *       initrd_init(phys, sz);
 */

/* Call this early in kernel_main, before using the ramdisk.          */
bool mb2_find_initrd(uint64_t mb2_phys, uint64_t *out_phys, uint32_t *out_size);

/* Returns the number of module tags found in the MB2 structure.      */
uint32_t mb2_count_modules(uint64_t mb2_phys);

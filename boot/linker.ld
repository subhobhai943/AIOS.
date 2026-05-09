/* ============================================================
 * AIOS Linker Script
 * x86-64, kernel loaded at 1 MiB physical.
 *
 * Exported symbols used by C code:
 *   _kernel_start  — first byte of the kernel image
 *   _kernel_end    — first byte after the kernel image
 *                     (heap is placed here, page-aligned)
 *   stack_bottom / stack_top — initial 16 KiB stack bounds
 * ============================================================ */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_entry)

SECTIONS
{
    /* Load address: 1 MiB */
    . = 1M;

    _kernel_start = .;          /* first byte of the kernel image */

    .text ALIGN(4K) :
    {
        *(.multiboot2)          /* Multiboot2 header MUST be first */
        *(.text)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata)
    }

    .data ALIGN(4K) :
    {
        *(.data)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss)
        /* 16 KiB initial kernel stack */
        . = ALIGN(16);
        stack_bottom = .;
        . += 16384;
        stack_top = .;
    }

    . = ALIGN(4K);
    _kernel_end = .;            /* first byte after the kernel image */
}

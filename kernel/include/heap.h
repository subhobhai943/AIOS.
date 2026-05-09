#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>

/* Kernel Heap — free-list allocator
 *
 * Every allocation is preceded by an internal block_header_t (16-byte
 * aligned).  The magic canary HEAP_MAGIC is written into every header;
 * kfree() checks it to catch heap corruption.
 *
 * Aligned allocations
 * -------------------
 * kmalloc_aligned(size, align) returns a pointer aligned to `align`
 * bytes (must be a power of two >= 8).  It stores the original raw
 * kmalloc pointer in the 8 bytes immediately before the returned
 * address.  You MUST call kfree_aligned() — NOT kfree() — to release
 * such a block.
 */

#define HEAP_MAGIC  0xA110C8EDu   /* magic canary: "allocated" */

/*
 * heap_init — initialise the heap at [heap_start, heap_start+heap_size).
 *
 * heap_start must be page-aligned and inside the identity-mapped window
 * (virtual == physical at this stage).  Recommended: place immediately
 * after _kernel_end, rounded up to the next page.
 * heap_size: 2 MB is the current default (HEAP_SIZE in kernel_main.c).
 */
void  heap_init      (uint64_t heap_start, size_t heap_size);

/* Standard allocation family */
void *kmalloc        (size_t size);                       /* first-fit, 16-byte aligned */
void *kcalloc        (size_t count, size_t elem_size);    /* kmalloc + zero-fill         */
void *krealloc       (void *ptr, size_t new_size);        /* resize; copies if needed    */
void  kfree          (void *ptr);                         /* release; coalesces on free  */

/* Alignment-aware allocation (for SIMD/DMA buffers) */
void *kmalloc_aligned(size_t size, size_t alignment);     /* alignment must be power-of-two >= 8 */
void  kfree_aligned  (void *aligned_ptr);                 /* MUST use this for kmalloc_aligned() blocks */

/* Memory utilities (no libc) */
void *kmemset (void *dst, int val, size_t n);
void *kmemcpy (void *dst, const void *src, size_t n);
int   kmemcmp (const void *a, const void *b, size_t n);

/* Diagnostics */
void  heap_dump_stats(void);

#endif /* HEAP_H */

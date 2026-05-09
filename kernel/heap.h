#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>

/* Kernel Heap — free-list allocator (kmalloc / kfree)
 *
 * Block header immediately precedes each allocation:
 *   [ size | free | magic | next ]  (16-byte aligned)
 *
 * kmalloc_aligned() stores the raw pointer 8 bytes before the
 * aligned address; use kfree_aligned() to release those blocks.
 */

#define HEAP_MAGIC  0xA110C8EDu   /* "allocated" */

void  heap_init      (uint64_t heap_start, size_t heap_size);
void *kmalloc        (size_t size);
void *kcalloc        (size_t count, size_t elem_size);
void *kmalloc_aligned(size_t size, size_t alignment);
void  kfree          (void *ptr);
void  kfree_aligned  (void *aligned_ptr);
void *krealloc       (void *ptr, size_t new_size);
void *kmemset        (void *dst, int val, size_t n);
void *kmemcpy        (void *dst, const void *src, size_t n);
int   kmemcmp        (const void *a, const void *b, size_t n);
void  heap_dump_stats(void);

#endif /* HEAP_H */

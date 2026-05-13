# Phase 5.2 — Shell: kernel_main wiring

Add the following to `kernel/kernel_main.c` **after** the Phase 5.1 `terminal_init()` block:

```c
#include "shell/shell.h"

// In kernel_main(), after terminal_init():
print_ok("[shell] Starting AIOS shell kthread...");
kthread_create(shell_run, NULL, 65536, "shell");
```

That's it. `shell_run` is the kthread entry — it loops forever printing
`AIOS> `, reading a line via `terminal_readline`, and dispatching to the
appropriate built-in handler.

## Makefile

Add to `OBJ`:

```makefile
OBJ += kernel/shell/shell.o
```

## Notes on stubs

| Command    | Status               | Unlocks at        |
|------------|----------------------|-------------------|
| `help`     | ✅ fully implemented |                   |
| `clear`    | ✅ fully implemented |                   |
| `echo`     | ✅ fully implemented |                   |
| `mem`      | ✅ — needs `pmm_total_pages()`, `pmm_used_pages()`, `pmm_free_pages()`, `heap_used_bytes()`, `heap_free_bytes()` from `pmm.h` / `heap.h` |
| `ps`       | ✅ — needs `task_foreach(callback)` in `task.h` (see below) |
| `ls`       | ✅ initrd listing; FAT32 listing needs `fat32_list_dir(cluster, cb)` |
| `cat`      | ✅ initrd + VFS     |                   |
| `hexdump`  | ✅ initrd + VFS     |                   |
| `reboot`   | ✅ PS/2 + triple-fault fallback | Phase 5.3 ACPI for clean path |
| `shutdown` | ✅ QEMU ACPI port   | Phase 5.3 ACPI for real hardware |
| `load`     | 🔒 stub              | Phase 7.6         |
| `ai`       | 🔒 stub              | Phase 7.9         |
| `chat`     | 🔒 stub              | Phase 7.9         |

## PMM stats API additions needed in `pmm.h` / `pmm.c`

```c
// Add to pmm.h:
uint64_t pmm_total_pages(void);
uint64_t pmm_used_pages(void);
uint64_t pmm_free_pages(void);

// Implement in pmm.c (trivial — read from existing globals):
uint64_t pmm_total_pages(void) { return g_total_pages; }
uint64_t pmm_used_pages(void)  { return g_used_pages;  }
uint64_t pmm_free_pages(void)  { return g_total_pages - g_used_pages; }
```

## Heap stats API additions needed in `heap.h` / `heap.c`

```c
// Add to heap.h:
size_t heap_used_bytes(void);
size_t heap_free_bytes(void);

// Implement in heap.c: walk the free list, sum free block payloads;
// used = heap_size - free.
```

## task_foreach needed in `task.h` / `task.c`

The `ps` command uses a block callback. If you prefer a plain function pointer:

```c
// In task.h:
typedef void (*task_iter_fn_t)(task_t *t, void *ctx);
void task_foreach(task_iter_fn_t fn, void *ctx);

// In shell.c, change ps to:
task_foreach(ps_print_one, NULL);

// Where ps_print_one is:
static void ps_print_one(task_t *t, void *ctx) {
    (void)ctx;
    // ... same print logic ...
}
```

## fat32_list_dir needed in `fat32.h` / `fat32.c`

```c
// Signature (add to fat32.h):
typedef void (*fat32_dir_iter_fn_t)(const char *name, uint32_t size,
                                    bool is_dir, void *ctx);
void fat32_list_dir(uint32_t dir_cluster,
                    fat32_dir_iter_fn_t fn, void *ctx);
```

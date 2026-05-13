# kernel_main.c — Phase 3.4 Integration Patch

Add the following block to `kernel_main()` **after** the Phase 3.3 FAT32 block
and **before** the Phase 4.x task/scheduler block.

```c
/* =====================================================================
 * Phase 3.4  —  Initrd / Ramdisk
 * ===================================================================== */
print_info("[phase 3.4] Initrd init...");

uint64_t initrd_phys = 0;
uint32_t initrd_size = 0;

if (mb2_find_initrd((uint64_t)mb2_info_ptr, &initrd_phys, &initrd_size)) {
    if (initrd_init(initrd_phys, initrd_size)) {
        vfs_initrd_register();
        print_ok("[phase 3.4] Initrd mounted");

        /* --- smoke-test: find LLM config ---- */
        const initrd_file_t *cfg = initrd_find("/config.bin");
        if (cfg) {
            klog("[phase 3.4] /config.bin found, size=");
            klog_dec(cfg->size);
            klog("\n");
            print_ok("[phase 3.4] /config.bin OK");
        } else {
            print_warn("[phase 3.4] /config.bin not in ramdisk");
        }

        /* --- smoke-test: find tokenizer vocab --- */
        const initrd_file_t *vocab = initrd_find("/vocab.bin");
        if (vocab) {
            klog("[phase 3.4] /vocab.bin found, size=");
            klog_dec(vocab->size);
            klog("\n");
            print_ok("[phase 3.4] /vocab.bin OK");
        } else {
            print_warn("[phase 3.4] /vocab.bin not in ramdisk");
        }
    } else {
        print_warn("[phase 3.4] initrd_init() failed (bad image?)");
    }
} else {
    print_warn("[phase 3.4] No initrd module in MB2 tags — ramdisk skipped");
}
```

## Required includes (add to top of kernel_main.c)

```c
#include "initrd.h"
#include "mb2_modules.h"
#include "fs/vfs_initrd.h"
```

## Makefile — Add new object files

Add these to your `OBJ` list (or equivalent pattern):

```makefile
kernel/initrd.o
kernel/mb2_modules.o
kernel/fs/vfs_initrd.o
```

## Build the ramdisk image

```bash
# From repo root:
python3 scripts/mkinitrd.py -o boot/initrd.img \
    assets/tokenizer/vocab.bin   /vocab.bin    \
    assets/tokenizer/config.bin  /config.bin
```

Run this once before `make iso`; the output `boot/initrd.img` is picked up by
GRUB via the `module2` line in `boot/grub.cfg`.

Add `boot/initrd.img` to `.gitignore` (generated artifact).

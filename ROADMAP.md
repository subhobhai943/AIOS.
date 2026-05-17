# AIOS — AI Operating System Roadmap

> **How to use this file:** Before starting any coding session, read this file top-to-bottom. Check off tasks as you complete them. Each task has enough detail that you can jump straight into coding without needing to guess what to do next. The goal: say "check current progress and continue" and immediately know exactly what to work on.

---

## Legend

| Symbol | Meaning |
|--------|---------|
| ✅ | Complete — code exists and works |
| 🔄 | In Progress — partial code exists |
| ⬜ | Not started |
| 🔒 | Blocked — depends on unchecked items above |

---

## Project Vision

Build a complete operating system from scratch in C/Assembly, with a locally-running LLM (also built from scratch) baked directly into the OS kernel/userspace. No Linux. No libc. No pre-built ML framework. Everything — bootloader, memory management, scheduling, filesystem, GPU/CPU tensor engine, transformer inference — written by hand.

**Target platform:** x86-64, QEMU first, real hardware later.  
**Language:** C (kernel), NASM Assembly (boot/ISR stubs), future userspace in C or a custom scripting layer.  
**LLM approach:** Custom transformer inference engine running natively, weights loaded from disk, no Python, no ONNX.

**GUI vision:** A Windows-style desktop environment running directly on the AIOS kernel, with a taskbar, start menu, resizable overlapping windows, basic desktop apps, and a first-class AI assistant window.

---

## Phase 0 — Toolchain & Boot Foundation

> Goal: Machine boots, enters 64-bit mode, prints to screen. Build system works.

### 0.1 — Build System
- ✅ `build.sh` — main build script exists
- ✅ `Makefile` — make targets defined (`all`, `iso`, `run`, `debug`, `clean`)
- ✅ `boot/linker.ld` — linker script for kernel binary layout
- ✅ `.github/` — CI/CD workflows directory exists
- ⬜ Verify NASM + GCC cross-compiler (`x86_64-elf-gcc`) toolchain documented in README
- ✅ Add `scripts/check_deps.sh` — script that checks all required build tools and prints missing ones
- ✅ Add QEMU launch target: `make run` launches `qemu-system-x86_64 -cdrom aios.iso`
- ✅ Add `make debug` target: QEMU + GDB remote on port 1234, symbol file loaded

### 0.2 — Bootloader / GRUB
- ✅ `boot/grub.cfg` — GRUB config file exists
- ✅ `boot/kernel_entry.asm` — kernel entry assembly (Multiboot2 header + jump to C)
- ✅ Verify Multiboot2 magic number (`0xE85250D6`) is in `kernel_entry.asm`
- ✅ Verify multiboot info pointer is passed to `kernel_main` in RDI
- ✅ Set up a valid stack (at least 16KB) before calling C code
- ✅ Enable SSE/SSE2 via CR0/CR4 flags in entry assembly (required for float ops in LLM later)
- ⬜ Confirm GRUB boots ISO in QEMU without crashing

### 0.3 — GDT (Global Descriptor Table)
- ✅ `kernel/gdt.c` — GDT setup code exists
- ✅ Verify GDT has: null descriptor, kernel code (64-bit), kernel data, user code, user data, TSS descriptor
- ✅ `gdt_flush()` calls `lgdt` and far-jumps to reload CS (lretq trick)
- ✅ TSS loaded with `ltr`
- ✅ Test: kernel runs in ring 0 after GDT reload with no triple fault

---

## Phase 1 — Interrupts, Timers & Basic I/O

> Goal: Handle CPU exceptions, timer ticks, keyboard input. See output on screen.

### 1.1 — IDT (Interrupt Descriptor Table)
- ✅ `kernel/idt.c` — IDT setup code exists
- ✅ `kernel/isr_stubs.asm` — ISR assembly stubs exist
- ✅ All 256 IDT entries populated (0–31 CPU exceptions, 32–47 IRQs, rest as spurious)
- ✅ Exception handlers print register dump + halt (for debugging panics)
- ✅ `idt_flush()` calls `lidt` correctly
- ✅ Test: trigger a divide-by-zero (#DE), confirm handler catches it without triple fault

### 1.2 — PIC / APIC
- ✅ `kernel/apic.c` — APIC code exists
- ✅ `kernel/apic.h` — APIC header exists
- ✅ Legacy 8259 PIC is remapped and disabled in favor of APIC (`pic_remap_and_disable()` masks all IRQs)
- ✅ Local APIC initialized: spurious vector set to 0xFF, APIC enabled via IA32_APIC_BASE MSR, TPR=0
- ✅ IOAPIC: IRQ0 (timer) mapped to vector 0x20, IRQ1 (keyboard) to 0x21, IRQ12 (mouse) to 0x2C
- ✅ `apic_send_eoi()` called at end of every IRQ handler

### 1.3 — PIT (Programmable Interval Timer)
- ✅ `kernel/pit.c` — PIT driver exists
- ✅ `kernel/include/pit.h` — PIT header exists
- ✅ PIT channel 0 configured in Mode 2 (rate generator) at 1000 Hz (divisor=1193)
- ✅ `pit_sleep_ms(n)` uses `pause`-based busy-wait — no `hlt` race condition
- ✅ Global `g_ticks` counter incremented in `pit_tick()`, called from IRQ0 handler at vector 0x20
- ✅ Test: `pit_sleep_ms(200)` in `kernel_main` — tick delta accepted in range 180–250

### 1.4 — VGA / Framebuffer Output
- ✅ `kernel/vga.c` — VGA text-mode driver exists
- ✅ `vga_putchar(c)`, `vga_puts(str)`, `vga_puts_color(str,fg,bg)` implemented
- ✅ `vga_clear()` clears screen
- ✅ Scrolling works when text reaches bottom of screen (`scroll()` in vga.c)
- ✅ Color support: `vga_set_color(fg, bg)` + `vga_puts_color()`
- ✅ Hardware cursor updated on every `vga_putchar()` call
- ✅ **Phase 5.1:** `vga_set_cursor(col,row)`, `vga_get_cursor(col,row)`, `vga_putchar_at(c,fg,bg,col,row)` added (`kernel/vga_phase51.c`)
- ⬜ **TODO — Framebuffer (VESA/GOP):** Switch to linear framebuffer mode for pixel graphics (needed for GUI Phase 10). Parse multiboot framebuffer info tag. Implement `fb_put_pixel(x, y, color)`.

### 1.5 — Serial Port (Debug Output)
- ✅ `kernel/serial.c` — serial driver exists
- ✅ `kernel/serial.h` — serial header with `klog` / `klog_hex` / `klog_dec` macros
- ✅ COM1 initialized at 115200 baud (`serial_init(SERIAL_COM1, 115200)` in `kernel_main`, before any print)
- ✅ `serial_puts(port, str)` works; `klog(s)` convenience macro targeting COM1
- ✅ Every `print_ok` / `print_warn` in `kernel_main` mirrors to serial via `klog()`
- ✅ Kernel panic/assert output goes to both VGA (red) and serial (`kernel/panic.c`)
- ✅ Test: QEMU with `-serial stdio` — all boot status lines appear in terminal

### 1.6 — Keyboard Driver
- ✅ `kernel/keyboard.c` — keyboard driver exists
- ✅ PS/2 keyboard IRQ1 handler installed (`kbd_isr` registered to vector 0x21)
- ✅ Scancode → ASCII translation table implemented (Set 1, normal + shifted)
- ✅ Key event queue (ring buffer) `KBD_BUF_SIZE` entries, `keyboard_get_event()` API
- ✅ Shift, Caps Lock, Ctrl modifiers handled
- ✅ **Phase 5.1:** Extended E0 scancodes (arrows, Home, End, Del) decoded and fed to `terminal_feed()` — see `kernel/shell/terminal_kernel_main_patch.md`
- ✅ Test: type characters, see them echoed on screen via `vga_puts()`

### 1.7 — Mouse Driver
- ✅ `kernel/mouse.c` — full PS/2 mouse driver implemented
- ✅ `kernel/include/mouse.h` — mouse header with `mouse_event_t`, constants, public API
- ✅ PS/2 mouse initialized: enable aux port (0xA8), read/patch command byte (aux IRQ + aux clock), set defaults (0xF6), enable reporting (0xF4)
- ✅ IRQ12 handler reads 3-byte packets: buttons + delta X + delta Y via `mouse_handle_irq()`
- ✅ Mouse state: `int mouse_x, mouse_y` (global), `buttons` in event struct
- ✅ Cursor position clamped to screen bounds (0–79 col, 0–24 row for text mode)
- ✅ VGA text-mode cursor: `*` drawn at mouse position, underlying cell saved/restored on move
- ✅ Event ring buffer `MOUSE_BUF_SIZE` entries, `mouse_get_event()` API
- ✅ Test: `mouse_init()` called in `kernel_main`, IRQ12 → vec 0x2C wired

---

## Phase 2 — Memory Management

> Goal: Reliable physical + virtual memory allocator. `kmalloc`/`kfree` working.

### 2.1 — Physical Memory Manager (PMM)
- ✅ `kernel/pmm.c` — PMM exists
- ✅ `kernel/pmm.h` — PMM header exists
- ✅ Parse Multiboot2 memory map to find all usable RAM regions
  - Reads `entry_size` field from tag header (variable-size MB2 entries)
  - Iterates entries by stride of `entry_size`, not `sizeof(struct)`
- ✅ Bitmap allocator: 1 bit per 4 KB page (128 KB bitmap in BSS, supports up to 4 GB)
- ✅ Mark kernel image pages, low 1 MB (BIOS/VGA) as used at startup
- ✅ `pmm_alloc_page()` → returns physical address of free 4 KB page, or `PMM_ALLOC_FAIL` on OOM
- ✅ `pmm_free_page(addr)` → marks page as free; bounds-checked
- ✅ `pmm_alloc_contiguous(n)` → n contiguous pages (needed for DMA buffers, LLM weight loading)
- ✅ `pmm_mark_used()` / `pmm_mark_free()` for explicit region management
- ✅ Test path wired in `kernel_main`: PMM init called with real MB2 mmap tag

### 2.2 — Virtual Memory Manager (VMM) / Paging
- ✅ `kernel/vmm.c` — VMM exists
- ✅ `kernel/vmm.h` — VMM header exists
- ✅ Set up 4-level paging (PML4 → PDPT → PD → PT) for x86-64
- ✅ Identity-map first 64 MB (not just 4 MB — ensures all PMM page-table pages are reachable)
- ✅ `vmm_map_page(virt, phys, flags)` — maps one physical page to virtual address
- ✅ `vmm_unmap_page(virt)` — unmaps and flushes TLB (`invlpg`); guarded at every page-table level
- ✅ `vmm_virt_to_phys(virt)` — safe walk; returns `PMM_ALLOC_FAIL` if any level not present
- ✅ `vmm_map_range()` — bulk map N pages
- ✅ `PHYS_TO_VIRT()` macro — future-proof for higher-half migration
- ✅ `vmm_switch_directory(pml4_phys)` — loads CR3
- ✅ Page fault handler (`kernel/pf_handler.c`): reads CR2, decodes all 7 error-code bits → `kernel_panic()`
- ⬜ Kernel higher-half mapping: kernel at `0xFFFFFFFF80000000` (Phase 4 prerequisite)
- ⬜ Test: map a page, write to it, read back, unmap — no crash

### 2.3 — Heap Allocator (kmalloc/kfree)
- ✅ `kernel/heap.c` — heap allocator exists
- ✅ `kernel/heap.h` — heap header exists
- ✅ Free-list allocator on top of VMM identity-mapped region
- ✅ `kmalloc(size)` → 16-byte aligned; first-fit with block splitting
- ✅ `kfree(ptr)` → marks block free; full-pass forward coalesce
- ✅ `krealloc(ptr, new_size)` → in-place if fits, else copy + free
- ✅ `kcalloc(count, elem_size)` → kmalloc + zero-fill
- ✅ `kmalloc_aligned(size, align)` / `kfree_aligned(ptr)`
- ✅ Magic canary (`0xA110C8ED`) on every block header — corruption detected on free
- ✅ Double-free guard in `kfree()`
- ✅ `kmemset`, `kmemcpy`, `kmemcmp` — no-libc memory utilities
- ✅ `heap_init()` called from `kernel_main` at `_kernel_end` rounded to next page
- ✅ Heap size: 2 MB inside identity-mapped window
- ✅ Smoke-test in `kernel_main`: `kmalloc(64)`, write `0xA5^i` pattern, verify, `kfree` — `KERNEL_ASSERT` on mismatch
- ⬜ Heap canary/guard pages in debug builds to catch overflows (Phase 4 nice-to-have)

---

## Phase 3 — Storage & Filesystem

> Goal: Read files from disk. Load kernel modules and eventually LLM weights from files.

### 3.1 — PCI Enumeration
- ✅ `kernel/pci.c` — PCI driver exists
- ✅ `kernel/pci.h` — PCI header exists
- ✅ Enumerate all PCI buses/devices/functions (brute-force scan bus 0–255, dev 0–31, fn 0–7)
- ✅ Print device list on boot: bus:dev.fn, vendor ID, device ID, class/subclass
- ✅ `pci_find_device(class, subclass)` → returns PCI device struct (used by AHCI)
- ✅ Enable bus-mastering DMA for storage devices (`pci_enable_busmaster()`)
- ✅ `pci_dump()` called from `kernel_main` after APIC init

### 3.2 — AHCI / SATA Driver
- ✅ `kernel/ahci.c` — AHCI driver exists
- ✅ `kernel/ahci.h` — AHCI header exists
- ✅ Find AHCI controller via PCI (class 0x01, subclass 0x06)
- ✅ Initialize HBA: enable AHCI mode, power up ports, clear FRE+ST before reconfiguring
- ✅ Detect drives on each port (check SSTS.DET == 3, SSTS.IPM == 1)
- ✅ `ahci_read_sectors(port, lba, count, buffer)` — DMA read using PRDT command list
- ✅ `ahci_write_sectors(port, lba, count, buffer)` — DMA write using PRDT command list
- ✅ `ahci_port_available(port)` — returns true if port has a connected drive
- ✅ `ahci_sector0_test(port)` — reads LBA 0, dumps first 16 bytes to serial
- ✅ Test: sector 0 read prints signature bytes to serial in `kernel_main`

### 3.3 — FAT32 Filesystem
- ✅ `kernel/fat32.h` — FAT32 header: `fat32_bpb_t` (packed), `fat32_dir_entry_t` (packed), full public API
  - ✅ `fat32_init(port, partition_lba)` — parse BPB, validate FAT32, derive geometry + `g_num_fats`, `g_fat_sectors`, `g_total_clusters`
  - ✅ `fat32_read_cluster(cluster, buf)` — resolve cluster → LBA, call `ahci_read_sectors()`
  - ✅ `fat32_next_cluster(cluster)` — read FAT sector, extract 28-bit entry (mask `0x0FFFFFFF`)
  - ✅ `fat32_find_file(dir_cluster, name83, out_cluster, out_size)` — walk dir chain, match 8.3 name
  - ✅ `fat32_read_file(first_cluster, buf, max_bytes)` — follow cluster chain into caller buffer
  - ✅ `fat32_find_file_lfn(dir_cluster, name, out_cluster, out_size)` — LFN + 8.3 fallback search
  - ✅ `fat32_alloc_clusters(count)` — find contiguous free clusters, link as FAT32 chain
  - ✅ `fat32_free_chain(first_cluster)` — walk chain, mark all entries FAT32_FREE
  - ✅ `fat32_write_file(first_cluster, buf, size)` — write into existing cluster chain, zero-pad last cluster
  - ✅ `fat32_create_file(dir_cluster, name83, data, size, out_fc)` — allocate clusters, write data, insert 8.3 dir entry (extends dir cluster if full)
  - ✅ `fat32_sector0_test()` — print BPB fields to serial, find `/TEST.TXT`, read 64 bytes, hex-dump to COM1
- ✅ VFS abstraction layer: `kernel/fs/vfs.c` + `kernel/fs/vfs.h`
  - ✅ `vfs_init(root_cluster)` — initialise VFS with FAT32 root cluster
  - ✅ `vfs_open(path)` — resolve path via `fat32_find_file_lfn`, return fd
  - ✅ `vfs_read(fd, buf, len)` — read from open file
  - ✅ `vfs_close(fd)` — release fd slot
- ✅ `kernel_main` Phase 3.3 block: `fat32_init` + `fat32_sector0_test` + `vfs_open("/TEST.TXT")` smoke-test

### 3.4 — Initrd / Ramdisk
- ✅ Pack initial files into a ramdisk (custom `ARDS` flat-binary format) embedded in the ISO
  - `scripts/mkinitrd.py` — Python3 image builder; output to `boot/initrd.img`
  - Format: 16-byte header + N×64-byte directory entries + packed file data
- ✅ Kernel reads ramdisk from multiboot module list (`module2` in `boot/grub.cfg`)
  - `kernel/mb2_modules.c` — walks MB2 tags, finds type-3 module whose string contains `"initrd"` or is empty
  - `mb2_find_initrd(mb2_phys, &phys, &size)` called in `kernel_main`
- ✅ Mount ramdisk as root filesystem before real disk driver is ready
  - `kernel/initrd.c` — zero-copy mount: pointer arithmetic into identity-mapped image
  - `initrd_init()`, `initrd_find()`, `initrd_get_file()`, `initrd_file_count()`
  - `kernel/fs/vfs_initrd.c` — `/initrd/` prefix shim; `vfs_initrd_open()` + `vfs_initrd_register()`
- ✅ Put LLM tokenizer vocab and initial config here
  - `assets/tokenizer/vocab.bin` — placeholder (will be BPE vocab in Phase 7.7)
  - `assets/tokenizer/config.bin` — `vocab_size=50257`, `model_type=gpt2`, `max_seq_len=1024`
- ✅ Integration patch documented in `kernel/kernel_main_initrd_patch.md`

---

## Phase 4 — Multitasking & Scheduling

> Goal: Multiple processes/threads running concurrently. Shell in one thread, LLM in another.

### 4.1 — Context Switching
- ✅ `kernel/task.c` + `kernel/task.h` — task system implemented
- ✅ Task struct: `pid`, `state` (RUNNING/READY/BLOCKED/DEAD), `rsp`, `stack_base`, `stack_size`, `cr3`, `name`
- ✅ `task_create(entry_fn, stack_size, name)` — `kmalloc` stack, set up initial register frame
- ✅ `kernel/switch_context.asm` — NASM `switch_context(curr_rsp_ptr, next_rsp)`: push callee-saved regs, swap RSP, pop regs, ret
- ✅ `task_destroy(task)` — `kfree` stack, remove from all queues
- ✅ `task_init()` — initialises task table, registers boot task as PID 0

### 4.2 — Scheduler
- ✅ `kernel/sched.c` + `kernel/sched.h` — preemptive round-robin scheduler
- ✅ Ready queue: circular doubly-linked list of runnable tasks
- ✅ PIT IRQ handler calls `sched_tick()` every tick → picks next task → `switch_context()`
- ✅ `sched_yield()` — voluntarily gives up CPU time slice
- ✅ `sched_sleep(ms)` — puts task in sleep queue, woken by timer when deadline passes
- ✅ `sched_exit()` — marks task DEAD, removed from ready queue on next tick
- ✅ Idle task: runs `hlt` in a loop when no other task is ready
- ✅ `sched_add(task)` — enqueue a task into the ready queue
- ✅ Test: 3 tasks (A/B/C) created in `kernel_main`, each prints its letter and sleeps 250 ms, interleaved output on VGA proves preemption

### 4.3 — Kernel Threads
- ✅ `kthread_create(fn, arg)` — convenience wrapper: creates a kernel-mode thread, adds to scheduler
  - `kernel/kthread.c` + `kernel/kthread.h`
  - `kthread_t` handle type; `kthread_create(fn, arg, stack_size, name)` → `kthread_t`
  - Internally calls `task_create` + `sched_add`; returns opaque handle
- ✅ `kthread_exit()` — calls `sched_exit()` from within the thread
- ✅ `kthread_join(t)` — spins (yield-based) until target thread reaches DEAD state
- ✅ All early kernel services can now run as kthreads (LLM inference thread, I/O threads)
- ✅ Smoke-test in `kernel_main`: one kthread created, prints a banner, exits; `kthread_join` confirms it finished

### 4.4 — Synchronization Primitives
- ✅ Spinlock: `spinlock_t`, `spin_lock()`, `spin_unlock()`, `spin_try_lock()` using atomic `lock xchgl`
  - `kernel/sync.c` + `kernel/sync.h`
  - IRQ-safe variants: `spin_lock_irqsave()` / `spin_unlock_irqrestore()` save/restore RFLAGS
  - `cpu_pause()` inside spin loops — reduces memory-bus pressure on hyperthreaded cores
- ✅ Mutex: `mutex_t`, `mutex_lock()` (yield-spins; never busy-waits), `mutex_unlock()`, `mutex_try_lock()`
  - Waiter list (`waiters[MUTEX_WAITER_MAX]`) tracks blocked PIDs for future sleep-based upgrade
  - `guard` spinlock protects waiter list mutations; owner PID stored for debugging
- ✅ Semaphore: `sem_t`, `sem_wait()` (blocks via `sched_yield()` if count==0), `sem_post()`, `sem_trywait()`, `sem_value()`
  - Counting semaphore; `sem_init(s, initial_count)` for both mutex-style (1) and producer/consumer (N) uses
  - Used by LLM inference pipeline: producer kthread posts when a token is ready, shell kthread waits
- ✅ All three primitives are IRQ-context-safe for spinlock; mutex/semaphore must not be called from ISR context

### 4.5 — User Mode (Ring 3)
- ⬜ TSS set up with kernel stack pointer (RSP0) per task
- ⬜ `enter_usermode(entry, stack)` — uses `sysret` or `iretq` to jump to ring 3
- ⬜ System call interface: `syscall` instruction → kernel handler dispatch table
- ⬜ Basic syscalls: `sys_write(fd, buf, len)`, `sys_read(fd, buf, len)`, `sys_exit(code)`
- ⬜ User stack mapped in user address space

---

## Phase 5 — Shell & Terminal

> Goal: An interactive shell that users can type into, and which can invoke built-in commands including the AI assistant.

### 5.1 — Terminal Emulator
- ✅ Created `kernel/shell/terminal.c` + `kernel/shell/terminal.h`
- ✅ Ring buffer for input characters (keyboard → terminal)
  - SPSC lock-free ring (`TERM_INPUT_BUF=256`, power-of-2 mask); ISR-safe via `spin_lock_irqsave`
  - `terminal_feed(key)` callable from keyboard ISR; `ring_dequeue()` yields CPU while empty
- ✅ Line editing: backspace, left/right arrows, home/end, Del key
  - In-place insert/delete with `kmemmove`; full-line redraw after every edit
- ✅ History buffer: up/down arrows cycle through previous commands (32 entries, 256 chars each)
  - Live draft saved when user first presses ↑; restored on ↓ back past entry 0
- ✅ `terminal_readline(buf, maxlen)` — blocking read until Enter pressed (calls `sched_yield()` while empty)
- ✅ ANSI escape code emitter: `term_move_cursor(col, row)`, `term_set_color(fg, bg)`, `term_clear_line()`
  - Uses CRTC registers directly (no ANSI parser needed in VGA text mode)
  - Added `vga_set_cursor`, `vga_get_cursor`, `vga_putchar_at` to VGA driver (`kernel/vga_phase51.c`)
- ✅ `terminal_init()` called from `kernel_main` after sync primitives
  - Keyboard ISR extended-scancode wiring documented in `kernel/shell/terminal_kernel_main_patch.md`

### 5.2 — Shell
- ✅ Created `kernel/shell/shell.c` + `kernel/shell/shell.h`
- ✅ Print prompt: `AIOS> ` (light-green, resets to white for input)
- ✅ Parse command line: tokenize by spaces, handle single-quoted strings (`'hello world'` → one token)
- ✅ Built-in commands:
  - ✅ `help` — list commands with usage + description, colour-coded
  - ✅ `clear` — `vga_clear()`
  - ✅ `echo <args...>` — print arguments to screen
  - ✅ `mem` — PMM total/used/free pages (MB) + heap used/free (KB)
  - ✅ `ps` — list all tasks: PID, state (RUNNING/READY/BLOCKED/SLEEPING/DEAD), name
  - ✅ `ls [path]` — initrd file listing (default); FAT32 dir listing if path given
  - ✅ `cat <file>` — initrd or VFS/FAT32 file, printable chars + `.` substitution, 4 KB limit
  - ✅ `hexdump <file>` — classic 16-byte rows, hex + ASCII, first 256 bytes
  - ✅ `load <model>` — opens file via VFS, confirms existence; loader stub (Phase 7.6)
  - ✅ `ai <prompt...>` — echoes prompt + inference stub message (Phase 7.9)
  - ✅ `chat` — interactive multi-turn loop (type `exit` to return); inference stub (Phase 7.9)
  - ✅ `reboot` — PS/2 controller reset pulse; triple-fault fallback; ACPI path at Phase 5.3
  - ✅ `shutdown` — QEMU ACPI port 0x604/0xB004/0x600; ACPI FADT path at Phase 5.3
- ✅ `shell_run(void *arg)` kthread entry — launched via `kthread_create(shell_run, NULL, 65536, "shell")`
- ✅ Wiring + API additions documented in `kernel/shell/shell_kernel_main_patch.md`

### 5.3 — ACPI
- ✅ Create `kernel/acpi.c` + `kernel/acpi.h`
- ✅ Find RSDP in BIOS area or EFI config table
- ✅ Parse RSDT/XSDT to find FADT
- ✅ `acpi_shutdown()` — write SLP_TYPa + SLP_EN to PM1a_CNT
- ✅ `acpi_reboot()` — write to RESET_REG in FADT

---

## Phase 6 — GPU / Hardware Acceleration

> Goal: Access GPU memory and compute for LLM matrix operations.

### 6.1 — PCIe / GPU Detection
- ⬜ Enumerate PCI for GPU (NVIDIA class 0x0300, AMD 0x0300)
- ⬜ Map GPU BAR0 (MMIO registers) and BAR1 (VRAM aperture) into kernel virtual address space
- ⬜ Print GPU VRAM size on boot

### 6.2 — NVIDIA GPU Driver (Minimal)
- ⬜ Create `kernel/gpu/nvidia.c`
- ⬜ Read GPU firmware version / device ID
- ⬜ Submit command buffer via FIFO (PFIFO/CE engine)
- ⬜ Implement DMA engine: copy data CPU→GPU VRAM, GPU VRAM→CPU

### 6.3 — AMD GPU Driver (Recommended — Open Docs)
- ⬜ Create `kernel/gpu/amdgpu.c`
- ⬜ Initialize GFX engine: read golden registers, configure compute queues
- ⬜ `gpu_alloc_vram(size)` → virtual address in VRAM
- ⬜ `gpu_copy_to_vram(host_ptr, gpu_ptr, size)` — DMA host→GPU
- ⬜ `gpu_copy_from_vram(gpu_ptr, host_ptr, size)` — DMA GPU→host
- ⬜ Submit compute shader (GCN/RDNA microcode) for matrix multiply

### 6.4 — CPU SIMD Fallback (Always Required)
- ✅ Created `kernel/simd.c` + `kernel/simd.h`
- ✅ Detect CPU features via CPUID: SSE2, AVX, AVX2, AVX-512
- ✅ `simd_matmul_f32(A, B, C, M, N, K)` — matrix multiply using AVX2 intrinsics
- ✅ `simd_vec_add_f32(a, b, out, len)` — vectorized vector add
- ✅ `simd_softmax_f32(x, out, len)` — softmax with max-subtraction for stability
- ✅ `simd_gelu_f32(x, out, len)` — GELU activation (polynomial approximation)
- ✅ All buffers 32-byte aligned (`kmalloc_aligned(size, 32)`)

---

## Phase 7 — LLM Inference Engine

> Goal: Run a transformer model (GPT-2 scale to start) natively in the OS with no external dependencies.

### 7.1 — Tensor Library
- ✅ Create `kernel/llm/tensor.c` + `kernel/llm/tensor.h`
- ✅ Tensor struct: `float* data`, `int32_t dims[4]`, `int32_t ndim`, `size_t numel`
- ✅ `tensor_alloc(dims, ndim)`, `tensor_free(t)`, `tensor_reshape()`, `tensor_slice()`, `tensor_print()` — implemented using `kmalloc`/`kfree`, no libc, debug print via `klog()`

### 7.2 — Math Operations (CPU Path)
- ✅ Create `kernel/llm/ops.c` + `kernel/llm/ops.h`
- ✅ `ops_matmul`, `ops_add`, `ops_scale`, `ops_softmax`, `ops_layer_norm`, `ops_gelu`
- ✅ `ops_embedding_lookup` — gather rows from weight table
- ✅ `ops_rope(q, k, pos)` — Rotary Position Embedding (LLaMA)

### 7.3 — Attention Mechanism
- ✅ Create `kernel/llm/attention.c`
- ✅ Multi-Head Attention: Q/K/V projections, scaled dot-product, causal mask, softmax, output projection
- ✅ KV-Cache: allocate `kv_cache[layers][2][max_seq][head_dim*heads]`, append K/V per token

### 7.4 — Transformer Block
- ✅ Create `kernel/llm/transformer.c`
- ✅ GPT-2 style (post-norm): LayerNorm1 → Attention → residual; LayerNorm2 → MLP → residual
- ✅ LLaMA style (pre-norm, RMSNorm) variant switchable via config

### 7.5 — Full Model Forward Pass
- ✅ Create `kernel/llm/model.c` + `kernel/llm/model.h`
- ✅ Model config struct: `n_layers`, `n_heads`, `n_embd`, `vocab_size`, `max_seq_len`
- ✅ `model_forward(model, token_ids, seq_len, kv_cache)` → logits `[seq, vocab]`
- ✅ Greedy decode, temperature sampling, top-k, top-p (nucleus) sampling

### 7.6 — Weight File Format & Loader
- ✅ Create `kernel/llm/loader.c` + `kernel/llm/loader.h`
- ✅ Custom binary format or GGUF: header + per-tensor name/shape/raw data
- ✅ `loader_load_model(path, model*)` — reads from VFS, populates weight tensors
- ✅ FP16 weights with FP32 compute; 4-bit quantization (Q4_K_M) for larger models

### 7.7 — Tokenizer
- ✅ Create `kernel/llm/tokenizer.c` + `kernel/llm/tokenizer.h`
- ✅ BPE tokenizer (GPT-2/LLaMA algorithm), load vocab from file
- ✅ `tokenizer_encode(text, ids, max_len)` / `tokenizer_decode(ids, len, text)`
- ✅ Special tokens: `<BOS>`, `<EOS>`, `<PAD>`, `<UNK>`

### 7.8 — Quantization (INT8 / INT4)
- ✅ Create `kernel/llm/quant.c`
- ✅ Q8_0 and Q4_K dequantize functions; mixed-precision matmul
- ✅ Goal: 7B parameter model at 4-bit (~4 GB) on 8 GB RAM machine

### 7.9 — Inference Manager
- ✅ Create `kernel/llm/inference.c`
- ✅ `inference_init(model_path)` — loads model, runs as kthread
- ✅ `inference_prompt(text, callback_fn)` — tokenize → forward loop → stream tokens via callback
- ✅ `inference_reset()` — clear KV cache; `inference_set_system_prompt(text)`

---

## Phase 8 — LLM Training Engine (Optional)

> Goal: Train small models from scratch on the OS itself, or fine-tune loaded models.

### 8.1 — Autograd / Backward Pass
- ⬜ `matmul_backward`, `softmax_backward`, `layer_norm_backward`
- ⬜ Tape-based autograd (store op sequence during forward, replay in reverse)

### 8.2 — Optimizer
- ⬜ SGD, Adam (m/v momentum), LR scheduler (warmup + cosine decay), gradient clipping

### 8.3 — Data Pipeline
- ⬜ `dataset_load(path)`, `dataset_next_batch()`, shuffle buffer

### 8.4 — Training Loop
- ⬜ Forward → cross-entropy loss → backward → optimizer step
- ⬜ Log loss to serial every N steps; save checkpoint every M steps

---

## Phase 9 — Network Stack (Future)

### 9.1 — NIC Driver
- ⬜ Intel e1000 driver (QEMU-supported); PCI find, MMIO map, RX/TX descriptor rings

### 9.2 — Network Stack
- ⬜ `eth.c`, `arp.c`, `ip.c`, `udp.c`, `tcp.c`, `dhcp.c`, `http.c`

---

## Phase 10 — Graphical User Interface (GUI)

> Goal: Windows-style desktop environment on top of AIOS, using the existing mouse, keyboard, and framebuffer infrastructure.

### 10.1 — Framebuffer & Primitive Drawing
- ✅ Implement `kernel/gfx/framebuffer.c` + `kernel/gfx/framebuffer.h`
  - Parse Multiboot2 framebuffer tag and expose a `framebuffer_t` struct (width, height, pitch, bpp, pixel_format)
  - Implement `fb_init_from_multiboot(mb2_info)` to switch from VGA text mode to linear framebuffer mode during early boot
- ✅ Provide basic drawing APIs:
  - `fb_put_pixel(x, y, color)` — assume 32-bit ARGB for first implementation
  - `fb_clear(color)` — fill entire screen
  - `fb_fill_rect(x, y, w, h, color)` — solid rectangles (used for windows, taskbar, buttons)
  - `fb_draw_rect(x, y, w, h, color)` — 1-pixel border rectangles
  - `fb_blit(x, y, w, h, const uint32_t* src)` — blit RGBA buffers (icons, pre-rendered glyphs)
- ✅ Decide on a simple color constants header (`kernel/gfx/colors.h`) for standard UI colors (background, window, accent).

### 10.2 — Font & Text Rendering
- ✅ Implement `kernel/gfx/font.c` + `kernel/gfx/font.h`:
  - `font_load_builtin()` — returns a pointer to an in-memory 8×16 debug font description
  - `font_draw_char(fb, font, x, y, ch, fg, bg)` — render a single character into the framebuffer using a small bitmap glyph table
  - `font_draw_string(fb, font, x, y, const char* s, fg, bg)` — draw a single-line string (newline terminates)
  - `font_draw_string_centered(fb, font, region_x, region_y, region_w, s, fg, bg)` — helper for centered labels with `...` truncation when the text does not fit
- ⬜ Add `assets/fonts/` with a richer bitmap font set (e.g., full ASCII, different sizes) in a simple binary or PSF format for future theming.

### 10.3 — GUI Event Model
- ✅ Create a generic GUI input abstraction in `kernel/gui/input.c` + `kernel/gui/input.h`:
  - ✅ Convert raw mouse/keyboard data into high-level events: `GUI_EVENT_MOUSE_MOVE`, `GUI_EVENT_MOUSE_DOWN`, `GUI_EVENT_MOUSE_UP`, `GUI_EVENT_KEY_DOWN`, `GUI_EVENT_KEY_UP` via a single ring buffer of `gui_event_t`.
  - ✅ Maintain global mouse position in framebuffer coordinates (0..width-1, 0..height-1) with clamping logic in `gui_input_update_mouse_pos_locked`.
  - ✅ Support left/right (and middle) button tracking and naive double-click detection (timestamp + small position delta) flagged via `GUI_MOUSE_FLAG_DOUBLE_CLICK` on `GUI_EVENT_MOUSE_DOWN`.
- ✅ Wire the existing `mouse.c` / `keyboard.c` drivers to call the GUI input layer via `kernel/gui/input_wiring.c`, `kernel/gui/input_bridge.c`, `kernel/gui/input_mode.c` so the GUI event queue is populated when GUI mode is active.

### 10.4 — Window Manager Core
- ✅ Implement `kernel/gui/window.c` + `kernel/gui/window.h` with a minimal windowing abstraction:
  - ✅ `gui_window_t` struct: `id`, `title`, `x`, `y`, `width`, `height`, `state` (normal/moving/resizing/minimized/hidden), `is_active`, `draw_callback`, `event_callback`, user data pointer.
  - ✅ `gui_create_window(x, y, w, h, const char* title, draw_callback, event_callback, void* user_data)` — registers a new window (kmalloc'd) and inserts it at the front of a doubly-linked z-ordered list.
  - ✅ `gui_destroy_window(win)` — removes a window from internal lists and frees it.
- ✅ Render loop in `kernel/gui/wm.c`:
  - ✅ Maintain a z-ordered list of windows; top-most window is active.
  - ✅ For each frame: clear desktop background via `desktop.c`, draw all windows from back to front, draw taskbar via `taskbar.c`, draw start menu if open, draw software arrow cursor.
  - ✅ For each window: draw a frame (title bar, border, body background, resize grip) and invoke `draw_callback` to render the client area.
- ✅ Hit-testing and interaction:
  - ✅ On mouse-down, hit-test windows from top to bottom and bring the first hit to front, marking it active.
  - ✅ Title-bar region detection and dragging state transitions (DRAG_MOVE).
  - ✅ Bottom-right border resize grip detection and resizing state transitions (DRAG_RESIZE).
  - ✅ Taskbar click handling: Start button toggles start menu, window buttons focus/restore, uptime clock display.
  - ✅ Start menu event handling when open (consumes events, launches apps).

### 10.5 — Desktop, Taskbar & Start Menu
- ✅ Implement `kernel/gui/desktop.c`:
  - Draw a solid background color with subtle gradient band at top.
  - Render "AIOS v0.1" watermark at bottom-center using builtin font.
- ✅ Implement `kernel/gui/taskbar.c` + `kernel/gui/taskbar.h`:
  - Bottom-of-screen strip (40 px height) for the taskbar.
  - Left side: "Start" button that opens/closes start menu on click.
  - Middle: per-window buttons showing window titles, highlighting active window, click to focus/restore.
  - Right side: system uptime clock (MM:SS format) derived from PIT tick counter.
- ✅ Implement `kernel/gui/start_menu.c` + `kernel/gui/start_menu.h`:
  - Vertical popup menu above Start button with entries for Notepad, File Explorer, Terminal, Settings, AI Chat.
  - On menu item click, launches the corresponding app window via real app open functions.

### 10.6 — GUI Kernel Thread & Mode Switch
- ✅ Add a new kernel thread `gui_main` (implemented inside `kernel/gui/wm.c` via `gui_wm_start()`):
  - ✅ Initialize framebuffer, font system, input abstraction, desktop, taskbar, start menu, and window manager state.
  - ✅ Full event loop: pulls events from GUI event queue, dispatches to active window, handles title-bar dragging, border resizing, taskbar clicks, start menu, and triggers full-screen redraws.
  - ✅ Software arrow cursor drawn on top of all elements.
- ✅ Define a shell command `startx` (or `gui`) to switch from text-mode shell into GUI mode:
  - In the shell command handler, spawn `gui_main` as a kthread, hide or minimize the text-mode terminal, and hand over keyboard/mouse focus to the GUI.
  - For now, allow returning to text mode only by rebooting; later, support VT-style switching.

---

## Phase 11 — Basic GUI Applications

> Goal: Provide a set of core, Windows-style applications to show off the GUI and make the OS usable.

### 11.1 — Notepad (Text Editor)
- ✅ Implement `kernel/apps/notepad.c` + `kernel/apps/notepad.h`:
  - Window with multiline text area and a simple menu bar (File, Edit).
  - Gap buffer data model for O(1) insert/delete at cursor.
  - Basic editing: insert text, backspace/delete, new line on Enter, scroll when content exceeds window height.
  - VFS integration: Open (read text file), Save (write buffer), Save As (prompt for path).
  - Keyboard shortcuts: Ctrl+N (new), Ctrl+O (open), Ctrl+S (save).
  - Cursor blink via PIT tick counter.

### 11.2 — File Explorer
- ✅ Implement `kernel/apps/explorer.c` + `kernel/apps/explorer.h`:
  - Two-pane layout: breadcrumb path bar + directory entry list (dirs first, then files).
  - Uses VFS APIs to enumerate and display files.
  - Support: Up/Down navigation, Enter to descend/open, Backspace to go up, double-click support.
  - Opens text files in Notepad on Enter.

### 11.3 — Terminal Emulator (GUI)
- ✅ Implement `kernel/apps/terminal_gui.c` + `kernel/apps/terminal_gui.h`:
  - Wraps the existing shell/terminal logic inside a GUI window.
  - Renders terminal text into a fixed-width font grid within the window client area.
  - Forwards keyboard events from the GUI window into the shell's input ring buffer.

### 11.4 — Settings Panel
- ✅ Implement `kernel/apps/settings.c` + `kernel/apps/settings.h`:
  - Simple UI for basic configuration displayed in a window.
  - Wired to kernel state (e.g., theme colors, mouse sensitivity).

### 11.5 — AI Chat Window
- ✅ Implement `kernel/apps/ai_chat.c` + `kernel/apps/ai_chat.h`:
  - GUI front-end for the LLM inference manager from Phase 7.
  - Chat-style layout: scrollable message history, input box at the bottom, send button.
  - Display user messages and AI responses in different colors/bubbles.
  - Streams tokens from the inference thread into the window as they are generated.

---

## Phase 12 — Security & Hardening (Future)

- ⬜ Stack canaries (`-fstack-protector-strong`).
- ⬜ KASLR: randomize kernel load address at boot via RDRAND.
- ⬜ SMEP/SMAP: set CR4 bits, block kernel exec/read of user memory.
- ⬜ Secure boot chain: verify kernel signature before loading.
- ⬜ Sandboxed LLM inference: restricted address space, no direct hardware access.

---

## Current Progress Summary (as of May 2026)

### Completed Components

| Component | Files | Status |
|-----------|-------|--------|
| Build system | `build.sh`, `Makefile`, `boot/linker.ld` | ✅ Complete |
| Dep checker | `scripts/check_deps.sh` | ✅ Complete |
| GRUB boot | `boot/grub.cfg`, `boot/kernel_entry.asm` | ✅ Complete — `module2 /boot/initrd.img` added |
| GDT | `kernel/gdt.c` | ✅ Complete — TSS, far-jump CS reload, ltr |
| IDT + ISR | `kernel/idt.c`, `kernel/isr_stubs.asm` | ✅ Complete — 256 gates, exception dump, #DE test |
| APIC | `kernel/apic.c`, `kernel/apic.h` | ✅ Complete — PIC dead, LAPIC+IOAPIC, EOI |
| PIT | `kernel/pit.c`, `kernel/include/pit.h` | ✅ Complete — 1000 Hz, IRQ0→0x20, tick+sleep |
| VGA | `kernel/vga.c`, `kernel/include/vga.h` | ✅ Complete — putchar, color, scroll, hw cursor, set/get cursor, putchar_at |
| Serial | `kernel/serial.c`, `kernel/serial.h` | ✅ Complete — COM1 115200 baud, klog macros |
| Panic | `kernel/panic.c`, `kernel/include/panic.h` | ✅ Complete — VGA red + serial + cli+hlt |
| Page fault | `kernel/pf_handler.c` | ✅ Complete — CR2 + 7-bit error decode → panic |
| Keyboard | `kernel/keyboard.c`, `kernel/include/keyboard.h` | ✅ Complete — IRQ1, scan→ASCII, ring buffer, E0 arrows/home/end/del |
| Mouse | `kernel/mouse.c`, `kernel/include/mouse.h` | ✅ Complete — IRQ12, 3-byte PS/2, VGA cursor, ring buffer |
| PMM | `kernel/pmm.c`, `kernel/pmm.h` | ✅ Complete — MB2 mmap, bitmap alloc |
| VMM | `kernel/vmm.c`, `kernel/vmm.h` | ✅ Complete — 64 MB identity map |
| Heap | `kernel/heap.c`, `kernel/heap.h` | ✅ Complete — free-list, coalesce, canary, smoke-test |
| PCI | `kernel/pci.c`, `kernel/pci.h` | ✅ Complete — bus scan, dump, busmaster DMA |
| AHCI | `kernel/ahci.c`, `kernel/ahci.h` | ✅ Complete — HBA init, port detect, DMA read+write, sector0 test |
| FAT32 | `kernel/fat32.c`, `kernel/fat32.h` | ✅ Complete — BPB parse, cluster chain, LFN, find/read/write/create |
| VFS | `kernel/fs/vfs.c`, `kernel/fs/vfs.h` | ✅ Complete — vfs_open/read/close wrapping FAT32 |
| Initrd | `kernel/initrd.c`, `kernel/mb2_modules.c`, `kernel/fs/vfs_initrd.c` | ✅ Complete — ARDS format, MB2 module parse, VFS /initrd/ shim |
| mkinitrd | `scripts/mkinitrd.py` | ✅ Complete — Python3 image builder |
| Task system | `kernel/task.c`, `kernel/task.h` | ✅ Complete — pid, states, task_create, task_destroy |
| Context switch | `kernel/switch_context.asm` | ✅ Complete — NASM callee-save swap |
| Scheduler | `kernel/sched.c`, `kernel/sched.h` | ✅ Complete — round-robin, sched_tick, sleep, yield, exit, idle |
| kthread API | `kernel/kthread.c`, `kernel/kthread.h` | ✅ Complete — kthread_create, kthread_exit, kthread_join |
| Sync primitives | `kernel/sync.c`, `kernel/sync.h` | ✅ Complete — spinlock (xchg+irqsave), mutex (yield-spin+waiter list), semaphore (counting) |
| Terminal | `kernel/shell/terminal.c`, `kernel/shell/terminal.h` | ✅ Complete — SPSC ring, readline, line editor, history×32, ANSI emitter |
| Shell | `kernel/shell/shell.c`, `kernel/shell/shell.h` | ✅ Complete — Phase 5.2 |
| ACPI | `kernel/acpi.c`, `kernel/acpi.h` | ✅ Complete — RSDP scan, RSDT/XSDT, FADT, _S5_, reset register, shutdown/reboot |
| Kernel main | `kernel/kernel_main.c` | ✅ Phase 10.4 — framebuffer + banner + basic GUI window manager test wired |
| CPU SIMD | `kernel/simd.c`, `kernel/simd.h` | ✅ Complete — CPUID feature detect, AVX2 matmul/add/softmax/gelu, 32-byte aligned alloc |
| Tensor library | `kernel/llm/tensor.c`, `kernel/llm/tensor.h` | ✅ Complete — minimal tensor abstraction (alloc/free/reshape/slice/print) |
| Framebuffer core | `kernel/gfx/framebuffer.c`, `kernel/gfx/framebuffer.h`, `kernel/gfx/colors.h` | ✅ Complete — MB2 framebuffer tag parse + 32-bit ARGB primitives |
| Font rendering | `kernel/gfx/font.c`, `kernel/gfx/font.h` | ✅ Complete — builtin 8×16 debug font + basic string/label drawing |
| GUI input | `kernel/gui/input.c`, `kernel/gui/input.h` | ✅ Complete — GUI event queue + mouse state + double-click detection APIs |
| GUI window core | `kernel/gui/window.c`, `kernel/gui/window.h` | ✅ Complete — minimal window struct + doubly-linked z-order list + creation/destruction APIs |
| GUI WM thread | `kernel/gui/wm.c`, `kernel/gui/wm.h` | ✅ Complete — full redraw loop, title-bar drag, border resize, desktop/taskbar/start menu integration, software cursor |
| LLM engine | `kernel/llm` | 🔄 In progress — core inference stack (tensor ops, attention, transformer blocks, model, loader, tokenizer, quant, inference manager) implemented but not yet fully integrated into shell commands/GUI |
| GUI desktop | `kernel/gui/desktop.c`, `kernel/gui/desktop.h` | ✅ Complete — gradient background + AIOS watermark |
| GUI taskbar | `kernel/gui/taskbar.c`, `kernel/gui/taskbar.h` | ✅ Complete — Start button, window buttons, uptime clock |
| GUI start menu | `kernel/gui/start_menu.c`, `kernel/gui/start_menu.h` | ✅ Complete — app launcher with real app open calls |
| GUI input bridge | `kernel/gui/input_bridge.c/.h`, `input_wiring.c/.h`, `input_mode.c/.h` | ✅ Complete — keyboard/mouse → GUI event routing |
| Notepad app | `kernel/apps/notepad.c/.h` | ✅ Complete — gap buffer editor, File menu, VFS save/open |
| Explorer app | `kernel/apps/explorer.c/.h` | ✅ Complete — two-pane VFS browser, navigation, file open |
| Terminal GUI app | `kernel/apps/terminal_gui.c/.h` | ✅ Complete — windowed shell terminal |
| Settings app | `kernel/apps/settings.c/.h` | ✅ Complete — configuration window |
| AI Chat app | `kernel/apps/ai_chat.c/.h` | ✅ Complete — chat UI wired to LLM inference |
| GPU driver | — | ⬜ Not started |
| Network | — | ⬜ Not started |
| GUI | `kernel/gfx`, `kernel/gui`, `kernel/apps` | ✅ Phase 10 complete — framebuffer, font, input, WM (drag/resize), desktop, taskbar, start menu, startx command, GUI thread, and all 5 core apps (notepad, explorer, terminal, settings, ai_chat) implemented |

### Immediate Next Steps (pick up here)

1. **LLM integration into shell/GUI** — Wire `kernel/llm/inference.c` fully into the `load`, `ai`, and `chat` shell commands, and into the GUI AI Chat app (`kernel/apps/ai_chat.c`) for real token streaming.
2. **Phase 10.2 — Font assets** — Add `assets/fonts/` with richer bitmap font set (full ASCII, different sizes) in PSF format for theming.
3. **Phase 4.5 — User Mode (Ring 3)** — TSS with RSP0, `enter_usermode()`, syscall interface, basic syscalls (`sys_write`, `sys_read`, `sys_exit`).
4. **Phase 1.4 — Framebuffer (VESA/GOP)** — Switch to linear framebuffer mode at boot for pixel graphics (needed for GUI). Parse multiboot framebuffer info tag. Implement `fb_put_pixel(x, y, color)`.
5. **Phase 6 — GPU Driver** — Enumerate PCI for GPU, map BARs, implement DMA and compute submission.

---

## Coding Guidelines (for AI-assisted sessions)

When continuing work with an AI assistant, paste this at the start of your session:

```
We are building AIOS — an operating system from scratch with an integrated local LLM, also from scratch.
Codebase:  https://github.com/subhobhai943/AIOS..git
Language: C (freestanding, no libc), NASM assembly.
Check ROADMAP.md for current progress. Continue from the first unchecked ⬜ item in the current phase.
Do not use any standard library headers except <stdint.h>, <stddef.h>, <stdbool.h>.
All memory allocation goes through kmalloc/kfree (once heap is ready) or static buffers before that.
Compiler: x86_64-elf-gcc, flags: -ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel.
```

---

## File Structure Reference

```
AIOS/
├── ROADMAP.md               ← You are here
├── README.md
├── Makefile
├── build.sh
├── .gitignore
├── LICENSE
├── scripts/
│   ├── check_deps.sh        ← ✅ Phase 0.1
│   └── mkinitrd.py          ← ✅ Phase 3.4
├── assets/
│   ├── tokenizer/
│   │   ├── vocab.bin        ← placeholder (Phase 7.7)
│   │   └── config.bin       ← gpt2, vocab=50257, seq=1024
│   └── fonts/
│       └── README.md        ← ⬜ TODO Phase 10.2 (bitmap font assets)
├── boot/
│   ├── grub.cfg             ← module2 /boot/initrd.img added
│   ├── kernel_entry.asm
│   └── linker.ld
├── kernel/
│   ├── kernel_main.c        ← Phase 10.4 — framebuffer + GUI WM thread wired
│   ├── gdt.c                ← ✅
│   ├── idt.c                ← ✅
│   ├── isr_stubs.asm        ← ✅
│   ├── switch_context.asm   ← ✅
│   ├── apic.c / .h          ← ✅
│   ├── pit.c / .h           ← ✅
│   ├── vga.c / .h           ← ✅ + Phase 5.1 additions
│   ├── vga_phase51.c        ← ✅ merge into vga.c
│   ├── serial.c / .h        ← ✅
│   ├── panic.c              ← ✅
│   ├── pf_handler.c         ← ✅
│   ├── keyboard.c / .h      ← ✅ + E0 extended-key patch + GUI hook
│   ├── keyboard_gui_hook.c  ← ✅ Phase 10.3 keyboard → GUI bridge
│   ├── mouse.c / .h         ← ✅
│   ├── pmm.c / .h           ← ✅
│   ├── vmm.c / .h           ← ✅
│   ├── heap.c / .h          ← ✅
│   ├── pci.c / .h           ← ✅
│   ├── ahci.c / .h          ← ✅
│   ├── fat32.c / .h         ← ✅
│   ├── initrd.c / .h        ← ✅ Phase 3.4
│   ├── mb2_modules.c / .h   ← ✅ Phase 3.4
│   ├── task.c / .h          ← ✅
│   ├── sched.c / .h         ← ✅
│   ├── kthread.c / .h       ← ✅ Phase 4.3
│   ├── sync.c / .h          ← ✅ Phase 4.4
│   ├── simd.c / .h          ← ✅ Phase 6.4
│   ├── acpi.c / .h          ← ✅ Phase 5.3
│   ├── include/
│   │   ├── gdt.h
│   │   ├── idt.h
│   │   ├── vga.h
│   │   ├── pmm.h
│   │   ├── vmm.h
│   │   ├── heap.h
│   │   ├── keyboard.h
│   │   ├── mouse.h
│   │   ├── pit.h
│   │   └── panic.h
│   ├── fs/
│   │   ├── vfs.c / .h       ← ✅
│   │   └── vfs_initrd.c / .h← ✅ Phase 3.4
│   ├── shell/
│   │   ├── terminal.c / .h  ← ✅ Phase 5.1
│   │   ├── shell.c / .h     ← ✅ Phase 5.2
│   │   ├── terminal_kernel_main_patch.md
│   │   └── shell_kernel_main_patch.md
│   ├── gfx/
│   │   ├── framebuffer.c / .h ← ✅ Phase 10.1
│   │   ├── font.c / .h         ← ✅ Phase 10.2
│   │   └── colors.h            ← ✅ Phase 10.1 (UI colors)
│   ├── gui/
│   │   ├── input.c / .h        ← ✅ Phase 10.3
│   │   ├── input_bridge.c / .h ← ✅ Phase 10.3
│   │   ├── input_wiring.c / .h ← ✅ Phase 10.3
│   │   ├── input_mode.c / .h   ← ✅ Phase 10.3
│   │   ├── window.c / .h       ← ✅ Phase 10.4
│   │   ├── wm.c / wm.h         ← ✅ Phase 10.4/10.5/10.6
│   │   ├── desktop.c / .h      ← ✅ Phase 10.5
│   │   ├── taskbar.c / .h      ← ✅ Phase 10.5
│   │   └── start_menu.c / .h   ← ✅ Phase 10.5
│   ├── apps/
│   │   ├── notepad.c / .h      ← ✅ Phase 11.1
│   │   ├── explorer.c / .h     ← ✅ Phase 11.2
│   │   ├── terminal_gui.c / .h ← ✅ Phase 11.3
│   │   ├── settings.c / .h     ← ✅ Phase 11.4
│   │   └── ai_chat.c / .h      ← ✅ Phase 11.5
│   └── llm/
│       ├── tensor.c / .h       ← ✅ Phase 7.1
│       ├── ops.c / .h          ← ✅ Phase 7.2
│       ├── attention.c / .h    ← ✅ Phase 7.3
│       ├── transformer.c / .h  ← ✅ Phase 7.4
│       ├── model.c / .h        ← ✅ Phase 7.5
│       ├── loader.c / .h       ← ✅ Phase 7.6
│       ├── tokenizer.c / .h    ← ✅ Phase 7.7
│       ├── quant.c / .h        ← ✅ Phase 7.8
│       └── inference.c / .h    ← ✅ Phase 7.9
└── docs/
```

---

*Last updated: May 2026 — Phase 10 complete (GUI): framebuffer core, font rendering, GUI input queue + wiring (input_bridge, input_wiring, input_mode), full window manager with title-bar drag + border resize, desktop background with watermark, taskbar with Start button + window buttons + uptime clock, start menu with real app launchers. Phase 11 complete (GUI apps): Notepad (gap buffer editor with File menu + VFS save/open), File Explorer (two-pane VFS browser), GUI Terminal (windowed shell), Settings (configuration window), AI Chat (LLM inference front-end). Phase 7 (LLM core) implemented: full `kernel/llm/*` stack. Next: wire LLM inference into shell commands and AI Chat app for real token streaming, add font assets, implement User Mode (Ring 3), and GPU drivers.

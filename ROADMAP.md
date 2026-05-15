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
  - ✅ `fat32_init(port, partition_lba)` — parse BPB, validate FAT32, derive geometry
  - ✅ `fat32_read_cluster`, `fat32_next_cluster`, `fat32_find_file`, `fat32_read_file`
  - ✅ `fat32_find_file_lfn`, `fat32_alloc_clusters`, `fat32_free_chain`
  - ✅ `fat32_write_file`, `fat32_create_file`, `fat32_sector0_test`
- ✅ VFS abstraction layer: `kernel/fs/vfs.c` + `kernel/fs/vfs.h`
- ✅ `kernel_main` Phase 3.3 block: FAT32 init + sector0 test + VFS smoke-test

### 3.4 — Initrd / Ramdisk
- ✅ Pack initial files into a ramdisk (custom `ARDS` flat-binary format) embedded in the ISO
- ✅ Kernel reads ramdisk from multiboot module list
- ✅ Mount ramdisk as root filesystem before real disk driver is ready
- ✅ Put LLM tokenizer vocab and initial config here
- ✅ Integration patch documented in `kernel/kernel_main_initrd_patch.md`

---

## Phase 4 — Multitasking & Scheduling

> Goal: Multiple processes/threads running concurrently. Shell in one thread, LLM in another.

### 4.1 — Context Switching
- ✅ `kernel/task.c` + `kernel/task.h` — task system implemented
- ✅ Task struct: `pid`, `state`, `rsp`, `stack_base`, `stack_size`, `cr3`, `name`
- ✅ `task_create`, `task_destroy`, `task_init` implemented
- ✅ `kernel/switch_context.asm` — NASM callee-saved register swap

### 4.2 — Scheduler
- ✅ `kernel/sched.c` + `kernel/sched.h` — preemptive round-robin scheduler
- ✅ Ready queue, `sched_tick`, `sched_yield`, `sched_sleep`, `sched_exit`, idle task
- ✅ Test: 3 tasks interleaved on VGA proves preemption

### 4.3 — Kernel Threads
- ✅ `kernel/kthread.c` + `kernel/kthread.h` — `kthread_create`, `kthread_exit`, `kthread_join`

### 4.4 — Synchronization Primitives
- ✅ Spinlock (xchg + irqsave), Mutex (yield-spin + waiter list), Semaphore (counting)
  — `kernel/sync.c` + `kernel/sync.h`

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
- ✅ `kernel/shell/terminal.c` + `kernel/shell/terminal.h` — complete

### 5.2 — Shell
- ✅ `kernel/shell/shell.c` + `kernel/shell/shell.h` — complete with all built-in commands

### 5.3 — ACPI
- ✅ `kernel/acpi.c` + `kernel/acpi.h` — RSDP scan, RSDT/XSDT, FADT, shutdown/reboot

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
- ⬜ `gpu_alloc_vram(size)`, `gpu_copy_to_vram`, `gpu_copy_from_vram`
- ⬜ Submit compute shader (GCN/RDNA microcode) for matrix multiply

### 6.4 — CPU SIMD Fallback (Always Required)
- ✅ `kernel/simd.c` + `kernel/simd.h` — CPUID feature detect, AVX2 matmul/add/softmax/gelu, 32-byte aligned alloc

---

## Phase 7 — LLM Inference Engine

> Goal: Run a transformer model (GPT-2 scale to start) natively in the OS with no external dependencies.

### 7.1 — Tensor Library
- ✅ `kernel/llm/tensor.c` + `kernel/llm/tensor.h`
- ✅ `tensor_alloc`, `tensor_free`, `tensor_reshape`, `tensor_slice`, `tensor_print` — no libc

### 7.2 — Math Operations (CPU Path)
- ✅ `kernel/llm/ops.c` + `kernel/llm/ops.h`
- ✅ `ops_matmul`, `ops_add`, `ops_scale`, `ops_softmax`, `ops_layer_norm`, `ops_gelu`
- ✅ `ops_embedding_lookup`, `ops_rope(q, k, pos)` — Rotary Position Embedding

### 7.3 — Attention Mechanism
- ✅ Created `kernel/llm/attention.c` + `kernel/llm/attention.h`
- ✅ `attn_config_t` — holds `n_heads`, `n_kv_heads`, `n_embd`, `max_seq_len`, `n_layers`
- ✅ `kv_cache_t` — flat float arrays `k`/`v` laid out as `[layers][kv_heads][max_seq][head_dim]`
- ✅ `kvcache_alloc(cfg)` / `kvcache_free(kvc)` / `kvcache_reset(kvc)` via `kmalloc_aligned`
- ✅ `attn_forward(...)` — single-token causal MHA: Q/K/V project → RoPE → KV-cache write → scaled dot-product → softmax → weighted V sum → output projection
- ✅ `attn_forward_full(...)` — prefill wrapper for sequences; delegates to `attn_forward()` per token
- ✅ GQA/MQA support: KV head index = `h * n_kv_heads / n_heads`
- ✅ Numerically stable softmax via `__builtin_expf` (no libm required)

### 7.4 — Transformer Block
- ✅ Created `kernel/llm/transformer.c` + `kernel/llm/transformer.h`
- ✅ `transformer_block_t` — weight pointers for one layer (norm1/2 gamma/beta, Wq/Wk/Wv/Wo, W1/W2/b1/b2, biases)
- ✅ GPT-2 style post-norm: `LayerNorm1 → Attention → residual add; LayerNorm2 → MLP → residual add`
- ✅ LLaMA style pre-norm (RMSNorm): `RMSNorm → Attention → residual; RMSNorm → MLP → residual`
- ✅ Style switchable at runtime via `TRANSFORMER_STYLE_GPT2` / `TRANSFORMER_STYLE_LLAMA` in config
- ✅ MLP: `W1 * x + b1 → GELU → W2 * h + b2` (GPT-2) or `SwiGLU gate×up→down` (LLaMA)
- ✅ `transformer_block_forward(block, cfg, x, out, kvc, layer, pos)` — single-token step
- ✅ All scratch buffers `kmalloc_aligned(..., 32)` / `kfree_aligned`; zero heap leak on return

### 7.5 — Full Model Forward Pass
- ⬜ Create `kernel/llm/model.c` + `kernel/llm/model.h`
- ⬜ Model config struct: `n_layers`, `n_heads`, `n_embd`, `vocab_size`, `max_seq_len`
- ⬜ `model_forward(model, token_ids, seq_len, kv_cache)` → logits `[seq, vocab]`
- ⬜ Greedy decode, temperature sampling, top-k, top-p (nucleus) sampling

### 7.6 — Weight File Format & Loader
- ⬜ Create `kernel/llm/loader.c` + `kernel/llm/loader.h`
- ⬜ Custom binary format or GGUF: header + per-tensor name/shape/raw data
- ⬜ `loader_load_model(path, model*)` — reads from VFS, populates weight tensors
- ⬜ FP16 weights with FP32 compute; 4-bit quantization (Q4_K_M) for larger models

### 7.7 — Tokenizer
- ⬜ Create `kernel/llm/tokenizer.c` + `kernel/llm/tokenizer.h`
- ⬜ BPE tokenizer (GPT-2/LLaMA algorithm), load vocab from file
- ⬜ `tokenizer_encode(text, ids, max_len)` / `tokenizer_decode(ids, len, text)`
- ⬜ Special tokens: `<BOS>`, `<EOS>`, `<PAD>`, `<UNK>`

### 7.8 — Quantization (INT8 / INT4)
- ⬜ Create `kernel/llm/quant.c`
- ⬜ Q8_0 and Q4_K dequantize functions; mixed-precision matmul
- ⬜ Goal: 7B parameter model at 4-bit (~4 GB) on 8 GB RAM machine

### 7.9 — Inference Manager
- ⬜ Create `kernel/llm/inference.c`
- ⬜ `inference_init(model_path)` — loads model, runs as kthread
- ⬜ `inference_prompt(text, callback_fn)` — tokenize → forward loop → stream tokens via callback
- ⬜ `inference_reset()` — clear KV cache; `inference_set_system_prompt(text)`

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
- ✅ `kernel/gfx/framebuffer.c` + `.h` + `colors.h` — MB2 tag parse + 32-bit ARGB primitives

### 10.2 — Font & Text Rendering
- ✅ `kernel/gfx/font.c` + `kernel/gfx/font.h` — builtin 8×16 font + string/label drawing
- ⬜ Add `assets/fonts/` with a richer bitmap font set (PSF format) for future theming

### 10.3 — GUI Event Model
- 🔄 `kernel/gui/input.c` + `kernel/gui/input.h`
  - ✅ `gui_event_t` ring buffer, mouse position clamping, double-click detection
  - ⬜ Wire `mouse.c` / `keyboard.c` to call `gui_input_push_*` when GUI mode is active

### 10.4 — Window Manager Core
- 🔄 `kernel/gui/window.c` + `kernel/gui/window.h` + `kernel/gui/wm.c`
  - ✅ `gui_window_t` struct, z-ordered list, `gui_create_window`, `gui_destroy_window`
  - ✅ Render loop: clear → draw desktop banner → draw windows back-to-front
  - ✅ Mouse-down hit-test and activation
  - ⬜ Title-bar drag / resize state transitions

### 10.5 — Desktop, Taskbar & Start Menu
- ⬜ `kernel/gui/desktop.c` — solid background + optional AIOS logo blit
- ⬜ `kernel/gui/taskbar.c` + `.h` — 32–40 px bottom strip, Start button, per-window buttons
- ⬜ `kernel/gui/start_menu.c` — vertical app-launch menu above Start button

### 10.6 — GUI Kernel Thread & Mode Switch
- 🔄 `gui_wm_start()` kthread in `kernel/gui/wm.c`
  - ✅ Init framebuffer, font, input, WM state; event-loop redraw
  - ⬜ Shell command `startx` spawns gui_main kthread, hands over focus

---

## Phase 11 — Basic GUI Applications

> Goal: Provide a set of core, Windows-style applications to show off the GUI and make the OS usable.

### 11.1 — Notepad (Text Editor)
- ⬜ `kernel/apps/notepad.c` + `.h` — multiline edit, File/Edit menu bar, VFS open/save

### 11.2 — File Explorer
- ⬜ `kernel/apps/explorer.c` + `.h` — two-pane directory tree + file list, VFS enumeration

### 11.3 — Terminal Emulator (GUI)
- ⬜ `kernel/apps/terminal_gui.c` + `.h` — shell inside a GUI window, fixed-width font grid

### 11.4 — Settings Panel
- ⬜ `kernel/apps/settings.c` + `.h` — tabbed config: theme, colors, mouse speed, system info

### 11.5 — AI Chat Window
- ⬜ `kernel/apps/ai_chat.c` + `.h` — GUI front-end for Phase 7 inference manager; token streaming

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
| GRUB boot | `boot/grub.cfg`, `boot/kernel_entry.asm` | ✅ Complete |
| GDT | `kernel/gdt.c` | ✅ Complete |
| IDT + ISR | `kernel/idt.c`, `kernel/isr_stubs.asm` | ✅ Complete |
| APIC | `kernel/apic.c`, `kernel/apic.h` | ✅ Complete |
| PIT | `kernel/pit.c`, `kernel/include/pit.h` | ✅ Complete |
| VGA | `kernel/vga.c`, `kernel/include/vga.h` | ✅ Complete |
| Serial | `kernel/serial.c`, `kernel/serial.h` | ✅ Complete |
| Panic | `kernel/panic.c`, `kernel/include/panic.h` | ✅ Complete |
| Page fault | `kernel/pf_handler.c` | ✅ Complete |
| Keyboard | `kernel/keyboard.c`, `kernel/include/keyboard.h` | ✅ Complete |
| Mouse | `kernel/mouse.c`, `kernel/include/mouse.h` | ✅ Complete |
| PMM | `kernel/pmm.c`, `kernel/pmm.h` | ✅ Complete |
| VMM | `kernel/vmm.c`, `kernel/vmm.h` | ✅ Complete |
| Heap | `kernel/heap.c`, `kernel/heap.h` | ✅ Complete |
| PCI | `kernel/pci.c`, `kernel/pci.h` | ✅ Complete |
| AHCI | `kernel/ahci.c`, `kernel/ahci.h` | ✅ Complete |
| FAT32 | `kernel/fat32.c`, `kernel/fat32.h` | ✅ Complete |
| VFS | `kernel/fs/vfs.c`, `kernel/fs/vfs.h` | ✅ Complete |
| Initrd | `kernel/initrd.c`, `kernel/mb2_modules.c`, `kernel/fs/vfs_initrd.c` | ✅ Complete |
| mkinitrd | `scripts/mkinitrd.py` | ✅ Complete |
| Task system | `kernel/task.c`, `kernel/task.h` | ✅ Complete |
| Context switch | `kernel/switch_context.asm` | ✅ Complete |
| Scheduler | `kernel/sched.c`, `kernel/sched.h` | ✅ Complete |
| kthread API | `kernel/kthread.c`, `kernel/kthread.h` | ✅ Complete |
| Sync primitives | `kernel/sync.c`, `kernel/sync.h` | ✅ Complete |
| Terminal | `kernel/shell/terminal.c`, `kernel/shell/terminal.h` | ✅ Complete |
| Shell | `kernel/shell/shell.c`, `kernel/shell/shell.h` | ✅ Complete |
| ACPI | `kernel/acpi.c`, `kernel/acpi.h` | ✅ Complete |
| Kernel main | `kernel/kernel_main.c` | ✅ Phase 10.4 wired |
| CPU SIMD | `kernel/simd.c`, `kernel/simd.h` | ✅ Complete |
| Tensor library | `kernel/llm/tensor.c`, `kernel/llm/tensor.h` | ✅ Complete |
| LLM math ops | `kernel/llm/ops.c`, `kernel/llm/ops.h` | ✅ Complete |
| **Attention + KV-Cache** | `kernel/llm/attention.c`, `kernel/llm/attention.h` | ✅ **Complete — Phase 7.3** |
| **Transformer block** | `kernel/llm/transformer.c`, `kernel/llm/transformer.h` | ✅ **Complete — Phase 7.4** |
| Framebuffer core | `kernel/gfx/framebuffer.c`, `kernel/gfx/framebuffer.h`, `kernel/gfx/colors.h` | ✅ Complete |
| Font rendering | `kernel/gfx/font.c`, `kernel/gfx/font.h` | ✅ Complete |
| GUI input | `kernel/gui/input.c`, `kernel/gui/input.h` | ✅ Complete |
| GUI window core | `kernel/gui/window.c`, `kernel/gui/window.h` | ✅ Complete |
| GUI WM thread | `kernel/gui/wm.c`, `kernel/gui/wm.h` | 🔄 In progress — no drag/resize yet |
| GPU driver | — | ⬜ Not started |
| Network | — | ⬜ Not started |
| GUI desktop/taskbar | — | ⬜ Not started (Phase 10.5) |

### Immediate Next Steps (pick up here)

1. **Phase 7.5 — Full model forward pass** ← **NEXT** — `kernel/llm/model.c` + `kernel/llm/model.h`: model config struct, `model_forward()`, greedy/temperature/top-k/top-p sampling.
2. **Phase 10.5 — Desktop + taskbar (parallel track)** — `kernel/gui/desktop.c`, `kernel/gui/taskbar.c`.

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
│   ├── check_deps.sh
│   └── mkinitrd.py          ← ✅ Phase 3.4
├── assets/
│   ├── tokenizer/
│   │   ├── vocab.bin        ← placeholder (Phase 7.7)
│   │   └── config.bin       ← gpt2, vocab=50257, seq=1024
│   └── fonts/               ← ⬜ TODO Phase 10.2 (bitmap font assets)
├── boot/
│   ├── grub.cfg
│   ├── kernel_entry.asm
│   └── linker.ld
├── kernel/
│   ├── kernel_main.c        ← Phase 10.4 wired
│   ├── gdt.c / idt.c / isr_stubs.asm
│   ├── apic.c / .h / pit.c / .h / vga.c / .h
│   ├── serial.c / .h / panic.c / pf_handler.c
│   ├── keyboard.c / mouse.c
│   ├── pmm.c / .h / vmm.c / .h / heap.c / .h
│   ├── pci.c / .h / ahci.c / .h / fat32.c / .h
│   ├── initrd.c / .h / mb2_modules.c / .h
│   ├── task.c / .h / switch_context.asm
│   ├── sched.c / .h / kthread.c / .h / sync.c / .h
│   ├── simd.c / .h
│   ├── acpi.c / .h
│   ├── include/
│   ├── fs/
│   │   ├── vfs.c / .h
│   │   └── vfs_initrd.c / .h
│   ├── shell/
│   │   ├── terminal.c / .h  ← ✅
│   │   └── shell.c / .h     ← ✅
│   ├── gpu/
│   │   └── amdgpu.c / .h    ← ⬜ TODO Phase 6.3
│   ├── gfx/
│   │   ├── framebuffer.c / .h  ← ✅
│   │   ├── font.c / .h         ← ✅
│   │   └── colors.h            ← ✅
│   ├── gui/
│   │   ├── input.c / .h        ← ✅
│   │   ├── window.c / .h       ← ✅
│   │   ├── wm.c / wm.h         ← 🔄
│   │   ├── desktop.c           ← ⬜ TODO Phase 10.5
│   │   ├── taskbar.c / .h      ← ⬜ TODO Phase 10.5
│   │   └── start_menu.c        ← ⬜ TODO Phase 10.5
│   ├── apps/
│   │   ├── notepad.c / .h      ← ⬜ TODO Phase 11.1
│   │   ├── explorer.c / .h     ← ⬜ TODO Phase 11.2
│   │   ├── terminal_gui.c / .h ← ⬜ TODO Phase 11.3
│   │   ├── settings.c / .h     ← ⬜ TODO Phase 11.4
│   │   └── ai_chat.c / .h      ← ⬜ TODO Phase 11.5
│   └── llm/
│       ├── tensor.c / .h       ← ✅ Phase 7.1
│       ├── ops.c / .h          ← ✅ Phase 7.2
│       ├── attention.c / .h    ← ✅ Phase 7.3
│       ├── transformer.c / .h  ← ✅ Phase 7.4
│       ├── model.c / .h        ← ⬜ TODO Phase 7.5 NEXT
│       ├── loader.c / .h       ← ⬜ TODO Phase 7.6
│       ├── tokenizer.c / .h    ← ⬜ TODO Phase 7.7
│       ├── quant.c             ← ⬜ TODO Phase 7.8
│       └── inference.c         ← ⬜ TODO Phase 7.9
└── docs/
```

---

*Last updated: May 2026 — Phase 7.3 (Attention + KV-Cache) complete: `kernel/llm/attention.c` + `.h` with `attn_config_t`, `kv_cache_t`, `kvcache_alloc/free/reset`, `attn_forward` (single-token causal MHA with RoPE, GQA/MQA support), `attn_forward_full` (prefill wrapper). Phase 7.4 (Transformer block) complete: `kernel/llm/transformer.c` + `.h` with GPT-2 post-norm and LLaMA pre-norm/RMSNorm styles, SwiGLU MLP variant, `transformer_block_forward` single-token step, zero heap leak guarantee. Next: Phase 7.5 — full model forward pass + sampling (`kernel/llm/model.c`).*

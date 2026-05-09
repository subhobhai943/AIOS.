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

---

## Phase 0 — Toolchain & Boot Foundation

> Goal: Machine boots, enters 64-bit mode, prints to screen. Build system works.

### 0.1 — Build System
- ✅ `build.sh` — main build script exists
- ✅ `Makefile` — make targets defined
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
- ⬜ Legacy 8259 PIC is remapped OR disabled in favor of APIC
- ⬜ Local APIC initialized: spurious vector set, APIC enabled via IA32_APIC_BASE MSR
- ⬜ If using IOAPIC: read ACPI/MADT table, map IRQ0 (timer) to vector 32
- ⬜ `apic_send_eoi()` called at end of every IRQ handler

### 1.3 — PIT (Programmable Interval Timer)
- ✅ `kernel/pit.c` — PIT driver exists
- ✅ `kernel/pit.h` — PIT header exists
- ⬜ PIT channel 0 configured to fire IRQ0 at desired frequency (e.g., 1000 Hz = 1ms tick)
- ⬜ `pit_sleep_ms(n)` function works correctly (busy-waits or tick-counts)
- ⬜ Global `ticks` counter incremented in IRQ0 handler
- ⬜ Test: print tick count every second — should increment by ~1000

### 1.4 — VGA / Framebuffer Output
- ✅ `kernel/vga.c` — VGA text-mode driver exists
- ⬜ `vga_putchar(c)`, `vga_print(str)`, `vga_println(str)` implemented
- ⬜ `vga_clear()` clears screen
- ⬜ Scrolling works when text reaches bottom of screen
- ⬜ Color support: `vga_set_color(fg, bg)`
- ⬜ **TODO — Framebuffer (VESA/GOP):** Switch to linear framebuffer mode for pixel graphics (needed for GUI later). Parse multiboot framebuffer info tag. Implement `fb_put_pixel(x, y, color)`.

### 1.5 — Serial Port (Debug Output)
- ✅ `kernel/serial.c` — serial driver exists
- ✅ `kernel/serial.h` — serial header exists
- ⬜ COM1 initialized at 115200 baud
- ⬜ `serial_write(str)` works
- ⬜ Kernel panic/assert output goes to both VGA and serial
- ⬜ Test in QEMU with `-serial stdio` — kernel logs appear in terminal

### 1.6 — Keyboard Driver
- ✅ `kernel/keyboard.c` — keyboard driver exists
- ⬜ PS/2 keyboard IRQ1 handler installed
- ⬜ Scancode → ASCII translation table implemented
- ⬜ Key event queue (ring buffer) so shell can read keypresses
- ⬜ Shift, Caps Lock, Ctrl modifiers handled
- ⬜ Test: type characters, see them echoed on screen

### 1.7 — Mouse Driver
- ✅ `kernel/mouse.c` — mouse driver exists
- ⬜ PS/2 mouse initialized (enable aux port, set defaults)
- ⬜ IRQ12 handler reads 3-byte packets: buttons + delta X + delta Y
- ⬜ Mouse state struct: `int x, y, buttons`
- ⬜ Cursor position clamped to screen bounds
- ⬜ Test: move mouse, print X/Y coordinates to serial

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
- ✅ `pmm_alloc_page()` → returns physical address of free 4 KB page, or `PMM_ALLOC_FAIL` (0xFFFF…FFFF) on OOM
- ✅ `pmm_free_page(addr)` → marks page as free; bounds-checked
- ✅ `pmm_alloc_contiguous(n)` → n contiguous pages (needed for DMA buffers, LLM weight loading)
- ✅ `pmm_mark_used()` / `pmm_mark_free()` for explicit region management
- ✅ Test path wired in `kernel_main`: PMM init called with real MB2 mmap tag

### 2.2 — Virtual Memory Manager (VMM) / Paging
- ✅ `kernel/vmm.c` — VMM exists
- ✅ `kernel/vmm.h` — VMM header exists
- ✅ Set up 4-level paging (PML4 → PDPT → PD → PT) for x86-64
- ✅ Identity-map first 64 MB for safe kernel access to physical memory
  - 64 MB (not 4 MB) ensures all PMM-allocated page-table pages are reachable
- ✅ `vmm_map_page(virt, phys, flags)` — maps one physical page to virtual address
- ✅ `vmm_unmap_page(virt)` — unmaps and flushes TLB (`invlpg`); guarded at every page-table level
- ✅ `vmm_virt_to_phys(virt)` — safe walk; returns `PMM_ALLOC_FAIL` if any level not present
- ✅ `vmm_map_range()` — bulk map N pages
- ✅ Physical addresses accessed via `PHYS_TO_VIRT()` macro — future-proof for higher-half migration
- ✅ `vmm_switch_directory(pml4_phys)` — loads CR3
- ⬜ Kernel higher-half mapping: kernel at `0xFFFFFFFF80000000` (Phase 4 prerequisite)
- ⬜ Page fault handler: print faulting address + error code, then panic or COW
- ⬜ Test: map a page, write to it, read back, unmap — no crash

### 2.3 — Heap Allocator (kmalloc/kfree)
- ✅ `kernel/heap.c` — heap allocator exists
- ✅ `kernel/heap.h` — heap header exists
- ✅ Free-list allocator on top of VMM identity-mapped region
- ✅ `kmalloc(size)` → pointer to 16-byte aligned memory; first-fit with block splitting
- ✅ `kfree(ptr)` → marks block free; full-pass forward coalesce (not single-step)
- ✅ `krealloc(ptr, new_size)` → in-place if fits, else copy + free
- ✅ `kcalloc(count, elem_size)` → kmalloc + zero-fill
- ✅ `kmalloc_aligned(size, align)` — stores raw pointer 8 bytes before aligned result; use `kfree_aligned()` to free
- ✅ `kfree_aligned(ptr)` — recovers raw pointer and calls `kfree()`
- ✅ Magic canary (`0xA110C8ED`) on every block header — corruption detected on free
- ✅ Double-free guard in `kfree()`
- ✅ `kmemset`, `kmemcpy`, `kmemcmp` — no-libc memory utilities
- ✅ `heap_init()` called from `kernel_main` at `_kernel_end` rounded to next page
- ✅ Heap size: 2 MB inside identity-mapped window (virtual == physical at this stage)
- ⬜ Heap canary/guard pages in debug builds to catch overflows (Phase 4 nice-to-have)
- ⬜ Test: allocate strings, structs, free in random order — no corruption (run after IDT/exceptions are solid)

---

## Phase 3 — Storage & Filesystem

> Goal: Read files from disk. Load kernel modules and eventually LLM weights from files.

### 3.1 — PCI Enumeration
- ✅ `kernel/pci.c` — PCI driver exists
- ✅ `kernel/pci.h` — PCI header exists
- ⬜ Enumerate all PCI buses/devices/functions
- ⬜ Print device list on boot: vendor ID, device ID, class
- ⬜ `pci_find_device(vendor, device)` → returns PCI device struct
- ⬜ Enable bus-mastering DMA for storage devices

### 3.2 — AHCI / SATA Driver
- ✅ `kernel/ahci.h` — AHCI header exists (no .c yet)
- ⬜ Create `kernel/ahci.c`
- ⬜ Find AHCI controller via PCI (class 0x01, subclass 0x06)
- ⬜ Initialize HBA: enable AHCI mode, power up ports
- ⬜ Detect drives on each port (check SSTS register)
- ⬜ `ahci_read_sectors(port, lba, count, buffer)` — DMA read
- ⬜ `ahci_write_sectors(port, lba, count, buffer)` — DMA write
- ⬜ Test: read sector 0 (MBR), print first 512 bytes to serial

### 3.3 — Filesystem (ext2 or FAT32)
- ⬜ Create `kernel/fs/` directory
- ⬜ Choose filesystem: **ext2 recommended** (simpler than ext4, well-documented)
- ⬜ `fs/ext2.c` + `fs/ext2.h`:
  - ⬜ Read superblock, verify magic (`0xEF53`)
  - ⬜ Parse block group descriptors
  - ⬜ Read inode by number
  - ⬜ `ext2_open(path)` → returns file descriptor
  - ⬜ `ext2_read(fd, buf, size)` → reads bytes
  - ⬜ `ext2_close(fd)`
  - ⬜ `ext2_list_dir(path)` → list directory entries
- ⬜ VFS (Virtual Filesystem) abstraction layer: `vfs_open`, `vfs_read`, `vfs_write`, `vfs_close` — wraps any underlying FS
- ⬜ Test: create a test disk image with ext2, put a text file on it, read it in the kernel

### 3.4 — Initrd / Ramdisk
- ⬜ Pack initial files into a ramdisk (CPIO or custom format) embedded in the ISO
- ⬜ Kernel reads ramdisk from multiboot module list
- ⬜ Mount ramdisk as root filesystem before real disk driver is ready
- ⬜ Put LLM tokenizer vocab and initial config here

---

## Phase 4 — Multitasking & Scheduling

> Goal: Multiple processes/threads running concurrently. Shell in one thread, LLM in another.

### 4.1 — Context Switching
- ⬜ Create `kernel/task.c` + `kernel/task.h`
- ⬜ Define task struct: registers (rsp, rbp, rip, rflags, cr3), stack, pid, state, priority
- ⬜ `task_create(entry_fn, stack_size)` — allocates stack, sets up initial register frame
- ⬜ `task_switch(current, next)` — saves current registers to task struct, loads next registers
- ⬜ Assembly `switch_context` routine: `push` all regs, save RSP, load new RSP, `pop` all regs, `ret`

### 4.2 — Scheduler
- ⬜ Create `kernel/sched.c` + `kernel/sched.h`
- ⬜ Simple round-robin scheduler first (can upgrade to priority later)
- ⬜ Ready queue: linked list of runnable tasks
- ⬜ Timer IRQ (PIT/APIC) calls `sched_tick()` → picks next task → switches context
- ⬜ `sched_yield()` — voluntarily gives up CPU
- ⬜ `sched_sleep(ms)` — puts task in sleep queue, woken up by timer
- ⬜ `sched_exit()` — marks task as dead, removed from queue
- ⬜ Test: create 3 tasks that each print their own ID — should interleave on screen

### 4.3 — Kernel Threads
- ⬜ `kthread_create(fn, arg)` — creates a kernel-mode thread
- ⬜ All early kernel services run as kthreads: idle thread, LLM inference thread, I/O threads
- ⬜ Idle thread: runs `hlt` in a loop when no other task is ready

### 4.4 — Synchronization Primitives
- ⬜ Spinlock: `spinlock_t`, `spin_lock()`, `spin_unlock()` using atomic `xchg`
- ⬜ Mutex: `mutex_t`, `mutex_lock()` (sleeps if busy), `mutex_unlock()`
- ⬜ Semaphore: `sem_t`, `sem_wait()`, `sem_post()`
- ⬜ These are critical for LLM: inference runs in one thread, shell reads output in another

### 4.5 — User Mode (Ring 3)
- ⬜ TSS set up with kernel stack pointer (RSP0)
- ⬜ `enter_usermode(entry, stack)` — uses `sysret` or `iretq` to jump to ring 3
- ⬜ System call interface: `syscall` instruction → kernel handler dispatch table
- ⬜ Basic syscalls: `sys_write(fd, buf, len)`, `sys_read(fd, buf, len)`, `sys_exit(code)`
- ⬜ User stack mapped in user address space

---

## Phase 5 — Shell & Terminal

> Goal: An interactive shell that users can type into, and which can invoke built-in commands including the AI assistant.

### 5.1 — Terminal Emulator
- ⬜ Create `kernel/shell/terminal.c`
- ⬜ Ring buffer for input characters (keyboard → terminal)
- ⬜ Line editing: backspace, left/right arrows, home/end
- ⬜ History buffer: up/down arrows cycle through previous commands
- ⬜ `terminal_readline(buf, maxlen)` — blocking read until Enter pressed
- ⬜ ANSI escape code support: cursor movement, colors (for AI output formatting)

### 5.2 — Shell
- ⬜ Create `kernel/shell/shell.c`
- ⬜ Print prompt: `AIOS> `
- ⬜ Parse command line: tokenize by spaces, handle quoted strings
- ⬜ Built-in commands:
  - ⬜ `help` — list commands
  - ⬜ `clear` — clear screen
  - ⬜ `mem` — print physical/virtual memory usage stats
  - ⬜ `ps` — list running tasks with PID and state
  - ⬜ `ls [path]` — list directory
  - ⬜ `cat [file]` — print file contents
  - ⬜ `load [model]` — load an LLM model from disk
  - ⬜ `ai [prompt]` — send prompt to loaded LLM, stream output to terminal
  - ⬜ `chat` — enter interactive chat mode (multi-turn conversation)
  - ⬜ `reboot` — reboot system
  - ⬜ `shutdown` — power off (ACPI)

### 5.3 — ACPI (Power Management)
- ⬜ Create `kernel/acpi.c` + `kernel/acpi.h`
- ⬜ Find RSDP in BIOS area or EFI config table
- ⬜ Parse RSDT/XSDT to find FADT
- ⬜ `acpi_shutdown()` — write SLP_TYPa + SLP_EN to PM1a_CNT
- ⬜ `acpi_reboot()` — write to RESET_REG in FADT

---

## Phase 6 — GPU / Hardware Acceleration

> Goal: Access GPU memory and compute for LLM matrix operations. This is the bridge between the OS and the LLM engine.

### 6.1 — PCIe / GPU Detection
- ⬜ Enumerate PCI for GPU (NVIDIA class 0x0300, AMD 0x0300)
- ⬜ Map GPU BAR0 (MMIO registers) and BAR1 (VRAM aperture) into kernel virtual address space
- ⬜ Print GPU VRAM size on boot

### 6.2 — NVIDIA GPU Driver (Minimal)
- ⬜ Create `kernel/gpu/nvidia.c`
- ⬜ Read GPU firmware version / device ID
- ⬜ Submit command buffer via FIFO (PFIFO/CE engine)
- ⬜ Implement DMA engine: copy data CPU→GPU VRAM, GPU VRAM→CPU
- ⬜ **Note:** Full open-source NVIDIA support requires nouveau-style reverse engineering OR target only AMD/Intel for which documentation is public

### 6.3 — AMD GPU Driver (Recommended for Open Docs)
- ⬜ Create `kernel/gpu/amdgpu.c`
- ⬜ Use AMD GPU Programming Guide (publicly available)
- ⬜ Initialize GFX engine: read golden registers, configure compute queues
- ⬜ `gpu_alloc_vram(size)` → virtual address in VRAM
- ⬜ `gpu_copy_to_vram(host_ptr, gpu_ptr, size)` — DMA host→GPU
- ⬜ `gpu_copy_from_vram(gpu_ptr, host_ptr, size)` — DMA GPU→host
- ⬜ Submit compute shader (custom GCN/RDNA microcode) for matrix multiply

### 6.4 — CPU SIMD Fallback (Always Required)
- ⬜ Create `kernel/simd.c` + `kernel/simd.h`
- ⬜ Detect CPU features via CPUID: SSE2, AVX, AVX2, AVX-512
- ⬜ `simd_matmul_f32(A, B, C, M, N, K)` — matrix multiply using AVX2 intrinsics
- ⬜ `simd_vec_add_f32(a, b, out, len)` — vectorized vector add
- ⬜ `simd_softmax_f32(x, out, len)` — softmax with max-subtraction for stability
- ⬜ `simd_gelu_f32(x, out, len)` — GELU activation (polynomial approximation)
- ⬜ All buffers 32-byte aligned (`kmalloc_aligned(size, 32)`)

---

## Phase 7 — LLM Inference Engine (The Core Innovation)

> Goal: Run a transformer model (GPT-2 scale to start, then larger) natively in the OS with no external dependencies.

### 7.1 — Tensor Library
- ⬜ Create `kernel/llm/tensor.c` + `kernel/llm/tensor.h`
- ⬜ Tensor struct: `float* data`, `int dims[4]`, `int ndim`, `size_t numel`
- ⬜ `tensor_alloc(dims, ndim)` — allocates aligned memory, fills struct
- ⬜ `tensor_free(t)` — frees backing memory
- ⬜ `tensor_reshape(t, new_dims, ndim)` — non-copying reshape (just change dim array)
- ⬜ `tensor_slice(t, dim, start, end)` — returns view into tensor (no copy)
- ⬜ `tensor_print(t)` — for debugging: print shape and first 8 values

### 7.2 — Math Operations (CPU Path)
- ⬜ Create `kernel/llm/ops.c` + `kernel/llm/ops.h`
- ⬜ `ops_matmul(A, B, C)` — uses `simd_matmul_f32` from Phase 6.4
- ⬜ `ops_add(a, b, out)` — element-wise add (bias add)
- ⬜ `ops_scale(a, scalar, out)` — multiply by scalar
- ⬜ `ops_softmax(x, out)` — row-wise softmax
- ⬜ `ops_layer_norm(x, weight, bias, out, eps)` — layer normalization
- ⬜ `ops_gelu(x, out)` — GELU activation
- ⬜ `ops_embedding_lookup(table, indices, out, vocab_size, dim)` — gather rows
- ⬜ `ops_rope(q, k, pos)` — Rotary Position Embedding (for modern transformers like LLaMA)

### 7.3 — Attention Mechanism
- ⬜ Create `kernel/llm/attention.c`
- ⬜ Multi-Head Attention (MHA):
  - ⬜ Project Q, K, V via weight matrices
  - ⬜ Split into heads: reshape `[seq, dim]` → `[heads, seq, head_dim]`
  - ⬜ Scaled dot-product: `scores = (Q @ K.T) / sqrt(head_dim)`
  - ⬜ Apply causal mask (upper-triangular -inf for autoregressive decoding)
  - ⬜ Softmax over last dim
  - ⬜ Weighted sum: `attn_out = softmax(scores) @ V`
  - ⬜ Merge heads: reshape back to `[seq, dim]`
  - ⬜ Output projection via weight matrix
- ⬜ KV-Cache for efficient autoregressive decoding:
  - ⬜ Allocate `kv_cache[num_layers][2][max_seq][head_dim * heads]`
  - ⬜ On each token: only compute Q for new token, look up cached K/V
  - ⬜ Append new K/V to cache

### 7.4 — Transformer Block
- ⬜ Create `kernel/llm/transformer.c`
- ⬜ One transformer block:
  1. Layer Norm 1 → Attention → residual add
  2. Layer Norm 2 → MLP (two linear layers + GELU) → residual add
- ⬜ MLP: `hidden = GELU(x @ W1 + b1)`, `out = hidden @ W2 + b2`
- ⬜ GPT-2 style: post-norm variant
- ⬜ LLaMA style: pre-norm with RMSNorm (implement `ops_rms_norm` if targeting LLaMA weights)

### 7.5 — Full Model Forward Pass
- ⬜ Create `kernel/llm/model.c` + `kernel/llm/model.h`
- ⬜ Model config struct: `n_layers`, `n_heads`, `n_embd`, `vocab_size`, `max_seq_len`
- ⬜ Model struct: arrays of weight tensors for all layers
- ⬜ `model_forward(model, token_ids, seq_len, kv_cache)` → logits tensor `[seq, vocab]`
- ⬜ Greedy decode: `argmax(logits[-1])` → next token id
- ⬜ Temperature sampling: divide logits by temp, softmax, sample from distribution
- ⬜ Top-k sampling: zero out all but top-k logits before softmax
- ⬜ Top-p (nucleus) sampling: cumulative probability threshold

### 7.6 — Weight File Format & Loader
- ⬜ Create `kernel/llm/loader.c` + `kernel/llm/loader.h`
- ⬜ Design a simple binary weight format (or support GGUF which is LLaMA.cpp's format):
  - Header: magic bytes, version, model config (n_layers, n_heads, etc.)
  - Weight tensors: name string + shape array + raw float32/float16 data
- ⬜ `loader_load_model(path, model*)` — reads file from VFS, populates model weight tensors
- ⬜ Support `float16` (FP16) weights with `float32` compute (dequantize on load or on the fly)
- ⬜ Support 4-bit quantization (GGUF Q4_K_M or custom scheme) to fit larger models in RAM
- ⬜ Progress reporting: print "Loading layer X/N..." to serial during load

### 7.7 — Tokenizer
- ⬜ Create `kernel/llm/tokenizer.c` + `kernel/llm/tokenizer.h`
- ⬜ Implement BPE (Byte Pair Encoding) tokenizer (same algorithm as GPT-2/LLaMA)
- ⬜ Load vocabulary from file: `vocab.json` or binary format
- ⬜ `tokenizer_encode(text, ids_out, max_len)` → converts string to token ID array
- ⬜ `tokenizer_decode(ids, len, text_out)` → converts token IDs back to string
- ⬜ Handle special tokens: `<BOS>`, `<EOS>`, `<PAD>`, `<UNK>`
- ⬜ Test: encode "Hello world", decode back — should round-trip correctly

### 7.8 — Quantization (INT8 / INT4)
- ⬜ Create `kernel/llm/quant.c`
- ⬜ `quant_q8_0_dequantize(quantized, scale, out, len)` — Q8_0 (8-bit integer) dequantize
- ⬜ `quant_q4_k_dequantize(data, scales, out, len)` — Q4_K (4-bit per-channel) dequantize
- ⬜ `quant_matmul_q8_f32(A_q8, B_f32, C_f32, M, N, K)` — mixed precision matmul
- ⬜ Goal: run a 7B parameter model quantized to 4-bit (~4GB) on a machine with 8GB RAM

### 7.9 — Inference Manager
- ⬜ Create `kernel/llm/inference.c`
- ⬜ `inference_init(model_path)` — loads model, initializes KV cache, runs as kthread
- ⬜ `inference_prompt(text, callback_fn)` — tokenizes, runs forward pass in loop, calls callback per token
- ⬜ Streaming output: each new token triggers `callback_fn(token_text)` → shell prints it immediately
- ⬜ `inference_reset()` — clears KV cache (new conversation)
- ⬜ `inference_set_system_prompt(text)` — prepend system prompt to every new conversation
- ⬜ Thread safety: inference runs in dedicated kthread, shell communicates via message queue

---

## Phase 8 — LLM Training Engine (Optional — Build Your Own Weights)

> Goal: Train small models from scratch on the OS itself, or fine-tune loaded models.

### 8.1 — Autograd / Backward Pass
- ⬜ Create `kernel/llm/grad.c` + `kernel/llm/grad.h`
- ⬜ Each op that needs a gradient has a corresponding backward function
- ⬜ `matmul_backward(grad_out, A, B, grad_A, grad_B)` — dL/dA, dL/dB
- ⬜ `softmax_backward(grad_out, softmax_out, grad_in)` — Jacobian-vector product
- ⬜ `layer_norm_backward(grad_out, x, weight, bias, grad_x, grad_w, grad_b)`
- ⬜ Tape-based autograd (store operation sequence during forward, replay in reverse for backward)

### 8.2 — Optimizer
- ⬜ Create `kernel/llm/optimizer.c`
- ⬜ SGD: `param -= lr * grad`
- ⬜ Adam: maintain m, v momentum tensors per parameter; update rule per Adam paper
- ⬜ Learning rate scheduler: warmup + cosine decay
- ⬜ Gradient clipping: clip global norm to threshold

### 8.3 — Data Pipeline
- ⬜ `dataset_load(path)` — reads tokenized training data from disk
- ⬜ `dataset_next_batch(batch_size, seq_len, tokens_out, labels_out)` — fills batch tensor
- ⬜ Shuffle buffer for training data

### 8.4 — Training Loop
- ⬜ `train(model, dataset, optimizer, config)` function
- ⬜ Forward pass → cross-entropy loss → backward pass → optimizer step
- ⬜ Print loss every N steps to serial
- ⬜ Save checkpoint to disk every M steps

---

## Phase 9 — Network Stack (Future)

> Goal: TCP/IP stack so the OS can fetch model weights from the internet or communicate with other machines.

### 9.1 — NIC Driver
- ⬜ Create `kernel/net/e1000.c` — Intel e1000 driver (well-documented, QEMU supports it)
- ⬜ Find e1000 via PCI enumeration
- ⬜ Map MMIO, set up RX/TX descriptor rings
- ⬜ `e1000_send(packet, len)` — transmit raw Ethernet frame
- ⬜ RX interrupt handler: copies received frame to rx_queue

### 9.2 — Network Stack
- ⬜ Create `kernel/net/` directory with:
  - ⬜ `eth.c` — Ethernet frame parsing (src MAC, dst MAC, EtherType)
  - ⬜ `arp.c` — ARP request/reply
  - ⬜ `ip.c` — IPv4: parse header, routing table, send/receive
  - ⬜ `udp.c` — UDP: checksum, send/receive
  - ⬜ `tcp.c` — TCP: three-way handshake, flow control, retransmit
  - ⬜ `dhcp.c` — DHCP client: auto-configure IP on boot
  - ⬜ `http.c` — minimal HTTP/1.1 client (for downloading model weights)

---

## Phase 10 — Graphical User Interface (Future)

> Goal: A simple window system where the AI assistant has a chat window.

### 10.1 — Framebuffer & Drawing Primitives
- ⬜ Switch to VESA/GOP framebuffer (from Phase 1.4 TODO)
- ⬜ `fb_fill_rect(x, y, w, h, color)` — filled rectangle
- ⬜ `fb_draw_rect(x, y, w, h, color)` — rectangle outline
- ⬜ `fb_draw_line(x1, y1, x2, y2, color)` — Bresenham's line
- ⬜ `fb_blit(src, dx, dy, w, h)` — copy bitmap to screen

### 10.2 — Font Rendering
- ⬜ Embed a bitmap font (PSF format or custom 8x16 bitmap)
- ⬜ `font_draw_char(x, y, c, fg, bg)` — draw one character
- ⬜ `font_draw_string(x, y, str, fg, bg)` — draw string

### 10.3 — Window Manager
- ⬜ Window struct: x, y, width, height, title, framebuffer region, event queue
- ⬜ `wm_create_window(title, w, h)` → window handle
- ⬜ `wm_draw_window(win)` — renders title bar, border, client area
- ⬜ Mouse hit testing: which window is under cursor?
- ⬜ Window dragging: click title bar, drag
- ⬜ Z-order: focused window on top, others clipped

### 10.4 — AI Chat Window
- ⬜ Text widget: wraps text, scrolls, renders with font
- ⬜ Input box: text field + send button
- ⬜ When user hits Enter: sends to inference engine, streams tokens into chat widget
- ⬜ System tray: shows model name, memory used, tokens/sec

---

## Phase 11 — Security & Hardening (Future)

- ⬜ Stack canaries in kernel compile flags (`-fstack-protector-strong`)
- ⬜ KASLR: randomize kernel load address at boot using RDRAND
- ⬜ SMEP/SMAP: set bits in CR4, prevent kernel executing/reading user memory
- ⬜ Secure boot chain: verify kernel signature before loading
- ⬜ Sandboxed LLM inference: run model in restricted address space, no direct hardware access

---

## Current Progress Summary (as of May 2026)

### What Exists in the Codebase

| Component | Files | Status |
|-----------|-------|--------|
| Build system | `build.sh`, `Makefile`, `boot/linker.ld` | ✅ Scaffolded |
| Dep checker | `scripts/check_deps.sh` | ✅ Complete |
| GRUB boot | `boot/grub.cfg`, `boot/kernel_entry.asm` | ✅ Fixed & Complete |
| GDT | `kernel/gdt.c` | ✅ Complete — real TSS, far-jump CS reload, ltr |
| IDT + ISR | `kernel/idt.c`, `kernel/isr_stubs.asm` | ✅ Complete — 256 gates, exception dump, idt_flush, #DE test |
| APIC | `kernel/apic.c`, `kernel/apic.h` | 🔄 Code exists |
| PIT | `kernel/pit.c`, `kernel/pit.h` | 🔄 Code exists |
| VGA | `kernel/vga.c` | 🔄 Code exists |
| Serial | `kernel/serial.c`, `kernel/serial.h` | 🔄 Code exists |
| Keyboard | `kernel/keyboard.c` | 🔄 Code exists |
| Mouse | `kernel/mouse.c` | 🔄 Code exists |
| PCI | `kernel/pci.c`, `kernel/pci.h` | 🔄 Code exists |
| AHCI header | `kernel/ahci.h` | 🔄 Header only — no .c |
| PMM | `kernel/pmm.c`, `kernel/pmm.h` | ✅ Complete — MB2 mmap, bitmap alloc, PMM_ALLOC_FAIL sentinel |
| VMM | `kernel/vmm.c`, `kernel/vmm.h` | ✅ Complete — 4-level paging, 64 MB identity map, safe walk |
| Heap | `kernel/heap.c`, `kernel/heap.h` | ✅ Complete — free-list, full coalesce, aligned alloc fixed |
| Kernel main | `kernel/kernel_main.c` | ✅ Phase 1.1 — IDT exception dump + #DE test wired |
| kernel/include/ | Headers directory | 🔄 Exists |
| Scheduler | — | ⬜ Not started |
| Filesystem | — | ⬜ Not started |
| Shell | — | ⬜ Not started |
| LLM engine | — | ⬜ Not started |
| GPU driver | — | ⬜ Not started |
| Network | — | ⬜ Not started |
| GUI | — | ⬜ Not started |

### Immediate Next Steps (pick up here)

1. **Confirm GRUB boots ISO in QEMU** — run `make run`, confirm it reaches `kernel_main` without crashing (Phase 0.2 last item)
2. **PIC / APIC** — remap legacy PIC OR initialize Local APIC (Phase 1.2) ← **next coding task**
3. **PIT timer** — configure channel 0 at 1000 Hz, wire IRQ0 handler, global tick counter (Phase 1.3)
4. **VGA verification** — confirm `vga_putchar`, `vga_clear`, scrolling, color support all work (Phase 1.4)
5. **Serial output** — COM1 at 115200 baud, run QEMU with `-serial stdio` (Phase 1.5)
6. **Page fault handler** — install `#PF` handler that prints faulting CR2 + error code (Phase 2.2 remaining item)
7. **Verify heap** — call `kmalloc(64)`, write to it, `kfree` it, no crash (Phase 2.3 test item)
8. **Create `ahci.c`** — Phase 3.2 is blocked on this

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
│   └── check_deps.sh        ← Dependency checker (run before first build)
├── boot/
│   ├── grub.cfg             ← GRUB boot config
│   ├── kernel_entry.asm     ← Multiboot2 entry, SSE/SSE2 enable, 64-bit mode
│   └── linker.ld            ← Kernel binary layout
├── kernel/
│   ├── kernel_main.c        ← C entry point
│   ├── gdt.c / .h           ← Global Descriptor Table
│   ├── idt.c                ← Interrupt Descriptor Table ✅
│   ├── isr_stubs.asm        ← ISR assembly trampolines ✅
│   ├── apic.c / .h          ← Advanced PIC
│   ├── pit.c / .h           ← Programmable Interval Timer
│   ├── vga.c                ← VGA text mode
│   ├── serial.c / .h        ← Serial port (debug)
│   ├── keyboard.c           ← PS/2 keyboard
│   ├── mouse.c              ← PS/2 mouse
│   ├── pmm.c / .h           ← Physical memory manager ✅
│   ├── vmm.c / .h           ← Virtual memory / paging ✅
│   ├── heap.c / .h          ← Kernel heap (kmalloc) ✅
│   ├── pci.c / .h           ← PCI enumeration
│   ├── ahci.h               ← AHCI header (needs ahci.c)
│   ├── include/             ← Shared kernel headers
│   ├── fs/                  ← [TODO] Filesystem drivers
│   │   └── ext2.c / .h
│   ├── shell/               ← [TODO] Shell + terminal
│   │   ├── shell.c
│   │   └── terminal.c
│   ├── task.c / .h          ← [TODO] Multitasking
│   ├── sched.c / .h         ← [TODO] Scheduler
│   ├── simd.c / .h          ← [TODO] AVX2 math ops
│   ├── gpu/                 ← [TODO] GPU drivers
│   │   └── amdgpu.c / .h
│   └── llm/                 ← [TODO] LLM inference engine
│       ├── tensor.c / .h
│       ├── ops.c / .h
│       ├── attention.c
│       ├── transformer.c
│       ├── model.c / .h
│       ├── loader.c / .h
│       ├── tokenizer.c / .h
│       ├── quant.c
│       ├── inference.c
│       ├── grad.c / .h      ← [Optional] Training
│       └── optimizer.c
└── docs/                    ← Reference documents
```

---

*Last updated: May 2026 — Phase 1.1 complete. IDT fully implemented with exception register dump, idt_flush(), #DE test. Next: Phase 1.2 (PIC/APIC).*

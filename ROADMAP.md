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

### 0.3 — GDT
- ✅ `kernel/gdt.c` — GDT setup code exists
- ✅ Verify GDT has: null descriptor, kernel code (64-bit), kernel data, user code, user data, TSS descriptor
- ✅ `gdt_flush()` calls `lgdt` and far-jumps to reload CS
- ✅ TSS loaded with `ltr`
- ✅ Test: kernel runs in ring 0 after GDT reload with no triple fault

---

## Phase 1 — Interrupts, Timers & Basic I/O

### 1.1 — IDT
- ✅ `kernel/idt.c`, `kernel/isr_stubs.asm` — 256 entries, exception handlers, `lidt`

### 1.2 — PIC / APIC
- ✅ `kernel/apic.c`, `kernel/apic.h` — 8259 remapped+masked, Local APIC + IOAPIC wired

### 1.3 — PIT
- ✅ `kernel/pit.c`, `kernel/include/pit.h` — 1 kHz, `pit_sleep_ms`, `g_ticks`

### 1.4 — VGA
- ✅ `kernel/vga.c` — text mode, color, scroll, hardware cursor
- ⬜ **TODO:** framebuffer VESA/GOP mode for GUI (Phase 10 prerequisite)

### 1.5 — Serial
- ✅ `kernel/serial.c`, `kernel/serial.h` — COM1 115200, `klog` macro

### 1.6 — Keyboard
- ✅ `kernel/keyboard.c` — PS/2 IRQ1, scancode→ASCII, ring buffer, modifiers
- ✅ **Phase 10.3:** `keyboard_set_gui_callback(fn)` hook added

### 1.7 — Mouse
- ✅ `kernel/mouse.c` — PS/2 IRQ12, 3-byte packets, abs position, ring buffer
- ✅ **Phase 10.3:** `mouse_set_gui_callback(fn)` hook added

---

## Phase 2 — Memory Management

### 2.1 — PMM
- ✅ `kernel/pmm.c`, `kernel/pmm.h` — bitmap allocator, MB2 mmap parse

### 2.2 — VMM
- ✅ `kernel/vmm.c`, `kernel/vmm.h` — 4-level paging, map/unmap/walk, page fault handler

### 2.3 — Heap
- ✅ `kernel/heap.c`, `kernel/heap.h` — kmalloc/kfree/krealloc/kcalloc/kmalloc_aligned

---

## Phase 3 — Storage & Filesystem

### 3.1 — PCI
- ✅ `kernel/pci.c`, `kernel/pci.h`

### 3.2 — AHCI
- ✅ `kernel/ahci.c`, `kernel/ahci.h`

### 3.3 — FAT32 + VFS
- ✅ `kernel/fat32.c`, `kernel/fs/vfs.c`, `kernel/fs/vfs.h`

### 3.4 — Initrd
- ✅ `kernel/initrd.c`, `scripts/mkinitrd.py`, VFS integration

---

## Phase 4 — Multitasking & Scheduling

### 4.1–4.4 — Tasks / Scheduler / kthread / Sync
- ✅ All complete

### 4.5 — User Mode (Ring 3)
- ⬜ TSS RSP0 per task, `enter_usermode`, syscall dispatch, basic syscalls

---

## Phase 5 — Shell & Terminal

### 5.1 — Terminal Emulator
- ✅ `kernel/shell/terminal.c`, `kernel/shell/terminal.h`

### 5.2 — Shell
- ✅ `kernel/shell/shell.c` — all built-ins including `startx` (Phase 10.6)

### 5.3 — ACPI
- ✅ `kernel/acpi.c`, `kernel/acpi.h`

---

## Phase 6 — GPU / Hardware Acceleration

### 6.1 — PCIe / GPU Detection
- ⬜ Enumerate GPU (class 0x0300), map BAR0/BAR1

### 6.2 — NVIDIA Driver
- ⬜ `kernel/gpu/nvidia.c` — FIFO/CE command, DMA

### 6.3 — AMD Driver
- ⬜ `kernel/gpu/amdgpu.c` — GFX/RDNA compute queues

### 6.4 — CPU SIMD Fallback
- ✅ `kernel/simd.c`, `kernel/simd.h` — AVX2 matmul/softmax/gelu

---

## Phase 7 — LLM Inference Engine

### 7.1 — Tensor Library
- ✅ `kernel/llm/tensor.c`, `kernel/llm/tensor.h`

### 7.2 — Math Operations
- ✅ `kernel/llm/ops.c`, `kernel/llm/ops.h` — matmul, softmax, layernorm, gelu, RoPE

### 7.3 — Attention + KV-Cache
- ✅ `kernel/llm/attention.c`, `kernel/llm/attention.h`

### 7.4 — Transformer Block
- ✅ `kernel/llm/transformer.c`, `kernel/llm/transformer.h`

### 7.5 — Full Model Forward Pass
- ✅ `kernel/llm/model.c`, `kernel/llm/model.h`
- ✅ `model_forward`, `model_sample` (greedy / top-k / top-p)

### 7.6 — Weight Loader
- ✅ `kernel/llm/loader.c`, `kernel/llm/loader.h`
- ✅ GGUF v2/v3 parse, FP32/FP16/Q8_0/Q4_K dequant, zero-copy tensor views

### 7.7 — Tokenizer
- ✅ `kernel/llm/tokenizer.c`, `kernel/llm/tokenizer.h`
- ✅ BPE encode/decode, SentencePiece ▁ prefix, byte-fallback, BOS/EOS

### 7.8 — Quantization
- ⬜ `kernel/llm/quant.c` — Q8_0, Q4_K dequant, mixed-precision matmul

### 7.9 — Inference Manager
- ⬜ `kernel/llm/inference.c` — kthread, `inference_prompt`, KV-cache reset

---

## Phase 8 — LLM Training Engine (Optional)
- ⬜ Autograd, Adam, dataset pipeline, training loop

---

## Phase 9 — Network Stack
- ⬜ e1000 NIC, eth/arp/ip/udp/tcp/dhcp/http

---

## Phase 10 — Graphical User Interface

### 10.1 — Framebuffer & Primitives
- ✅ `kernel/gfx/framebuffer.c`, `kernel/gfx/colors.h`

### 10.2 — Font & Text Rendering
- ✅ `kernel/gfx/font.c`, `kernel/gfx/font.h`
- ⬜ `assets/fonts/` PSF richer font set

### 10.3 — GUI Input Wiring
- ✅ All complete (input.c, input_wiring.c, keyboard/mouse hooks)

### 10.4 — Window Manager Core
- ✅ `kernel/gui/wm.c` — render loop, drag, resize

### 10.5 — Desktop, Taskbar & Start Menu
- ✅ All complete

### 10.6 — GUI Kernel Thread & Mode Switch
- ✅ `gui_wm_start()` + `startx` in `shell.c`
- ⬜ TTY switch key (Ctrl+Alt+F1)

---

## Phase 11 — Basic GUI Applications

### 11.1 — Notepad
- ✅ `kernel/apps/notepad.c`, `kernel/apps/notepad.h`
- ✅ Gap buffer — O(1) insert/delete at cursor
- ✅ Keyboard: printable chars, Enter, Backspace, Delete, arrows, Home/End, PgUp/PgDn
- ✅ Ctrl+N (new), Ctrl+S (save), Ctrl+A (select all)
- ✅ VFS load on open, VFS save on Ctrl+S
- ✅ Line-number gutter, blinking cursor, status bar (Ln/Col + filename + modified dot)
- ✅ Integrated with WM via `wm_create_window` / `wm_get_fb` / `wm_dirty`

### 11.2 — File Explorer
- ⬜ `kernel/apps/explorer.c` — two-pane VFS browser

### 11.3 — GUI Terminal
- ⬜ `kernel/apps/terminal_gui.c` — shell in a GUI window

### 11.4 — Settings
- ⬜ `kernel/apps/settings.c` — tabbed config panel

### 11.5 — AI Chat Window
- ⬜ `kernel/apps/ai_chat.c` — GUI front-end for Phase 7.9 inference

---

## Phase 12 — Security & Hardening
- ⬜ Stack canaries, KASLR, SMEP/SMAP, secure boot, sandboxed LLM

---

## Current Progress Summary (as of May 2026)

| Component | Files | Status |
|-----------|-------|--------|
| Boot / GDT / IDT / APIC / PIT | — | ✅ |
| VGA / Serial / Keyboard / Mouse | — | ✅ |
| PMM / VMM / Heap | — | ✅ |
| PCI / AHCI / FAT32 / VFS / Initrd | — | ✅ |
| Task / Sched / kthread / Sync / ACPI | — | ✅ |
| Shell + Terminal | `shell/` | ✅ + startx |
| CPU SIMD | `kernel/simd.c` | ✅ |
| Tensor + Ops | `llm/tensor.c`, `ops.c` | ✅ 7.1–7.2 |
| Attention + KV-Cache | `llm/attention.c` | ✅ 7.3 |
| Transformer Block | `llm/transformer.c` | ✅ 7.4 |
| Model Forward + Sampling | `llm/model.c` | ✅ 7.5 |
| Weight Loader | `llm/loader.c` | ✅ 7.6 |
| Tokenizer | `llm/tokenizer.c` | ✅ 7.7 |
| **Notepad** | `apps/notepad.c` | ✅ **11.1** |
| Quantization | `llm/quant.c` | ⬜ 7.8 |
| File Explorer | `apps/explorer.c` | ⬜ **NEXT → 11.2** |

### Immediate Next Steps

1. **Phase 11.2 — File Explorer** ← **NEXT (GUI track)**  
   `kernel/apps/explorer.c` — two-pane VFS browser: left pane = directory tree, right pane = file list. Double-click dir to navigate, double-click file to open in Notepad.

2. **Phase 7.8 — Quantization** (LLM track)  
   `kernel/llm/quant.c` — on-the-fly Q8_0/Q4_K dequant matmul.

---

## Coding Guidelines (for AI-assisted sessions)

```
We are building AIOS — an operating system from scratch with an integrated local LLM.
Codebase: https://github.com/subhobhai943/AIOS..git
Language: C (freestanding, no libc), NASM assembly.
Check ROADMAP.md for current progress. Continue from the first unchecked ⬜ item.
No stdlib headers except <stdint.h>, <stddef.h>, <stdbool.h>.
All heap: kmalloc/kfree (heap.h). Compiler: x86_64-elf-gcc -ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel
```

---

## File Structure Reference

```
kernel/llm/
  tensor.c/h      ← ✅ 7.1
  ops.c/h         ← ✅ 7.2
  attention.c/h   ← ✅ 7.3
  transformer.c/h ← ✅ 7.4
  model.c/h       ← ✅ 7.5
  loader.c/h      ← ✅ 7.6
  tokenizer.c/h   ← ✅ 7.7
  quant.c/h       ← ⬜ 7.8
  inference.c/h   ← ⬜ 7.9

kernel/apps/
  notepad.c/h     ← ✅ 11.1
  explorer.c/h    ← ⬜ NEXT 11.2
  terminal_gui.c  ← ⬜ 11.3
  settings.c      ← ⬜ 11.4
  ai_chat.c       ← ⬜ 11.5
```

---

*Last updated: May 2026 — Phase 11.1 complete (Notepad: gap buffer, full keyboard handling, VFS open/save, line gutter, blinking cursor, status bar). Next: Phase 11.2 — File Explorer (`kernel/apps/explorer.c`).*

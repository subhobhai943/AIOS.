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
- ✅ `attn_config_t`, `kv_cache_t`, `kvcache_alloc/free/reset`
- ✅ `attn_forward` (causal MHA, RoPE, GQA), `attn_forward_full` (prefill wrapper)

### 7.4 — Transformer Block
- ✅ `kernel/llm/transformer.c`, `kernel/llm/transformer.h`
- ✅ GPT-2 post-norm (LayerNorm + GELU MLP) and LLaMA pre-norm (RMSNorm + SwiGLU)
- ✅ `transformer_block_forward` — single-token step, zero heap leak

### 7.5 — Full Model Forward Pass
- ⬜ `kernel/llm/model.c`, `kernel/llm/model.h`
- ⬜ `model_config_t`, `aios_model_t`, `model_forward()`, greedy/top-k/top-p sampling

### 7.6 — Weight Loader
- ⬜ `kernel/llm/loader.c` — GGUF or custom binary, FP16/Q4

### 7.7 — Tokenizer
- ⬜ `kernel/llm/tokenizer.c` — BPE, encode/decode, special tokens

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
- ✅ `kernel/gfx/framebuffer.c`, `kernel/gfx/colors.h` — MB2 parse, 32-bit ARGB

### 10.2 — Font & Text Rendering
- ✅ `kernel/gfx/font.c`, `kernel/gfx/font.h` — 8×16 bitmap font
- ⬜ `assets/fonts/` PSF richer font set

### 10.3 — GUI Input Wiring
- ✅ `kernel/gui/input.c`, `kernel/gui/input.h` — event ring buffer, double-click, clamping
- ✅ `kernel/gui/input_wiring.c`, `kernel/gui/input_wiring.h` — activation fence
- ✅ `keyboard_set_gui_callback()` in `keyboard.c`
- ✅ `mouse_set_gui_callback()` in `mouse.c`
- ✅ `gui_wiring_activate()` called from `startx` shell command

### 10.4 — Window Manager Core
- ✅ `kernel/gui/window.c`, `kernel/gui/window.h` — z-ordered list, create/destroy
- ✅ `kernel/gui/wm.c` — render loop, hit-test, activation
- ✅ Title-bar drag state machine (`DRAG_MOVE` in `wm.c`)
- ✅ Border resize grip state machine (`DRAG_RESIZE` in `wm.c`)

### 10.5 — Desktop, Taskbar & Start Menu
- ✅ `kernel/gui/desktop.c`, `kernel/gui/desktop.h` — background + logo blit
- ✅ `kernel/gui/taskbar.c`, `kernel/gui/taskbar.h` — 32–40 px strip, Start button, window buttons
- ✅ `kernel/gui/start_menu.c`, `kernel/gui/start_menu.h` — vertical app-launch menu

### 10.6 — GUI Kernel Thread & Mode Switch
- ✅ `gui_wm_start()` kthread spawned by `startx` shell command
- ✅ `startx` command in `shell.c`: wiring → WM spawn → async desktop
- ⬜ TTY switch key (e.g. Ctrl+Alt+F1) to return focus to shell terminal

---

## Phase 11 — Basic GUI Applications

### 11.1 — Notepad
- ⬜ `kernel/apps/notepad.c` — multiline edit, VFS open/save

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
| Build / boot / GDT / IDT | — | ✅ |
| APIC / PIT / VGA / Serial | — | ✅ |
| Keyboard | `kernel/keyboard.c` | ✅ + GUI hook |
| Mouse | `kernel/mouse.c` | ✅ + GUI hook |
| PMM / VMM / Heap | — | ✅ |
| PCI / AHCI / FAT32 / VFS / Initrd | — | ✅ |
| Task / Scheduler / kthread / Sync | — | ✅ |
| Terminal / Shell / ACPI | — | ✅ |
| CPU SIMD | `kernel/simd.c` | ✅ |
| Tensor + Ops | `kernel/llm/tensor.c`, `ops.c` | ✅ |
| **Attention + KV-Cache** | `kernel/llm/attention.c` | ✅ Phase 7.3 |
| **Transformer Block** | `kernel/llm/transformer.c` | ✅ Phase 7.4 |
| Framebuffer / Font | `kernel/gfx/` | ✅ |
| GUI Input | `kernel/gui/input.c` | ✅ |
| **GUI Input Wiring** | `kernel/gui/input_wiring.c` | ✅ **Phase 10.3** |
| Window Manager | `kernel/gui/wm.c` | ✅ drag+resize |
| Desktop / Taskbar / Start Menu | `kernel/gui/desktop.c` etc. | ✅ Phase 10.5 |
| **startx shell command** | `kernel/shell/shell.c` | ✅ **Phase 10.6** |
| GPU driver | — | ⬜ Phase 6 |
| Model forward pass | `kernel/llm/model.c` | ⬜ **NEXT → Phase 7.5** |
| GUI Apps | `kernel/apps/` | ⬜ Phase 11 |

### Immediate Next Steps

1. **Phase 7.5 — Full model forward pass** ← **NEXT**  
   `kernel/llm/model.c` + `model.h`: `model_config_t`, `aios_model_t` (array of transformer blocks + embedding table + LM-head), `model_forward()`, greedy/temperature/top-k/top-p sampling.

2. **Phase 11.1 — Notepad app (parallel GUI track)**  
   `kernel/apps/notepad.c` + `.h` — first real GUI application window.

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
AIOS/
├── ROADMAP.md
├── boot/  grub.cfg  kernel_entry.asm  linker.ld
├── scripts/  check_deps.sh  mkinitrd.py
├── assets/  tokenizer/vocab.bin  tokenizer/config.bin
├── kernel/
│   ├── kernel_main.c
│   ├── [gdt/idt/apic/pit/vga/serial/panic/keyboard/mouse]
│   ├── [pmm/vmm/heap/pci/ahci/fat32/initrd/task/sched/kthread/sync/acpi/simd]
│   ├── fs/  vfs.c  vfs.h  vfs_initrd.c
│   ├── shell/  terminal.c  shell.c          ← ✅ startx
│   ├── gfx/  framebuffer.c  font.c  colors.h
│   ├── gui/
│   │   ├── input.c / input.h              ← ✅
│   │   ├── input_wiring.c / input_wiring.h ← ✅ Phase 10.3
│   │   ├── window.c / window.h            ← ✅
│   │   ├── wm.c / wm.h                    ← ✅ drag+resize
│   │   ├── desktop.c / desktop.h          ← ✅
│   │   ├── taskbar.c / taskbar.h          ← ✅
│   │   └── start_menu.c / start_menu.h    ← ✅
│   ├── llm/
│   │   ├── tensor.c / tensor.h            ← ✅ 7.1
│   │   ├── ops.c / ops.h                  ← ✅ 7.2
│   │   ├── attention.c / attention.h      ← ✅ 7.3
│   │   ├── transformer.c / transformer.h  ← ✅ 7.4
│   │   ├── model.c / model.h              ← ⬜ NEXT 7.5
│   │   ├── loader.c / loader.h            ← ⬜ 7.6
│   │   ├── tokenizer.c / tokenizer.h      ← ⬜ 7.7
│   │   ├── quant.c                        ← ⬜ 7.8
│   │   └── inference.c                    ← ⬜ 7.9
│   └── apps/  (all Phase 11 — not started)
└── docs/
```

---

*Last updated: May 2026 — Phase 10.3 complete (GUI input wiring: `input_wiring.c/h`, `keyboard_set_gui_callback`, `mouse_set_gui_callback`). Phase 10.6 complete (`startx` in `shell.c`: activates wiring, spawns `gui_wm` kthread). GUI subsystem fully wired end-to-end. Next: Phase 7.5 — full model forward pass (`kernel/llm/model.c`).*

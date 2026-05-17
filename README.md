<div align="center">

# ⚡ AIOS
### Autonomous Intelligent Operating System

*A bare-metal OS with a local LLM as its core intelligence engine*

[![License](https://img.shields.io/badge/license-Proprietary-red.svg)](./LICENSE)
[![Phase](https://img.shields.io/badge/phase-11%20%E2%80%94%20GUI%20Apps-brightgreen.svg)](#roadmap-progress)
[![Architecture](https://img.shields.io/badge/arch-x86__64-blue.svg)](#)
[![Language](https://img.shields.io/badge/language-C%20%2F%20Assembly-orange.svg)](#)

</div>

---

## Vision

> Build a bare-metal Operating System from scratch that runs a local LLM as its core logic engine — eliminating reliance on external APIs and creating a truly **autonomous, intelligent computer system**.

AIOS is not an OS that runs AI applications. AIOS *is* the AI. The kernel, scheduler, memory manager, and shell are all designed around a single goal: running a local language model directly on bare hardware with no userland dependencies.

---

## Roadmap Progress

| Phase | Name | Deliverable | Version | Status |
|-------|------|-------------|---------|--------|
| 0 | Toolchain & Boot | Build system + GRUB + GDT | v0.0.1 | ✅ Complete |
| 1 | Foundation | IDT + APIC + PIT + VGA + Serial + Keyboard + Mouse | v0.1.0 | ✅ Complete |
| 2 | Core Systems | PMM + VMM + Heap Allocator | v0.2.0 | ✅ Complete |
| 3 | Storage | PCI + AHCI + FAT32 + VFS + Initrd | v0.3.0 | ✅ Complete |
| 4 | Multitasking | Task system + Scheduler + kthreads + Sync primitives | v0.5.0 | ✅ Complete |
| 5 | Shell & ACPI | Terminal emulator + Shell + ACPI power management | v0.6.0 | ✅ Complete |
| 6 | SIMD | CPUID feature detect + AVX2 matmul/softmax/GELU | v0.7.0 | ✅ Complete |
| 7 | AI Layer | Tensor ops + Attention + Transformer + Loader + Tokenizer + Quant + Inference | v0.8.0 | ✅ Complete |
| 8 | Training | Autograd + Optimizer + Data pipeline | v0.9.0 | ⬜ |
| 9 | Network | e1000 NIC + TCP/IP stack | v0.9.5 | ⬜ |
| 10 | GUI | Framebuffer + Font + Input + WM + Desktop + Taskbar + Start Menu | v1.0.0 | ✅ Complete |
| 11 | GUI Apps | Notepad + Explorer + Terminal + Settings + AI Chat | v1.1.0 | ✅ Complete |
| 12 | Security | Stack canaries + KASLR + SMEP/SMAP + Secure boot | v1.2.0 | ⬜ |

---

## Architecture

```
┌─────────────────────────────────────────────────┐
│              GUI Apps (Phase 11) ✅              │
│  Notepad │ Explorer │ Terminal │ Settings │ Chat │
├─────────────────────────────────────────────────┤
│          Window Manager + Desktop (Phase 10) ✅   │
│   WM thread │ Drag/Resize │ Taskbar │ Start Menu │
├─────────────────────────────────────────────────┤
│              Local LLM Engine (Phase 7) ✅        │
│   Tokenizer │ Transformer │ AVX2 MatMul │ GGUF   │
├─────────────────────────────────────────────────┤
│           Multitasking + Scheduler (Phase 4) ✅   │
│     PCB │ Context Switch │ kthreads │ Sync       │
├─────────────────────────────────────────────────┤
│         Shell + Terminal + ACPI (Phase 5) ✅      │
│   readline │ history │ builtins │ reboot/shutdown │
├─────────────────────────────────────────────────┤
│              File System (Phase 3) ✅             │
│     FAT32 │ VFS │ AHCI │ PCI │ Initrd (ARDS)     │
├─────────────────────────────────────────────────┤
│           Core Kernel Systems (Phase 1-2) ✅      │
│   PMM │ VMM │ kmalloc │ APIC │ PIT │ VGA │ Serial│
├─────────────────────────────────────────────────┤
│            Foundation — Phase 0 ✅                │
│   Multiboot2 │ GDT │ IDT │ Build system │ GRUB   │
└─────────────────────────────────────────────────┘
               x86_64 Bare Metal Hardware
```

---

## What's Built

### `boot/`
- **`kernel_entry.asm`** — Multiboot2 header, 16 KiB stack, SSE/SSE2 enable, calls `kernel_main`
- **`linker.ld`** — Linker script: kernel loaded at 1 MiB physical
- **`grub.cfg`** — GRUB2 boot menu with initrd module

### `kernel/` — Core
| File | Description |
|------|-------------|
| `kernel_main.c` | Entry point — initializes all subsystems, boots GUI WM thread |
| `gdt.c` | GDT with null/kcode/kdata/ucode/udata + TSS, far-jump CS reload |
| `idt.c` / `isr_stubs.asm` | IDT (256 gates), exception register dump |
| `apic.c` | Local APIC + IOAPIC, legacy PIC disabled, EOI |
| `pit.c` | 1000 Hz timer, `pit_sleep_ms`, tick counter |
| `vga.c` | Full VGA text driver (80×25, 16 colors, scroll, hw cursor, putchar_at) |
| `serial.c` | COM1 115200 baud, `klog` macros |
| `panic.c` | VGA red screen + serial output + cli+hlt |
| `keyboard.c` | PS/2 keyboard, scancode→ASCII, ring buffer, E0 extended keys |
| `mouse.c` | PS/2 mouse, 3-byte packets, VGA cursor, ring buffer |

### `kernel/` — Memory
| File | Description |
|------|-------------|
| `pmm.c` | Physical memory manager — MB2 mmap, bitmap allocator |
| `vmm.c` | Virtual memory manager — 4-level paging, 64 MB identity map |
| `heap.c` | Heap allocator — free-list, coalesce, canary, aligned alloc |

### `kernel/` — Storage
| File | Description |
|------|-------------|
| `pci.c` | PCI enumeration, bus-master DMA enable |
| `ahci.c` | SATA driver — HBA init, port detect, DMA read/write |
| `fat32.c` | FAT32 filesystem — BPB, cluster chains, LFN, read/write/create |
| `initrd.c` / `mb2_modules.c` | Ramdisk — ARDS format, MB2 module parse |
| `fs/vfs.c` / `fs/vfs_initrd.c` | VFS abstraction — open/read/close, initrd shim |

### `kernel/` — Multitasking
| File | Description |
|------|-------------|
| `task.c` | Task system — pid, states, create/destroy |
| `switch_context.asm` | NASM callee-saved register swap |
| `sched.c` | Round-robin scheduler — tick, sleep, yield, exit, idle |
| `kthread.c` | Kernel thread API — create, exit, join |
| `sync.c` | Spinlock (IRQ-safe), mutex (yield-spin), semaphore (counting) |

### `kernel/` — Shell & ACPI
| File | Description |
|------|-------------|
| `shell/terminal.c` | Terminal — SPSC ring, readline, line editor, history×32, ANSI |
| `shell/shell.c` | Shell — prompt, tokenizing, builtins (help, clear, echo, mem, ps, ls, cat, hexdump, load, ai, chat, reboot, shutdown) |
| `acpi.c` | ACPI — RSDP scan, RSDT/XSDT, FADT, shutdown/reboot |

### `kernel/` — LLM Engine
| File | Description |
|------|-------------|
| `llm/tensor.c` | Tensor abstraction — alloc/free/reshape/slice |
| `llm/ops.c` | Math ops — matmul, add, scale, softmax, layer norm, GELU, embedding, RoPE |
| `llm/attention.c` | Multi-head attention with KV-cache |
| `llm/transformer.c` | GPT-2 / LLaMA transformer blocks |
| `llm/model.c` | Full forward pass, greedy/top-k/top-p sampling |
| `llm/loader.c` | Weight file loader — FP16/FP32/Q4_K_M |
| `llm/tokenizer.c` | BPE tokenizer — encode/decode, special tokens |
| `llm/quant.c` | Q8_0 / Q4_K dequantize, mixed-precision matmul |
| `llm/inference.c` | Inference manager — kthread, prompt, streaming tokens |

### `kernel/` — GUI (Phase 10-11)
| File | Description |
|------|-------------|
| `gfx/framebuffer.c` | Linear framebuffer — MB2 tag parse, put_pixel, fill_rect, blit |
| `gfx/font.c` | Font rendering — builtin 8×16 debug font, string draw, centered |
| `gfx/colors.h` | UI color constants |
| `gui/input.c` | GUI event queue — mouse/keyboard → high-level events, double-click |
| `gui/input_bridge.c` | Input bridge — raw driver → GUI event conversion |
| `gui/input_wiring.c` | Input wiring — keyboard/mouse driver hooks |
| `gui/input_mode.c` | Input mode switching — text mode ↔ GUI mode |
| `gui/window.c` | Window system — z-ordered doubly-linked list, create/destroy |
| `gui/wm.c` | Window manager thread — drag, resize, redraw, cursor |
| `gui/desktop.c` | Desktop — gradient background + AIOS watermark |
| `gui/taskbar.c` | Taskbar — Start button, window buttons, uptime clock |
| `gui/start_menu.c` | Start menu — app launcher (Notepad, Explorer, Terminal, Settings, AI Chat) |
| `apps/notepad.c` | Notepad — gap buffer editor, File menu, VFS save/open |
| `apps/explorer.c` | File Explorer — two-pane VFS browser, navigation |
| `apps/terminal_gui.c` | GUI Terminal — windowed shell |
| `apps/settings.c` | Settings — configuration window |
| `apps/ai_chat.c` | AI Chat — LLM inference front-end with streaming |

---

## Quick Start

### Prerequisites

```bash
# Ubuntu / WSL2 (recommended)
sudo apt install nasm qemu-system-x86 grub-pc-bin xorriso mtools build-essential

# You must build a cross-compiler:
# https://wiki.osdev.org/GCC_Cross-Compiler
# Target: x86_64-elf-gcc
```

### Build & Run

```bash
git clone https://github.com/hillaryns/AIOS..git
cd AIOS.

# Install system deps
./build.sh deps

# Build ISO + launch in QEMU
./build.sh run

# Debug with GDB
./build.sh debug

# Build ISO only
./build.sh iso

# Clean artifacts
./build.sh clean
```

### Expected Output

```
======================================================
   AIOS — Autonomous Intelligent Operating System
   Phase 1: Foundation
======================================================

  [ OK ] Multiboot2 handoff verified
  [ OK ] GDT loaded (null / kernel-code / kernel-data / user-code / user-data)
  [ OK ] IDT loaded, PIC remapped (IRQ0-15 → INT 0x20-0x2F)
  [ OK ] STI — interrupts enabled

  AIOS Initialized

  Roadmap Phase 1 complete. Ready for Phase 2 (Memory Management).
```

---

## Repository Structure

```
AIOS/
├── README.md
├── ROADMAP.md
├── LICENSE
├── Makefile                    ← x86_64-elf cross-compiler build system
├── build.sh                    ← Quick-start: deps / run / debug / clean
├── .github/
│   └── workflows/
│       └── roadmap-check.yml   ← CI: warns if kernel/boot changed without ROADMAP update
├── boot/
│   ├── kernel_entry.asm        ← Multiboot2 + stack + SSE + kernel_main call
│   ├── linker.ld               ← Memory layout (kernel @ 1 MiB)
│   └── grub.cfg                ← GRUB2 config (with initrd module)
├── kernel/
│   ├── kernel_main.c           ← Kernel entry point (Phase 10.4+)
│   ├── gdt.c / idt.c / apic.c / pit.c
│   ├── vga.c / serial.c / panic.c / pf_handler.c
│   ├── keyboard.c / mouse.c
│   ├── pmm.c / vmm.c / heap.c
│   ├── pci.c / ahci.c / fat32.c
│   ├── initrd.c / mb2_modules.c
│   ├── task.c / sched.c / kthread.c / sync.c
│   ├── switch_context.asm / isr_stubs.asm
│   ├── simd.c                  ← CPUID + AVX2 intrinsics
│   ├── acpi.c                  ← RSDP/FADT power management
│   ├── include/                ← Legacy headers
│   ├── fs/                     ← VFS + initrd shim
│   ├── shell/                  ← Terminal + Shell
│   ├── gfx/                    ← Framebuffer + Font + Colors
│   ├── gui/                    ← Input + WM + Desktop + Taskbar + Start Menu
│   ├── apps/                   ← Notepad + Explorer + Terminal + Settings + AI Chat
│   └── llm/                    ← Tensor + Ops + Attention + Transformer + Model + Loader + Tokenizer + Quant + Inference
├── scripts/
│   ├── check_deps.sh           ← Build dependency checker
│   └── mkinitrd.py             ← Ramdisk image builder (ARDS format)
├── assets/
│   ├── tokenizer/              ← vocab.bin + config.bin (Phase 7.7)
│   └── fonts/                  ← ⬜ TODO: bitmap font assets
└── docs/
```

---

## Tech Stack

| Layer | Technology |
|-------|------------|
| Assembly | NASM (Intel syntax, ELF64) |
| Kernel language | C (freestanding, `-ffreestanding -mno-red-zone`) |
| Cross-compiler | `x86_64-elf-gcc` (GCC/Binutils) |
| Boot protocol | Multiboot2 (GRUB2) |
| Emulator | QEMU (`qemu-system-x86_64`) |
| Debugger | GDB with QEMU remote stub |
| Target | x86_64 bare metal |

---

## Contributing

This repository is **proprietary**. Contributions are limited to authorized maintainers only. See [`LICENSE`](./LICENSE) for full terms.

If you are an authorized maintainer:
1. Fork the repository
2. Create a branch: `git checkout -b feature/your-feature`
3. Commit with conventional commits: `feat:`, `fix:`, `docs:`, `refactor:`
4. Open a Pull Request targeting `hillaryns/AIOS.`

---

## Maintainers

| Name | GitHub |
|------|--------|
| Hillary | [@hillaryns](https://github.com/hillaryns) |
| Subhobhai | [@subhobhai943](https://github.com/subhobhai943) |
| Subham | [@SUBHAM646](https://github.com/SUBHAM646) |

---

## License

Copyright © 2026 AIOS Maintainers (Hillary NS, Subhobhai Sarkar, Subham). All rights reserved.

This software is proprietary and confidential. See [`LICENSE`](./LICENSE) for full terms. Unauthorized use, reproduction, or distribution is strictly prohibited.

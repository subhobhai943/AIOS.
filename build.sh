#!/usr/bin/env bash
# ============================================================
# AIOS — Quick Build Script
# Installs dependencies, builds the cross-compiler (if needed),
# compiles AIOS Phase 1, and launches in QEMU.
#
# Usage:
#   chmod +x build.sh
#   ./build.sh          # build + run in QEMU
#   ./build.sh iso      # build ISO only
#   ./build.sh clean    # clean build artifacts
# ============================================================

set -e

ACTION=${1:-run}

# ── Detect cross-compiler ─────────────────────────────────
check_crosscompiler() {
    if ! command -v x86_64-elf-gcc &>/dev/null; then
        echo "[!] x86_64-elf-gcc not found."
        echo "    Build it from: https://wiki.osdev.org/GCC_Cross-Compiler"
        echo "    Or install via: brew install x86_64-elf-gcc  (macOS)"
        echo "    On Arch Linux:  yay -S cross-x86_64-elf-gcc"
        exit 1
    fi
}

# ── Install build deps (Debian/Ubuntu) ────────────────────
install_deps() {
    echo "[*] Installing build dependencies..."
    sudo apt-get install -y \
        nasm \
        qemu-system-x86 \
        grub-pc-bin \
        xorriso \
        mtools \
        build-essential
}

# ── Main ──────────────────────────────────────────────────
case "$ACTION" in
    deps)
        install_deps
        ;;
    clean)
        make clean
        echo "[*] Build artifacts cleaned."
        ;;
    iso)
        check_crosscompiler
        make iso
        echo "[*] ISO built: aios.iso"
        ;;
    run)
        check_crosscompiler
        make iso
        echo "[*] Launching AIOS in QEMU..."
        qemu-system-x86_64 \
            -cdrom aios.iso \
            -m 512M \
            -vga std \
            -display gtk,grab-on-hover=on,show-tabs=off \
            -serial stdio \
            -machine type=pc,accel=tcg \
            -device ps2-kbd \
            -device ps2-mouse \
            -no-reboot \
            -no-shutdown
        ;;
    debug)
        check_crosscompiler
        make iso
        echo "[*] Starting QEMU with GDB stub on :1234..."
        qemu-system-x86_64 \
            -cdrom aios.iso \
            -m 512M \
            -vga std \
            -display gtk,grab-on-hover=on,show-tabs=off \
            -machine type=pc,accel=tcg \
            -device ps2-kbd \
            -device ps2-mouse \
            -s -S \
            -no-reboot \
            -no-shutdown &
        gdb -ex "target remote :1234" \
            -ex "symbol-file build/kernel.bin" \
            -ex "break kernel_main"
        ;;
    *)
        echo "Usage: $0 [deps|iso|run|debug|clean]"
        exit 1
        ;;
esac

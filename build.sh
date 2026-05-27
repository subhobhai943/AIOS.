#!/usr/bin/env bash
# ============================================================
# AIOS — Quick Build Script
# Installs dependencies, builds the cross-compiler (if needed),
# compiles AIOS, and launches in QEMU.
#
# Usage:
#   chmod +x build.sh
#   ./build.sh          # build + run in QEMU
#   ./build.sh iso      # build ISO only
#   ./build.sh clean    # clean build artifacts
#   ./build.sh deps     # install build dependencies
#   ./build.sh debug    # build + run with GDB stub on :1234
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

# ── Detect QEMU ───────────────────────────────────────────
check_qemu() {
    if ! command -v qemu-system-x86_64 &>/dev/null; then
        echo "[!] qemu-system-x86_64 not found."
        echo "    Install via: sudo apt-get install qemu-system-x86  (Debian/Ubuntu)"
        echo "                 sudo dnf install qemu-system-x86      (Fedora)"
        echo "                 brew install qemu                      (macOS)"
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
        python3 \
        build-essential
}

# ── Resolve grub-mkrescue (name differs across distros) ───
GRUB_MKRESCUE=$(command -v grub-mkrescue 2>/dev/null || \
                command -v grub2-mkrescue 2>/dev/null || true)
if [ -z "$GRUB_MKRESCUE" ]; then
    echo "[!] Neither grub-mkrescue nor grub2-mkrescue found."
    echo "    Run: ./build.sh deps   (Debian/Ubuntu)"
    echo "    Or install the grub tools for your distro manually."
    # Don't exit here — the check only matters for iso/run/debug actions.
fi

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
        check_qemu
        make iso
        echo "[*] Launching AIOS in QEMU..."
        # -no-reboot  : keep QEMU open if the kernel triple-faults
        # -no-shutdown: prevent VM auto-poweroff on halt instruction
        # -serial stdio: pipe kernel serial output to this terminal
        qemu-system-x86_64 \
            -cdrom aios.iso \
            -m 512M \
            -vga std \
            -serial stdio \
            -display gtk,grab-on-hover=on,show-tabs=off \
            -machine type=pc,accel=tcg \
            -device ps2-kbd \
            -device ps2-mouse \
            -no-reboot \
            -no-shutdown
        ;;
    debug)
        check_crosscompiler
        check_qemu
        make iso
        echo "[*] Starting QEMU with GDB stub on :1234..."
        qemu-system-x86_64 \
            -cdrom aios.iso \
            -m 512M \
            -vga std \
            -serial stdio \
            -display gtk,grab-on-hover=on,show-tabs=off \
            -machine type=pc,accel=tcg \
            -device ps2-kbd \
            -device ps2-mouse \
            -no-reboot \
            -no-shutdown \
            -s -S &
        gdb -ex "target remote :1234" \
            -ex "symbol-file build/kernel.bin" \
            -ex "break kernel_main"
        ;;
    *)
        echo "Usage: $0 [deps|iso|run|debug|clean]"
        exit 1
        ;;
esac

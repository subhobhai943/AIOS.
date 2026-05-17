#!/usr/bin/env bash
# ============================================================
# AIOS — Dependency Checker
# Run this before your first build to verify all required
# tools are installed and on PATH.
# ============================================================

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

MISSING=0

check() {
    local name="$1"
    local cmd="$2"
    local hint="$3"

    if command -v "$cmd" &>/dev/null; then
        local ver
        ver=$("$cmd" --version 2>&1 | head -n1)
        printf "  ${GREEN}[OK]${NC}  %-28s %s\n" "$name" "$ver"
    else
        printf "  ${RED}[MISSING]${NC} %-24s ${YELLOW}$hint${NC}\n" "$name"
        MISSING=$((MISSING + 1))
    fi
}

check_optional() {
    local name="$1"
    local cmd="$2"
    local hint="$3"

    if command -v "$cmd" &>/dev/null; then
        local ver
        ver=$("$cmd" --version 2>&1 | head -n1)
        printf "  ${GREEN}[OK]${NC}  %-28s %s\n" "$name" "$ver"
    else
        printf "  ${YELLOW}[OPTIONAL]${NC} %-21s ${YELLOW}$hint${NC}\n" "$name"
    fi
}

echo
echo "AIOS Build Dependency Check"
echo "============================="
echo

check "x86_64-elf-gcc" "x86_64-elf-gcc" \
    "Cross-compiler required. Build from source or install via: brew install x86_64-elf-gcc / apt install gcc-x86-64-linux-gnu"

check "x86_64-elf-ld"  "x86_64-elf-ld"  \
    "Cross-linker (ships with binutils). Build binutils targeting x86_64-elf."

check "nasm"           "nasm"           \
    "Netwide Assembler: sudo apt install nasm / brew install nasm"

check "qemu-system-x86_64" "qemu-system-x86_64" \
    "QEMU emulator: sudo apt install qemu-system-x86 / brew install qemu"

check "grub-mkrescue"  "grub-mkrescue"  \
    "GRUB ISO builder: sudo apt install grub-pc-bin xorriso / brew install grub"

check "xorriso"        "xorriso"        \
    "ISO creation tool used by grub-mkrescue: sudo apt install xorriso / brew install xorriso"

check_optional "gdb"   "gdb"            \
    "GNU Debugger (optional but recommended): sudo apt install gdb"

check "make"           "make"           \
    "GNU Make: sudo apt install make / brew install make"

echo
if [ "$MISSING" -eq 0 ]; then
    printf "${GREEN}All dependencies satisfied. You're ready to build AIOS!${NC}\n"
    echo "  Run: make iso && make run"
else
    printf "${RED}$MISSING dependency/dependencies missing. Install them and re-run this script.${NC}\n"
    exit 1
fi
echo

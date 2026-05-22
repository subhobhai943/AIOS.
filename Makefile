# ============================================================
# AIOS — Build System
# Phase 11: GUI Apps + Shell
# Cross-compiler target: x86_64-elf
# ============================================================

CC      := x86_64-elf-gcc
AS      := nasm
LD      := x86_64-elf-ld

CFLAGS  := -ffreestanding -O2 -Wall -Wextra -fno-exceptions \
           -fno-stack-protector -mno-red-zone -mcmodel=kernel -fno-pic \
           -I./kernel -I./kernel/include

LDFLAGS := -T boot/linker.ld -nostdlib

ASFLAGS_BIN  := -f bin
ASFLAGS_ELF  := -f elf64

# Directories
BUILD   := build
ISO     := aios.iso
INITRD  := boot/initrd.img

# Sources
# Exclude full notepad.c and ai_chat.c — _simple variants are used instead.
# Exclude vga_phase51.c — its symbols are already in vga.c.
C_SRCS  := $(shell find kernel -type f -name '*.c' \
           ! -path 'kernel/apps/notepad.c' \
           ! -path 'kernel/apps/ai_chat.c' \
           ! -path 'kernel/vga_phase51.c')
ASM_SRCS := $(shell find kernel -type f -name '*.asm')

C_OBJS  := $(patsubst kernel/%.c,  $(BUILD)/%.o, $(C_SRCS))
A_OBJS  := $(patsubst kernel/%.asm,$(BUILD)/%_asm.o, $(ASM_SRCS))

.PHONY: all clean iso run

all: $(BUILD)/kernel.bin

# ── Initrd ──────────────────────────────────────────────────
$(INITRD): scripts/mkinitrd.py assets/tokenizer/vocab.bin assets/tokenizer/config.bin
	python3 scripts/mkinitrd.py -o $@ \
		assets/tokenizer/vocab.bin /tokenizer/vocab.bin \
		assets/tokenizer/config.bin /tokenizer/config.bin

# ── Kernel objects ─────────────────────────────────────────
$(BUILD)/%.o: kernel/%.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%_asm.o: kernel/%.asm | $(BUILD)
	mkdir -p $(dir $@)
	$(AS) $(ASFLAGS_ELF) $< -o $@

# ── Boot object ────────────────────────────────────────────
$(BUILD)/kernel_entry_asm.o: boot/kernel_entry.asm | $(BUILD)
	$(AS) $(ASFLAGS_ELF) $< -o $@

# ── Link ───────────────────────────────────────────────────
$(BUILD)/kernel.bin: $(BUILD)/kernel_entry_asm.o $(C_OBJS) $(A_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# ── ISO (requires GRUB) ────────────────────────────────────
iso: $(BUILD)/kernel.bin $(INITRD)
	mkdir -p $(BUILD)/isodir/boot/grub
	cp $(BUILD)/kernel.bin $(BUILD)/isodir/boot/kernel.bin
	cp $(INITRD) $(BUILD)/isodir/boot/initrd.img
	cp boot/grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/isodir

# ── Run in QEMU ────────────────────────────────────────────
run: iso
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -vga std -serial stdio

# ── Debug in QEMU + GDB ────────────────────────────────────
debug: iso
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -vga std -s -S &
	gdb -ex "target remote :1234" -ex "symbol-file $(BUILD)/kernel.bin"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) $(ISO) $(INITRD)

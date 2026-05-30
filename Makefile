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
QEMU    ?= qemu-system-x86_64

# ── Disk image (64 MB FAT32 virtual SATA drive) ───────────
DISK_IMG := aios_disk.img
DISK_MB  := 64

# QEMU flags:
#   -cdrom        : bootable ISO (GRUB + kernel)
#   -drive / -device ahci: expose a raw disk image as a SATA drive so
#                 the AHCI driver can find an HBA at PCI class 0x01/0x06
#                 and port_mask != 0 → FAT32 mounts successfully.
#   -m 512M       : enough RAM for kernel heap + framebuffer
#   -vga std      : standard VGA for Multiboot2 framebuffer tag
#   -serial stdio : kernel serial log → host terminal
#   -no-reboot / -no-shutdown : keep window open on triple-fault
QEMUFLAGS = \
	-cdrom $(ISO) \
	-drive id=disk0,file=$(DISK_IMG),format=raw,if=none \
	-device ahci,id=ahci0 \
	-device ide-hd,drive=disk0,bus=ahci0.0 \
	-m 512M \
	-vga std \
	-serial stdio \
	-display gtk,grab-on-hover=on,show-tabs=off \
	-machine type=pc,accel=tcg \
	-no-reboot \
	-no-shutdown

# Directories
BUILD   := build
ISO     := aios.iso
INITRD  := boot/initrd.img

# Sources
# Exclude full notepad.c and ai_chat.c — _simple variants are used instead.
# Exclude vga_phase51.c — its symbols are already in vga.c.
C_SRCS  := $(shell find kernel -type f -name '*.c' \
           ! -path 'kernel/keyboard_gui_hook.c' \
           ! -path 'kernel/apps/notepad.c' \
           ! -path 'kernel/apps/ai_chat.c' \
           ! -path 'kernel/vga_phase51.c')
ASM_SRCS := $(shell find kernel -type f -name '*.asm')

C_OBJS  := $(patsubst kernel/%.c,  $(BUILD)/%.o, $(C_SRCS))
A_OBJS  := $(patsubst kernel/%.asm,$(BUILD)/%_asm.o, $(ASM_SRCS))

.PHONY: all clean iso run debug disk

all: $(BUILD)/kernel.bin

# ── Disk image (FAT32, 64 MB) ──────────────────────────────
# Creates aios_disk.img with a single FAT32 partition.
# Requires: dd, mkfs.fat (dosfstools), mtools (for mcopy).
# The TEST.TXT is written so the VFS smoke-test in kernel_main
# can verify file reads work end-to-end.
disk: $(DISK_IMG)

$(DISK_IMG):
	@echo "[DISK] Creating $(DISK_MB) MB FAT32 disk image: $(DISK_IMG)"
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=$(DISK_MB) status=progress
	mkfs.fat -F 32 -n AIOS $(DISK_IMG)
	echo "AIOS VFS smoke-test OK" | mcopy -i $(DISK_IMG) - ::TEST.TXT
	@echo "[DISK] Done — $(DISK_IMG) ready"

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
	$(shell command -v grub-mkrescue || command -v grub2-mkrescue) \
		--compress=none -o $(ISO) $(BUILD)/isodir 2>&1

# ── Run in QEMU ────────────────────────────────────────────
# Run requires the disk image to exist first.
# If $(DISK_IMG) is missing, create it automatically.
run: iso $(DISK_IMG)
	$(QEMU) $(QEMUFLAGS)

# ── Debug in QEMU + GDB ────────────────────────────────────
debug: iso $(DISK_IMG)
	$(QEMU) $(QEMUFLAGS) -s -S &
	gdb -ex "target remote :1234" -ex "symbol-file $(BUILD)/kernel.bin"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) $(ISO) $(INITRD)
	# NOTE: $(DISK_IMG) is intentionally NOT removed by clean.
	#       It is slow to recreate (mkfs + mcopy). Run 'rm $(DISK_IMG)' manually.

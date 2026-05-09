; ============================================================
; AIOS — Kernel Entry
; Multiboot2 → Protected Mode → Long Mode → kernel_main
;
; Checklist implemented here:
;   ✅ Multiboot2 magic + valid header (type/flags/size checksum)
;   ✅ Valid 16 KB stack set up before C call
;   ✅ Multiboot2 info pointer passed in RDI to kernel_main
;   ✅ SSE / SSE2 enabled (CR0 + CR4) — required for float/LLM ops
;   ✅ GDT with null / kernel-code / kernel-data descriptors
;   ✅ Far jump to reload CS, then into 64-bit long_mode_entry
; ============================================================

bits 32

; ============================================================
; Multiboot2 header
; Must be in first 32 KB of the image, 8-byte aligned.
; Layout: magic, architecture, header_length, checksum,
;         then a list of tags terminated by the end tag.
; ============================================================
section .multiboot2
align 8

MB2_MAGIC       equ 0xE85250D6
MB2_ARCH_I386   equ 0           ; 32-bit protected mode of i386

header_start:
    dd MB2_MAGIC
    dd MB2_ARCH_I386
    dd header_end - header_start        ; header_length
    dd -(MB2_MAGIC + MB2_ARCH_I386 + (header_end - header_start))  ; checksum

    ; --- End tag (required) ---
    dw 0    ; type  = 0 (end)
    dw 0    ; flags = 0
    dd 8    ; size  = 8
header_end:


; ============================================================
; Protected-mode (32-bit) startup
; ============================================================
section .text
global kernel_entry
extern kernel_main

kernel_entry:
    cli

    ; Save multiboot2 info pointer (EBX) — we'll pass it to kernel_main
    ; via RDI once we reach 64-bit mode.  Store in a static slot for now.
    mov [mb2_info_ptr], ebx

    ; -------------------------------------------------------
    ; Set up a temporary stack (16 KB, 16-byte aligned)
    ; -------------------------------------------------------
    mov esp, stack_top

    ; -------------------------------------------------------
    ; Build identity-map for first 2 MB via a single 2 MB
    ; huge page (PML4 → PDPT → PD).
    ; -------------------------------------------------------
    ; PML4[0] → pdpt_table
    mov eax, pdpt_table
    or  eax, 0x03           ; P + RW
    mov [pml4_table], eax

    ; PDPT[0] → pd_table
    mov eax, pd_table
    or  eax, 0x03
    mov [pdpt_table], eax

    ; PD[0]  → 0x000000 (2 MB huge page, P + RW + PS)
    mov dword [pd_table], 0x00000083

    ; -------------------------------------------------------
    ; Enable PAE (CR4.PAE)
    ; -------------------------------------------------------
    mov eax, cr4
    or  eax, (1 << 5)       ; PAE
    mov cr4, eax

    ; -------------------------------------------------------
    ; Load PML4 into CR3
    ; -------------------------------------------------------
    mov eax, pml4_table
    mov cr3, eax

    ; -------------------------------------------------------
    ; Enable Long Mode via EFER.LME (MSR 0xC0000080)
    ; -------------------------------------------------------
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)       ; LME
    wrmsr

    ; -------------------------------------------------------
    ; Enable Paging (CR0.PG) + Protected Mode (already set)
    ; CR0.MP must be set (bit 1) and CR0.EM must be CLEAR
    ; (bit 2) so that SSE instructions reach the FPU/SSE
    ; hardware rather than triggering a #NM exception.
    ; -------------------------------------------------------
    mov eax, cr0
    or  eax, (1 << 31)      ; PG — enable paging → activates long mode
    or  eax, (1 << 1)       ; MP — monitor co-processor
    and eax, ~(1 << 2)      ; clear EM — not emulated
    mov cr0, eax

    ; -------------------------------------------------------
    ; Load our minimal 64-bit GDT and far-jump to long mode
    ; -------------------------------------------------------
    lgdt [gdt64.pointer]
    jmp  0x08:long_mode_entry


; ============================================================
; 64-bit entry point
; ============================================================
bits 64

long_mode_entry:
    ; Reload all segment registers with the data descriptor
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Reload RSP — the 16 KB stack is already set up
    mov rsp, stack_top

    ; -------------------------------------------------------
    ; Enable SSE / SSE2 via CR4.OSFXSR + CR4.OSXMMEXCPT
    ; This must be done AFTER entering 64-bit mode.
    ; Without this, any SSE instruction in kernel C code will
    ; cause a #UD (invalid opcode) exception.
    ; -------------------------------------------------------
    mov rax, cr4
    or  rax, (1 << 9)       ; OSFXSR  — OS supports FXSAVE/FXRSTOR
    or  rax, (1 << 10)      ; OSXMMEXCPT — OS handles SSE exceptions
    mov cr4, rax

    ; -------------------------------------------------------
    ; Pass multiboot2 info pointer as first argument (RDI)
    ; per System V AMD64 ABI calling convention.
    ; -------------------------------------------------------
    mov edi, dword [mb2_info_ptr]   ; zero-extended into RDI

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang


; ============================================================
; Paging structures  (BSS — zeroed by loader/GRUB)
; ============================================================
section .bss
align 4096

pml4_table: resb 4096
pdpt_table: resb 4096
pd_table:   resb 4096


; ============================================================
; Stack  (16 KB, 16-byte aligned)
; ============================================================
align 16
stack_bottom:   resb 16384
stack_top:


; ============================================================
; Small data slot for multiboot2 pointer
; ============================================================
section .data
align 4
mb2_info_ptr:   dd 0


; ============================================================
; Minimal 64-bit GDT
;   0x00 — null descriptor
;   0x08 — 64-bit kernel code  (L=1, P=1, DPL=0)
;   0x10 — 64-bit kernel data  (P=1, DPL=0, S=1, W=1)
; ============================================================
section .rodata
align 8

gdt64:
    ; null
    dq 0x0000000000000000
    ; kernel code: L(53)=1, P(47)=1, S(44)=1, E(43)=1, RW(41)=1
    dq 0x00AF9A000000FFFF
    ; kernel data: P(47)=1, S(44)=1, RW(41)=1
    dq 0x00AF92000000FFFF

.pointer:
    dw .pointer - gdt64 - 1     ; limit
    dq gdt64                    ; base

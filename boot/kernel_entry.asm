; ============================================================
; AIOS — Kernel Entry
; Multiboot2 → Protected Mode → Long Mode → kernel_main
;
; Checklist implemented here:
;   ✅ Multiboot2 magic + valid header (type/flags/size checksum)
;   ✅ Valid 16 KB stack set up before C call
;   ✅ Multiboot2 magic  passed in RDI (arg 1) to kernel_main
;   ✅ Multiboot2 info ptr passed in RSI (arg 2) to kernel_main
;   ✅ SSE / SSE2 enabled (CR0 + CR4) — required for float/LLM ops
;   ✅ GDT with null / kernel-code / kernel-data descriptors
;   ✅ Far jump to reload CS, then into 64-bit long_mode_entry
;
; FIX (May 2026): Previously only RDI was set (the info ptr).
; The Multiboot2 magic (EAX = 0x36D76289) was never forwarded
; to kernel_main, so the magic check always failed, PMM was
; skipped, total_frames stayed 0, pmm_alloc_page() returned
; PMM_ALLOC_FAIL on the very first call, and vmm_init() halted
; with "out of memory for PML4".
;
; System V AMD64 ABI argument registers:
;   arg1 = RDI  ← multiboot2 magic  (was in EAX at entry)
;   arg2 = RSI  ← multiboot2 info pointer (was in EBX at entry)
; ============================================================

bits 32

; ============================================================
; Multiboot2 header
; Must be in first 32 KB of the image, 8-byte aligned.
; ============================================================
section .multiboot2
align 8

MB2_MAGIC       equ 0xE85250D6
MB2_ARCH_I386   equ 0

header_start:
    dd MB2_MAGIC
    dd MB2_ARCH_I386
    dd header_end - header_start
    dd -(MB2_MAGIC + MB2_ARCH_I386 + (header_end - header_start))

    ; Request a 32-bit linear framebuffer for the GUI.
    dw 5
    dw 0
    dd 20
    dd 1024
    dd 768
    dd 32
align 8

    ; End tag (required)
    dw 0
    dw 0
    dd 8
header_end:


; ============================================================
; Protected-mode (32-bit) startup
; ============================================================
section .text
global kernel_entry
extern kernel_main

kernel_entry:
    cli

    ; -------------------------------------------------------
    ; GRUB / Multiboot2 hands us:
    ;   EAX = 0x36D76289  (multiboot2 boot magic)
    ;   EBX = physical address of multiboot2 info structure
    ;
    ; Save BOTH before we touch any registers.
    ; -------------------------------------------------------
    mov [mb2_magic_save],   eax
    mov [mb2_info_ptr],     ebx

    ; Temporary stack
    mov esp, stack_top

    ; -------------------------------------------------------
    ; Build identity-map for first 2 MB via a single 2 MB
    ; huge page (PML4 → PDPT → PD).
    ; -------------------------------------------------------
    mov eax, pdpt_table
    or  eax, 0x03
    mov [pml4_table], eax

    mov eax, pd_table
    or  eax, 0x03
    mov [pdpt_table], eax

    ; Identity-map the first 64 MB with 2 MB huge pages. The kernel's
    ; BSS can extend beyond 2 MB before vmm_init() installs its own map.
    xor ecx, ecx
.map_pd:
    mov eax, ecx
    shl eax, 21
    or  eax, 0x00000083                ; P+RW+PS
    mov [pd_table + ecx * 8], eax
    mov dword [pd_table + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 32
    jl .map_pd

    ; Enable PAE
    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    ; Load PML4
    mov eax, pml4_table
    mov cr3, eax

    ; Enable Long Mode (EFER.LME)
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)
    wrmsr

    ; Enable Paging + keep MP, clear EM
    mov eax, cr0
    or  eax, (1 << 31)
    or  eax, (1 << 1)
    and eax, ~(1 << 2)
    mov cr0, eax

    lgdt [gdt64.pointer]
    jmp  0x08:long_mode_entry


; ============================================================
; 64-bit entry point
; ============================================================
bits 64

long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, stack_top

    ; Enable SSE/SSE2
    mov rax, cr4
    or  rax, (1 << 9)    ; OSFXSR
    or  rax, (1 << 10)   ; OSXMMEXCPT
    mov cr4, rax

    ; -------------------------------------------------------
    ; Set up kernel_main arguments (System V AMD64 ABI):
    ;   RDI (arg1) = multiboot2 boot magic  (0x36D76289)
    ;   RSI (arg2) = multiboot2 info struct physical address
    ;
    ; Use zero-extending 32-bit loads so the upper 32 bits
    ; of RDI and RSI are cleanly zero.
    ; -------------------------------------------------------
    mov edi, dword [mb2_magic_save]  ; RDI = magic
    mov esi, dword [mb2_info_ptr]    ; RSI = info ptr

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang


; ============================================================
; BSS: boot page tables + stack
; ============================================================
section .bss
align 4096

pml4_table: resb 4096
pdpt_table: resb 4096
pd_table:   resb 4096

align 16
stack_bottom:   resb 16384
stack_top:


; ============================================================
; .data: saved multiboot2 values
; ============================================================
section .data
align 4
mb2_magic_save: dd 0
mb2_info_ptr:   dd 0


; ============================================================
; Minimal 64-bit GDT
; ============================================================
section .rodata
align 8

gdt64:
    dq 0x0000000000000000   ; null
    dq 0x00AF9A000000FFFF   ; kernel code (64-bit, L=1)
    dq 0x00AF92000000FFFF   ; kernel data

.pointer:
    dw .pointer - gdt64 - 1
    dq gdt64

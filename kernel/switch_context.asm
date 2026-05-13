; ================================================================
; AIOS — Low-level context switch (x86-64, System V ABI)
;
; void switch_context(uint64_t *curr_rsp_out, uint64_t next_rsp);
;                        RDI                   RSI
;
; Saves the current task's callee-saved registers onto the current
; stack, stores RSP into *curr_rsp_out, loads next_rsp into RSP,
; then restores the next task's callee-saved registers and returns
; (which jumps to the next task's entry point or resume point).
;
; Callee-saved registers per System V AMD64 ABI:
;   RBP, RBX, R12, R13, R14, R15
;
; IMPORTANT: -mno-red-zone is set on all kernel C code, so the
; 128-byte red zone below RSP is never used by C — safe to write
; below the current RSP here.
; ================================================================

section .text
global switch_context

switch_context:
    ; ---- Save current task's callee-saved regs onto current stack ----
    push    rbp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15

    ; ---- Save current RSP into *curr_rsp_out (RDI) -------------------
    mov     [rdi], rsp

    ; ---- Load next task's RSP (RSI) ----------------------------------
    mov     rsp, rsi

    ; ---- Restore next task's callee-saved regs from its stack --------
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp

    ; ---- Return to next task (ret pops the "return address" we pushed
    ;      in task_create, or the real return address if resuming)  ----
    ret

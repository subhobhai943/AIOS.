/* ================================================================
 * AIOS — Task Management
 * ================================================================ */

#include "task.h"
#include "heap.h"
#include "include/vga.h"
#include "serial.h"
#include "include/panic.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

extern void task_entry_trampoline(void);

/* ----------------------------------------------------------------
 * Static task table
 * ---------------------------------------------------------------- */
static task_t    g_tasks[TASK_MAX];
static uint32_t  g_next_pid  = 1;    /* 0 is reserved for the boot task */
static task_t   *g_current   = NULL;

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

static void kstrncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    while (i + 1 < n && src && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void kmemzero(void *p, size_t n)
{
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

/* ----------------------------------------------------------------
 * task_init
 * ---------------------------------------------------------------- */
void task_init(void)
{
    kmemzero(g_tasks, sizeof(g_tasks));
    g_next_pid = 1;

    /* Slot 0 = boot task (kernel_main context). */
    g_tasks[0].pid        = 0;
    g_tasks[0].state      = TASK_RUNNING;
    g_tasks[0].stack_base = NULL;   /* stack is the boot stack from entry.asm */
    g_tasks[0].stack_size = 0;
    g_tasks[0].cr3        = 0;      /* inherits current CR3 */
    g_tasks[0].next       = NULL;
    g_tasks[0].prev       = NULL;
    kstrncpy(g_tasks[0].name, "kmain", TASK_NAME_LEN);

    g_current = &g_tasks[0];

    klog("[TASK] init: boot task PID 0 registered\r\n");
}

/* ----------------------------------------------------------------
 * task_create
 * ---------------------------------------------------------------- */
task_t *task_create(void (*entry)(void), uint32_t stack_size, const char *name)
{
    /* Find a free slot. */
    task_t *t = NULL;
    for (int i = 1; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_DEAD) { t = &g_tasks[i]; break; }
    }
    if (!t) {
        klog("[TASK] create: no free task slots\r\n");
        return NULL;
    }

    /* Allocate stack (16-byte aligned, guard-friendly). */
    if (stack_size < 4096u) stack_size = 4096u;
    /* Round up to 16 bytes. */
    stack_size = (stack_size + 15u) & ~15u;

    void *stack = kmalloc(stack_size);
    if (!stack) {
        klog("[TASK] create: stack kmalloc failed\r\n");
        return NULL;
    }
    kmemzero(stack, stack_size);

    /* ----------------------------------------------------------------
     * Build the initial stack frame.
     *
     * switch_context pops: rbp, rbx, r12, r13, r14, r15, then ret.
     * So we push (in memory from high to low):
     *
     *   [stack_top - 8 ]  = entry_fn   ← popped by trampoline
     *   [stack_top - 16]  = trampoline ← popped by `ret`
     *   [stack_top - 24]  = r15 = 0
     *   [stack_top - 32]  = r14 = 0
     *   [stack_top - 40]  = r13 = 0
     *   [stack_top - 48]  = r12 = 0
     *   [stack_top - 56]  = rbx = 0
     *   [stack_top - 64]  = rbp = 0    ← RSP stored here
     * ---------------------------------------------------------------- */
    uint64_t stack_top = (uint64_t)(uintptr_t)stack + stack_size;
    /* x86-64 ABI: RSP must be 16-byte aligned before CALL; after CALL
     * (which pushes return address), RSP is 16n-8.  We're building a
     * synthetic frame so we match that: align stack_top to 16, then
     * subtract 8 to make room for the "return address".             */
    stack_top = stack_top & ~(uint64_t)15u;  /* align down to 16 */

    uint64_t *sp = (uint64_t *)stack_top;
    /* Push trampoline metadata. ret enters the trampoline, which pops entry. */
    sp--;  *sp = (uint64_t)(uintptr_t)entry;
    sp--;  *sp = (uint64_t)(uintptr_t)task_entry_trampoline;
    /* Push callee-saved registers (all zero). */
    sp--;  *sp = 0;   /* r15 */
    sp--;  *sp = 0;   /* r14 */
    sp--;  *sp = 0;   /* r13 */
    sp--;  *sp = 0;   /* r12 */
    sp--;  *sp = 0;   /* rbx */
    sp--;  *sp = 0;   /* rbp */

    /* Fill TCB. */
    kmemzero(t, sizeof(task_t));
    t->pid        = g_next_pid++;
    t->state      = TASK_READY;
    t->rsp        = (uint64_t)(uintptr_t)sp;
    t->cr3        = 0;   /* kernel shares PML4; filled in by vmm when needed */
    t->stack_base = stack;
    t->stack_size = stack_size;
    t->next       = NULL;
    t->prev       = NULL;
    t->wake_tick  = 0;
    t->ticks_run  = 0;
    kstrncpy(t->name, name ? name : "?", TASK_NAME_LEN);

    klog("[TASK] created PID ");
    klog_dec(t->pid);
    klog(" name=");
    klog(t->name);
    klog(" stack=");
    klog_hex((uint64_t)(uintptr_t)stack);
    klog(" rsp=");
    klog_hex(t->rsp);
    klog("\r\n");

    return t;
}

/* ----------------------------------------------------------------
 * task_destroy
 * ---------------------------------------------------------------- */
void task_destroy(task_t *t)
{
    if (!t || t->state == TASK_DEAD) return;
    if (t->stack_base) {
        kfree(t->stack_base);
        t->stack_base = NULL;
    }
    t->state = TASK_DEAD;
    klog("[TASK] destroyed PID ");
    klog_dec(t->pid);
    klog("\r\n");
}

/* ----------------------------------------------------------------
 * Current task accessors
 * ---------------------------------------------------------------- */
task_t *task_get_current(void) { return g_current; }
void    task_set_current(task_t *t) { g_current = t; }

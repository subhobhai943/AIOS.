/* ================================================================
 * AIOS — Round-Robin Preemptive Scheduler
 * ================================================================ */

#include "sched.h"
#include "task.h"
#include "heap.h"
#include "include/vga.h"
#include "include/panic.h"
#include "serial.h"

/* PIT tick counter (extern from pit.c). */
extern volatile uint64_t g_ticks;

/* Assembly context switch. */
extern void switch_context(uint64_t *curr_rsp_out, uint64_t next_rsp);

/* ----------------------------------------------------------------
 * Scheduler state
 * ---------------------------------------------------------------- */

/* Ready queue: circular doubly-linked list. */
static task_t *g_ready_head = NULL;   /* oldest / next-to-run     */
static int     g_ready_count = 0;

/* Sleep queue: singly-linked (via task->next used as sleep link). */
static task_t *g_sleep_head = NULL;

/* Remaining ticks for the current task's quantum. */
static uint32_t g_quantum_left = SCHED_TICK_QUANTUM;

/* ----------------------------------------------------------------
 * Idle task
 * ---------------------------------------------------------------- */
static void idle_task_fn(void)
{
    for (;;) __asm__ volatile ("hlt");
}

/* ----------------------------------------------------------------
 * Ready queue helpers (circular doubly-linked)
 * ---------------------------------------------------------------- */

/* Append task at tail of circular list. */
static void rq_push_back(task_t *t)
{
    t->next = NULL;
    t->prev = NULL;
    if (!g_ready_head) {
        g_ready_head = t;
        t->next = t;
        t->prev = t;
    } else {
        task_t *tail = g_ready_head->prev;
        tail->next       = t;
        t->prev          = tail;
        t->next          = g_ready_head;
        g_ready_head->prev = t;
    }
    g_ready_count++;
}

/* Remove task from circular list. */
static void rq_remove(task_t *t)
{
    if (g_ready_count == 0 || !t->next) return;
    if (t->next == t) {
        /* Only element. */
        g_ready_head = NULL;
    } else {
        t->prev->next = t->next;
        t->next->prev = t->prev;
        if (g_ready_head == t) g_ready_head = t->next;
    }
    t->next = NULL;
    t->prev = NULL;
    g_ready_count--;
}

/* Pop the front (oldest) task. */
static task_t *rq_pop_front(void)
{
    if (!g_ready_head) return NULL;
    task_t *t = g_ready_head;
    rq_remove(t);
    return t;
}

/* ----------------------------------------------------------------
 * Sleep queue helpers
 * ---------------------------------------------------------------- */

static void sq_push(task_t *t)
{
    t->next = g_sleep_head;
    g_sleep_head = t;
}

/* Wake all tasks whose wake_tick <= g_ticks. */
static void sq_wake_due(void)
{
    task_t **pp = &g_sleep_head;
    while (*pp) {
        task_t *t = *pp;
        if (g_ticks >= t->wake_tick) {
            *pp = t->next;          /* remove from sleep list */
            t->next  = NULL;
            t->prev  = NULL;
            t->state = TASK_READY;
            rq_push_back(t);
        } else {
            pp = &t->next;
        }
    }
}

/* ----------------------------------------------------------------
 * sched_init
 * ---------------------------------------------------------------- */
void sched_init(void)
{
    g_ready_head  = NULL;
    g_ready_count = 0;
    g_sleep_head  = NULL;
    g_quantum_left = SCHED_TICK_QUANTUM;

    /* The boot task (PID 0) is already RUNNING; it does NOT go on
     * the ready queue until it yields/sleeps.  The idle task is
     * always at the back of the ready queue as the last resort.   */

    /* Create idle task (lowest priority — always runnable). */
    task_t *idle = task_create(idle_task_fn, 4096, "idle");
    KERNEL_ASSERT(idle != NULL, "sched_init: idle task creation failed");
    rq_push_back(idle);

    vga_puts_color("  [ OK ] Scheduler: round-robin, quantum=",
                   VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_putdec(SCHED_TICK_QUANTUM);
    vga_puts(" ms, idle PID ");
    vga_putdec(idle->pid);
    vga_putchar('\n');
    klog("[SCHED] init complete, idle PID=");
    klog_dec(idle->pid);
    klog("\r\n");
}

/* ----------------------------------------------------------------
 * sched_add
 * ---------------------------------------------------------------- */
void sched_add(task_t *t)
{
    KERNEL_ASSERT(t != NULL, "sched_add: NULL task");
    t->state = TASK_READY;
    rq_push_back(t);
    klog("[SCHED] added PID ");
    klog_dec(t->pid);
    klog(" name=");
    klog(t->name);
    klog("\r\n");
}

/* ----------------------------------------------------------------
 * Internal: perform the actual context switch.
 * Called with interrupts disabled (from IRQ handler) or from
 * sched_yield (disables interrupts itself).
 * ---------------------------------------------------------------- */
static void do_switch(void)
{
    if (g_ready_count == 0) return;   /* only idle or nothing — no switch */

    task_t *curr = task_get_current();
    task_t *next = rq_pop_front();

    if (next == curr) {
        /* Only one runnable task (e.g. only idle). Put it back. */
        rq_push_back(next);
        return;
    }

    /* Move current task back to ready queue (unless it exited/blocked). */
    if (curr->state == TASK_RUNNING) {
        curr->state = TASK_READY;
        rq_push_back(curr);
    }

    /* Dispatch next task. */
    next->state = TASK_RUNNING;
    task_set_current(next);
    g_quantum_left = SCHED_TICK_QUANTUM;

    /* Perform the register-level context switch.  This either returns
     * to next's previous call site, or (first dispatch) jumps to its
     * entry function via the synthetic ret in switch_context.        */
    switch_context(&curr->rsp, next->rsp);
    /* We return here when curr is re-dispatched later. */
}

/* ----------------------------------------------------------------
 * sched_tick  — called from PIT IRQ handler (interrupts disabled)
 * ---------------------------------------------------------------- */
void sched_tick(void)
{
    /* Tick running task's counter. */
    task_t *curr = task_get_current();
    if (curr) curr->ticks_run++;

    /* Wake sleeping tasks. */
    sq_wake_due();

    /* Decrement quantum; switch when exhausted. */
    if (g_quantum_left > 0) g_quantum_left--;
    if (g_quantum_left == 0) {
        do_switch();
    }
}

/* ----------------------------------------------------------------
 * sched_yield
 * ---------------------------------------------------------------- */
void sched_yield(void)
{
    __asm__ volatile ("cli");
    g_quantum_left = 0;
    do_switch();
    __asm__ volatile ("sti");
}

/* ----------------------------------------------------------------
 * sched_sleep
 * ---------------------------------------------------------------- */
void sched_sleep(uint32_t ms)
{
    __asm__ volatile ("cli");

    task_t *curr = task_get_current();
    curr->state     = TASK_SLEEPING;
    curr->wake_tick = g_ticks + (uint64_t)ms;

    /* Remove from ready queue if present (it should be RUNNING). */
    /* (do_switch won't re-enqueue a SLEEPING task.) */

    /* Push onto sleep queue. */
    sq_push(curr);

    /* Force immediate switch. */
    g_quantum_left = 0;

    /* Temporarily set curr state so do_switch won't re-enqueue. */
    /* do_switch checks curr->state == TASK_RUNNING; it's SLEEPING now. */
    do_switch();

    __asm__ volatile ("sti");
}

/* ----------------------------------------------------------------
 * sched_exit
 * ---------------------------------------------------------------- */
void sched_exit(void)
{
    __asm__ volatile ("cli");

    task_t *curr = task_get_current();
    curr->state = TASK_DEAD;
    /* do_switch won't re-enqueue a DEAD task. */
    g_quantum_left = 0;
    do_switch();

    /* Never reached — switch_context won't return to us because
     * this task is DEAD and will be re-used for a new task next time
     * task_create finds a DEAD slot.                               */
    for (;;) __asm__ volatile ("hlt");
}

/* ----------------------------------------------------------------
 * sched_current
 * ---------------------------------------------------------------- */
task_t *sched_current(void) { return task_get_current(); }

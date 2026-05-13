#ifndef TASK_H
#define TASK_H

/* ================================================================
 * AIOS — Task / Process Control Block
 *
 * Each kernel thread has one task_t.  The scheduler owns the
 * ready-queue links; task.c only manages creation/destruction.
 *
 * Stack layout when a task is first switched to:
 *
 *   HIGH  ┌──────────────────────────────────────┐
 *         │  (entry_fn address — popped by ret)  │ ← initial "return address"
 *         │  r15 = 0                             │
 *         │  r14 = 0                             │
 *         │  r13 = 0                             │
 *         │  r12 = 0                             │
 *         │  rbx = 0                             │
 *         │  rbp = 0                             │
 *   LOW   └──────────────────────────────────────┘  ← initial RSP stored in task->rsp
 *
 * switch_context pops these 6 registers then does `ret`, which
 * jumps to entry_fn.
 * ================================================================ */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Maximum simultaneous tasks (static table, no kmalloc for TCB). */
#define TASK_MAX  32

/* Maximum task name length. */
#define TASK_NAME_LEN  32

/* ----------------------------------------------------------------
 * Task states
 * ---------------------------------------------------------------- */
typedef enum {
    TASK_DEAD    = 0,   /* slot free / task exited           */
    TASK_READY   = 1,   /* on the ready queue, runnable      */
    TASK_RUNNING = 2,   /* currently executing on CPU        */
    TASK_BLOCKED = 3,   /* waiting for I/O or mutex          */
    TASK_SLEEPING = 4,  /* sleeping until wake_tick          */
} task_state_t;

/* ----------------------------------------------------------------
 * Task Control Block
 * ---------------------------------------------------------------- */
typedef struct task {
    /* Identity */
    uint32_t      pid;                     /* unique process ID   */
    char          name[TASK_NAME_LEN];     /* human-readable name */
    task_state_t  state;                   /* current state       */

    /* Context */
    uint64_t      rsp;        /* saved stack pointer (used by switch_context) */
    uint64_t      cr3;        /* page table root; 0 = use kernel PML4         */

    /* Stack */
    void         *stack_base; /* kmalloc'd stack bottom                       */
    uint32_t      stack_size; /* bytes                                        */

    /* Scheduler links (intrusive doubly-linked list) */
    struct task  *next;
    struct task  *prev;

    /* Timing */
    uint64_t      wake_tick;  /* for TASK_SLEEPING: wake when g_ticks >= this */
    uint64_t      ticks_run;  /* total ticks this task has been RUNNING       */
} task_t;

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

/*
 * task_init()
 *   Clear the static task table, assign PID 0 to the bootstrap
 *   (currently running) kernel main context.
 *   Must be called before sched_init().
 */
void task_init(void);

/*
 * task_create(entry, stack_size, name)
 *   Allocate a task slot and a stack via kmalloc.
 *   Set up the initial register frame so switch_context() will
 *   jump to `entry` on first dispatch.
 *   Returns pointer to new task_t, or NULL on failure.
 */
task_t *task_create(void (*entry)(void), uint32_t stack_size, const char *name);

/*
 * task_destroy(t)
 *   Free the stack and mark the slot DEAD.
 *   Caller must have already removed t from any scheduler queues.
 */
void task_destroy(task_t *t);

/*
 * task_get_current()
 *   Return pointer to the currently running task_t.
 *   Set by the scheduler before each dispatch.
 */
task_t *task_get_current(void);

/*
 * task_set_current(t)
 *   Used only by the scheduler.
 */
void task_set_current(task_t *t);

#endif /* TASK_H */

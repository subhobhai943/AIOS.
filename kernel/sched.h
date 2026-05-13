#ifndef SCHED_H
#define SCHED_H

/* ================================================================
 * AIOS — Round-Robin Preemptive Scheduler
 *
 * The scheduler maintains two queues:
 *   - ready_queue : circular doubly-linked list of TASK_READY tasks
 *   - sleep_queue : singly-linked list of TASK_SLEEPING tasks
 *
 * sched_tick() is called from the PIT IRQ handler (every 1 ms).
 * It decrements sleep timers and performs a round-robin context
 * switch every SCHED_TICK_QUANTUM ticks.
 * ================================================================ */

#include "task.h"
#include <stdint.h>

/* Time-slice length in PIT ticks (1 tick = 1 ms at 1000 Hz). */
#define SCHED_TICK_QUANTUM  10u

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

/*
 * sched_init()
 *   Initialise scheduler data structures and enqueue the boot task
 *   (PID 0) as the current running task.  Call after task_init().
 */
void sched_init(void);

/*
 * sched_add(t)
 *   Add a TASK_READY task to the back of the ready queue.
 */
void sched_add(task_t *t);

/*
 * sched_tick()
 *   Called from PIT IRQ0 handler (with interrupts disabled by CPU).
 *   Wakes sleeping tasks whose wake_tick <= g_ticks.
 *   Decrements the running task's remaining quantum; on expiry
 *   performs a round-robin switch to the next READY task.
 */
void sched_tick(void);

/*
 * sched_yield()
 *   Voluntarily surrender the remaining time slice.
 *   Can be called from kernel task code with interrupts enabled.
 */
void sched_yield(void);

/*
 * sched_sleep(ms)
 *   Block the current task for at least `ms` milliseconds.
 *   Puts it on the sleep queue; woken by sched_tick().
 */
void sched_sleep(uint32_t ms);

/*
 * sched_exit()
 *   Terminate the current task.  Never returns.
 *   Marks the task DEAD and yields to the next runnable task.
 */
void sched_exit(void) __attribute__((noreturn));

/*
 * sched_current()
 *   Return the currently running task (same as task_get_current()).
 */
task_t *sched_current(void);

#endif /* SCHED_H */

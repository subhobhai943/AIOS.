#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * kthread  —  Kernel Thread API
 *
 * Thin convenience wrapper over task_create() + sched_add().
 * Every long-running kernel service (LLM inference, I/O handler, shell)
 * should be launched as a kthread rather than inlined in kernel_main.
 *
 * Usage:
 *
 *   static void my_worker(void *arg) {
 *       int *val = (int *)arg;
 *       vga_puts("worker running\n");
 *       kthread_exit();
 *   }
 *
 *   // In kernel_main or another thread:
 *   kthread_t t = kthread_create(my_worker, &some_val,
 *                                KTHREAD_STACK_DEFAULT, "my-worker");
 *   kthread_join(t);   // optional: wait for completion
 */

/* Default kernel-thread stack size (16 KB). */
#define KTHREAD_STACK_DEFAULT  (16 * 1024)

/* Opaque handle — currently just a pointer to the underlying task_t.
 * Treat it as an opaque integer-sized value; do not dereference. */
typedef void *kthread_t;

/* Sentinel returned on allocation failure. */
#define KTHREAD_INVALID  ((kthread_t)0)

/*
 * kthread_create  —  spawn a new kernel thread.
 *
 * @fn         : thread entry point; must call kthread_exit() before returning
 * @arg        : opaque argument passed to fn
 * @stack_size : stack bytes; pass KTHREAD_STACK_DEFAULT for 16 KB
 * @name       : debug name (shown in ps output once shell is ready)
 *
 * Returns KTHREAD_INVALID if task allocation fails.
 * The thread is immediately added to the scheduler ready queue.
 */
kthread_t kthread_create(void (*fn)(void *arg), void *arg,
                         size_t stack_size, const char *name);

/*
 * kthread_exit  —  terminate the calling thread.
 *
 * Must be called (directly or via return) from the thread function.
 * Calls sched_exit() which marks the task DEAD and yields the CPU.
 * Does not return.
 */
void kthread_exit(void) __attribute__((noreturn));

/*
 * kthread_join  —  wait for a thread to finish.
 *
 * Busy-yield-waits until the target thread has been marked DEAD by
 * sched_exit().  Suitable for short-lived init threads; not intended
 * for production long-lived waits (use a semaphore from Phase 4.4).
 *
 * It is safe to call kthread_join on KTHREAD_INVALID (no-op).
 */
void kthread_join(kthread_t t);

/*
 * kthread_self  —  return handle for the currently running thread.
 *
 * Returns KTHREAD_INVALID if called before the scheduler is up.
 */
kthread_t kthread_self(void);

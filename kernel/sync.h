#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "task.h"

/* ============================================================
 * AIOS Phase 4.4 — Synchronization Primitives
 * sync.h — spinlock, mutex, semaphore
 *
 * Rules:
 *   - Spinlock: IRQ-safe, xchg-based. Hold for < ~100 ns only.
 *   - Mutex:    Sleep-based. Calls sched_sleep/sched_yield.
 *               NEVER call mutex_lock() with interrupts disabled.
 *   - Semaphore: Counting. sem_wait() blocks if count == 0.
 *                sem_post() wakes one waiter.
 * ============================================================ */

/* ----------------------------------------------------------
 * Spinlock
 * ---------------------------------------------------------- */
typedef struct {
    volatile uint32_t locked;   /* 0 = free, 1 = held */
} spinlock_t;

#define SPINLOCK_INIT  { .locked = 0 }

void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
bool spin_try_lock(spinlock_t *lock);   /* non-blocking; true = acquired */

/* IRQ-safe variants — save/restore RFLAGS around the critical section */
uint64_t spin_lock_irqsave(spinlock_t *lock);
void     spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags);

/* ----------------------------------------------------------
 * Mutex  (sleep-based, NOT IRQ-safe — do not use in ISRs)
 * ---------------------------------------------------------- */
#define MUTEX_WAITER_MAX  16

typedef struct {
    volatile uint32_t locked;               /* 0 = free, 1 = held */
    volatile uint32_t owner_pid;            /* PID of holder, 0 if free */
    volatile uint32_t waiters[MUTEX_WAITER_MAX]; /* PIDs sleeping on this mutex */
    volatile uint32_t nwaiters;
    spinlock_t        guard;                /* protects waiter list */
} mutex_t;

#define MUTEX_INIT  { .locked=0, .owner_pid=0, .nwaiters=0, .guard=SPINLOCK_INIT }

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);    /* blocks (yield-spin) until acquired */
void mutex_unlock(mutex_t *m);
bool mutex_try_lock(mutex_t *m);

/* ----------------------------------------------------------
 * Semaphore  (counting, sleep-based)
 * ---------------------------------------------------------- */
#define SEM_WAITER_MAX  16

typedef struct {
    volatile int32_t  count;
    volatile uint32_t waiters[SEM_WAITER_MAX];
    volatile uint32_t nwaiters;
    spinlock_t        guard;
} sem_t;

void sem_init(sem_t *s, int32_t initial_count);
void sem_wait(sem_t *s);           /* P() — decrement; block if count == 0 */
void sem_post(sem_t *s);           /* V() — increment; wake one waiter */
bool sem_trywait(sem_t *s);        /* non-blocking; true = decremented */
int32_t sem_value(sem_t *s);       /* read current count (snapshot) */

#include "sync.h"
#include "sched.h"
#include "task.h"
#include "serial.h"

/* ============================================================
 * AIOS Phase 4.4 — Synchronization Primitives
 * sync.c
 * ============================================================ */

/* ----------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------- */

/* Read RFLAGS */
static inline uint64_t read_rflags(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; pop %0" : "=r"(f));
    return f;
}

/* Disable interrupts, return old RFLAGS */
static inline uint64_t cli_save(void) {
    uint64_t f = read_rflags();
    __asm__ volatile ("cli" ::: "memory");
    return f;
}

/* Restore RFLAGS (re-enables interrupts if they were on) */
static inline void rflags_restore(uint64_t f) {
    __asm__ volatile ("push %0; popfq" :: "r"(f) : "memory");
}

/* Atomic xchg: returns old value of *ptr, stores new_val */
static inline uint32_t atomic_xchg(volatile uint32_t *ptr, uint32_t new_val) {
    uint32_t old;
    __asm__ volatile (
        "lock xchgl %0, %1"
        : "=r"(old), "+m"(*ptr)
        : "0"(new_val)
        : "memory"
    );
    return old;
}

/* CPU pause hint — reduces power in spin loops */
static inline void cpu_pause(void) {
    __asm__ volatile ("pause" ::: "memory");
}

/* ============================================================
 * Spinlock
 * ============================================================ */

void spin_lock(spinlock_t *lock) {
    while (atomic_xchg(&lock->locked, 1) != 0) {
        /* busy-wait with pause to avoid memory-bus saturation */
        while (lock->locked)
            cpu_pause();
    }
}

void spin_unlock(spinlock_t *lock) {
    __asm__ volatile ("" ::: "memory");  /* compiler barrier */
    lock->locked = 0;
}

bool spin_try_lock(spinlock_t *lock) {
    return atomic_xchg(&lock->locked, 1) == 0;
}

uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t flags = cli_save();
    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spin_unlock(lock);
    rflags_restore(flags);
}

/* ============================================================
 * Mutex
 * ============================================================ */

void mutex_init(mutex_t *m) {
    m->locked     = 0;
    m->owner_pid  = 0;
    m->nwaiters   = 0;
    m->guard.locked = 0;
    for (uint32_t i = 0; i < MUTEX_WAITER_MAX; i++)
        m->waiters[i] = 0;
}

bool mutex_try_lock(mutex_t *m) {
    if (atomic_xchg(&m->locked, 1) == 0) {
        task_t *cur = sched_current();
        m->owner_pid = cur ? cur->pid : 0;
        return true;
    }
    return false;
}

void mutex_lock(mutex_t *m) {
    task_t *cur = sched_current();
    uint32_t my_pid = cur ? cur->pid : 0;

    /* Fast path — try once before registering as a waiter */
    if (atomic_xchg(&m->locked, 1) == 0) {
        m->owner_pid = my_pid;
        return;
    }

    /* Register this task as a waiter so unlock() can wake us */
    uint64_t flags = spin_lock_irqsave(&m->guard);
    if (m->nwaiters < MUTEX_WAITER_MAX) {
        m->waiters[m->nwaiters++] = my_pid;
    }
    spin_unlock_irqrestore(&m->guard, flags);

    /* Yield-spin until we acquire the lock */
    while (atomic_xchg(&m->locked, 1) != 0) {
        sched_yield();
    }
    m->owner_pid = my_pid;

    /* Remove ourselves from the waiter list */
    flags = spin_lock_irqsave(&m->guard);
    for (uint32_t i = 0; i < m->nwaiters; i++) {
        if (m->waiters[i] == my_pid) {
            /* compact: move last entry here */
            m->waiters[i] = m->waiters[--m->nwaiters];
            m->waiters[m->nwaiters] = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&m->guard, flags);
}

void mutex_unlock(mutex_t *m) {
    m->owner_pid = 0;
    __asm__ volatile ("" ::: "memory");
    m->locked = 0;
    /* No explicit wake needed — waiting tasks are yield-spinning and
     * will see locked==0 on their next round. The scheduler's round-robin
     * tick will schedule them naturally. */
}

/* ============================================================
 * Semaphore
 * ============================================================ */

void sem_init(sem_t *s, int32_t initial_count) {
    s->count    = initial_count;
    s->nwaiters = 0;
    s->guard.locked = 0;
    for (uint32_t i = 0; i < SEM_WAITER_MAX; i++)
        s->waiters[i] = 0;
}

bool sem_trywait(sem_t *s) {
    uint64_t flags = spin_lock_irqsave(&s->guard);
    bool ok = false;
    if (s->count > 0) {
        s->count--;
        ok = true;
    }
    spin_unlock_irqrestore(&s->guard, flags);
    return ok;
}

void sem_wait(sem_t *s) {
    task_t *cur = sched_current();
    uint32_t my_pid = cur ? cur->pid : 0;

    /* Fast path */
    {
        uint64_t f = spin_lock_irqsave(&s->guard);
        if (s->count > 0) {
            s->count--;
            spin_unlock_irqrestore(&s->guard, f);
            return;
        }
        /* Register as waiter */
        if (s->nwaiters < SEM_WAITER_MAX)
            s->waiters[s->nwaiters++] = my_pid;
        spin_unlock_irqrestore(&s->guard, f);
    }

    /* Yield-spin until count > 0 */
    for (;;) {
        uint64_t f = spin_lock_irqsave(&s->guard);
        if (s->count > 0) {
            s->count--;
            /* remove from waiter list */
            for (uint32_t i = 0; i < s->nwaiters; i++) {
                if (s->waiters[i] == my_pid) {
                    s->waiters[i] = s->waiters[--s->nwaiters];
                    s->waiters[s->nwaiters] = 0;
                    break;
                }
            }
            spin_unlock_irqrestore(&s->guard, f);
            return;
        }
        spin_unlock_irqrestore(&s->guard, f);
        sched_yield();
    }
}

void sem_post(sem_t *s) {
    uint64_t flags = spin_lock_irqsave(&s->guard);
    s->count++;
    spin_unlock_irqrestore(&s->guard, flags);
    /* No explicit unblock needed — yield-spinners will see count > 0 */
}

int32_t sem_value(sem_t *s) {
    uint64_t flags = spin_lock_irqsave(&s->guard);
    int32_t v = s->count;
    spin_unlock_irqrestore(&s->guard, flags);
    return v;
}

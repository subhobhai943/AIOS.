#include "kthread.h"
#include "task.h"
#include "sched.h"
#include "serial.h"

/*
 * Internal trampoline
 * -------------------
 * task_create() sets up the initial stack so that RIP points to this
 * function on first dispatch.  We need to route the real (fn, arg) pair
 * through the task struct somehow.
 *
 * We store fn+arg in two extra fields that we piggy-back onto task_t
 * via a kthread_frame_t placed at the top of the stack before handing
 * control to the entry stub in switch_context.asm.
 *
 * Simpler approach used here: a tiny wrapper struct stored in the name
 * field is too small; instead we store fn+arg in a static per-task slot
 * keyed by pid.  Maximum concurrent kthreads = KTHREAD_MAX (128).
 *
 * This avoids any dynamic allocation for the metadata itself.
 */

#define KTHREAD_MAX 128

typedef struct {
    void (*fn)(void *arg);
    void  *arg;
    int    pid;       /* -1 = slot free */
} kthread_slot_t;

static kthread_slot_t g_slots[KTHREAD_MAX];
static bool           g_slots_init = false;

static void slots_init(void) {
    if (g_slots_init) return;
    for (int i = 0; i < KTHREAD_MAX; i++)
        g_slots[i].pid = -1;
    g_slots_init = true;
}

static kthread_slot_t *slot_alloc(void) {
    for (int i = 0; i < KTHREAD_MAX; i++)
        if (g_slots[i].pid == -1)
            return &g_slots[i];
    return (void*)0;
}

/* The actual task entry point.  The scheduler dispatches here.
 * We find our slot by scanning for our own PID (set just after
 * task_create returns). */
static void kthread_trampoline(void) {
    /* Identify ourselves */
    task_t *self = task_current();
    int my_pid   = self ? self->pid : -1;

    kthread_slot_t *slot = (void*)0;
    for (int i = 0; i < KTHREAD_MAX; i++) {
        if (g_slots[i].pid == my_pid) {
            slot = &g_slots[i];
            break;
        }
    }

    if (!slot) {
        serial_puts(SERIAL_COM1, "[kthread] trampoline: no slot found!\n");
        kthread_exit();
    }

    void (*fn)(void *) = slot->fn;
    void  *arg         = slot->arg;

    /* Release slot early so it can be reused */
    slot->pid = -1;

    fn(arg);

    /* If fn returned without calling kthread_exit(), do it now. */
    kthread_exit();
}

/* ------------------------------------------------------------------ */

kthread_t kthread_create(void (*fn)(void *arg), void *arg,
                         size_t stack_size, const char *name)
{
    slots_init();

    kthread_slot_t *slot = slot_alloc();
    if (!slot) {
        serial_puts(SERIAL_COM1, "[kthread] no free slots\n");
        return KTHREAD_INVALID;
    }

    /* task_create with our trampoline as the entry point */
    task_t *t = task_create((task_entry_fn)kthread_trampoline,
                            stack_size, name);
    if (!t) {
        serial_puts(SERIAL_COM1, "[kthread] task_create failed\n");
        return KTHREAD_INVALID;
    }

    /* Fill the slot now that we have the pid */
    slot->fn  = fn;
    slot->arg = arg;
    slot->pid = t->pid;

    sched_add(t);
    return (kthread_t)t;
}

void kthread_exit(void) {
    sched_exit();   /* marks DEAD + yields; does not return */
    /* unreachable */
    while (1) { __asm__ volatile("hlt"); }
}

void kthread_join(kthread_t t) {
    if (t == KTHREAD_INVALID) return;
    task_t *task = (task_t *)t;
    while (task->state != TASK_DEAD)
        sched_yield();
}

kthread_t kthread_self(void) {
    task_t *t = task_current();
    return t ? (kthread_t)t : KTHREAD_INVALID;
}

#ifndef TASK_H
#define TASK_H

/* ================================================================
 * AIOS — Task / Process Control Block
 * ================================================================ */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TASK_MAX      32
#define TASK_NAME_LEN 32

typedef enum {
    TASK_DEAD    = 0,
    TASK_READY   = 1,
    TASK_RUNNING = 2,
    TASK_BLOCKED = 3,
    TASK_SLEEPING = 4,
} task_state_t;

typedef struct task {
    uint32_t      pid;
    char          name[TASK_NAME_LEN];
    task_state_t  state;
    uint64_t      rsp;
    uint64_t      cr3;
    void         *stack_base;
    uint32_t      stack_size;
    struct task  *next;
    struct task  *prev;
    uint64_t      wake_tick;
    uint64_t      ticks_run;
} task_t;

void    task_init(void);
task_t *task_create(void (*entry)(void), uint32_t stack_size, const char *name);
void    task_destroy(task_t *t);
task_t *task_get_current(void);
void    task_set_current(task_t *t);

/*
 * task_foreach(cb, ctx)
 *   Iterate over all non-DEAD task slots and call cb(task, ctx)
 *   for each one.  Safe to call from the shell thread.
 */
typedef void (*task_iter_cb_t)(task_t *t, void *ctx);
void task_foreach(task_iter_cb_t cb, void *ctx);

#endif /* TASK_H */

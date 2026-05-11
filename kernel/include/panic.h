#ifndef PANIC_H
#define PANIC_H

/* ============================================================
 * AIOS — Kernel Panic & Assert
 *
 * kernel_panic(msg)  — print msg to VGA (red) + COM1 serial,
 *                       then halt forever.
 * KERNEL_ASSERT(c,m) — panic if condition c is false.
 * ============================================================ */

/* kernel_panic never returns — tell the compiler so it can
 * suppress "control reaches end of non-void function" etc. */
__attribute__((noreturn))
void kernel_panic(const char *msg);

#define KERNEL_ASSERT(cond, msg) \
    do { if (!(cond)) kernel_panic(msg); } while (0)

#endif /* PANIC_H */

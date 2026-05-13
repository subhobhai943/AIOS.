#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ─────────────────────────────────────────────────────────────
   Phase 5.2 — Shell
   Public API for the AIOS interactive shell.
   shell_run() is launched as a kthread from kernel_main.
   ───────────────────────────────────────────────────────────── */

#define SHELL_PROMPT        "AIOS> "
#define SHELL_LINE_MAX      256
#define SHELL_ARGC_MAX      16
#define SHELL_TOKEN_MAX     64   /* max chars per token */

/* Opaque command handler type */
typedef void (*shell_cmd_fn_t)(int argc, char **argv);

typedef struct {
    const char     *name;       /* command name, e.g. "ls"    */
    const char     *usage;      /* one-line usage string       */
    const char     *help;       /* longer description          */
    shell_cmd_fn_t  fn;         /* handler function            */
} shell_cmd_t;

/* ── Public API ─────────────────────────────────────────────── */

/* Entry point — call as a kthread:
 *   kthread_create(shell_run, NULL, 32768, "shell");
 */
void shell_run(void *arg);

/* Internal: tokenise one line in-place.
 * Returns argc; argv[] point into buf.
 * Handles single-quoted strings: 'hello world' → one token.
 */
int  shell_tokenize(char *buf, char **argv, int argv_max);

/* Print all registered commands */
void shell_print_help(void);

#endif /* SHELL_H */

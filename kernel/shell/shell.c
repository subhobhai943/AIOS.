/* shell.c — AIOS shell core */

#include "shell.h"
#include "terminal.h"

#include "../vga.h"
#include "../serial.h"
#include "../heap.h"
#include "../pmm.h"
#include "../sched.h"
#include "../task.h"
#include "../acpi.h"
#include "../fs/vfs.h"

#include "../gfx/framebuffer.h"
#include "../gui/wm.h"          /* Phase 10.6: start GUI WM thread */
#include "../gui/input_mode.h"   /* Phase 10.3: GUI input enable/disable */

#include <stdint.h>

#define SHELL_LINE_MAX  256

static void sh_puts(const char *s) {
    while (*s) {
        vga_putchar(*s++);
    }
}

static void sh_puthex(uint64_t v) {
    static const char *hex = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4) {
        vga_putchar(hex[(v >> i) & 0xF]);
    }
}

/* ------------------------------------------------------------------ */
/* Built-in command handlers                                           */
/* ------------------------------------------------------------------ */

static int g_gui_started = 0;

static void cmd_help(int argc, char **argv);
static void cmd_clear(int argc, char **argv);
static void cmd_mem(int argc, char **argv);
static void cmd_ps(int argc, char **argv);
static void cmd_ls(int argc, char **argv);
static void cmd_cat(int argc, char **argv);
static void cmd_hexdump(int argc, char **argv);
static void cmd_reboot(int argc, char **argv);
static void cmd_shutdown(int argc, char **argv);
static void cmd_startx(int argc, char **argv);

struct builtin {
    const char *name;
    void (*fn)(int argc, char **argv);
};

static const struct builtin g_builtins[] = {
    { "help",    cmd_help    },
    { "clear",   cmd_clear   },
    { "mem",     cmd_mem     },
    { "ps",      cmd_ps      },
    { "ls",      cmd_ls      },
    { "cat",     cmd_cat     },
    { "hexdump", cmd_hexdump },
    { "reboot",  cmd_reboot  },
    { "shutdown",cmd_shutdown },
    { "startx",  cmd_startx  },
    { "gui",     cmd_startx  },
};

#define NUM_BUILTINS  (int)(sizeof(g_builtins)/sizeof(g_builtins[0]))

/* ------------------------------------------------------------------ */
/* Command implementations                                             */
/* ------------------------------------------------------------------ */

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    sh_puts("Built-in commands:\n");
    sh_puts("  help      - show this help\n");
    sh_puts("  clear     - clear the screen\n");
    sh_puts("  mem       - show memory usage\n");
    sh_puts("  ps        - list tasks\n");
    sh_puts("  ls [path] - list directory\n");
    sh_puts("  cat FILE  - show file contents\n");
    sh_puts("  hexdump FILE - hex dump first 256 bytes\n");
    sh_puts("  reboot    - reboot the machine\n");
    sh_puts("  shutdown  - power off via ACPI (if available)\n");
    sh_puts("  startx    - start the graphical desktop (GUI)\n");
}

static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    vga_clear();
}

/* ... existing cmd_mem/cmd_ps/cmd_ls/cmd_cat/cmd_hexdump/cmd_reboot/cmd_shutdown ... */

static void cmd_startx(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (g_gui_started) {
        sh_puts("GUI already running.\n");
        return;
    }

    /* Basic sanity: require a framebuffer or we just log a warning. */
    framebuffer_t *fb = fb_get();
    if (!fb) {
        sh_puts("No framebuffer available; cannot start GUI.\n");
        return;
    }

    g_gui_started = 1;

    sh_puts("Starting AIOS GUI...\n");

    /* Route input into GUI event queue and spawn WM thread. */
    gui_input_enable();
    gui_wm_start();
}

/* ------------------------------------------------------------------ */
/* Shell main loop                                                     */
/* ------------------------------------------------------------------ */

static void dispatch(char *line)
{
    /* Simple space-separated tokenization. */
    char *argv[16];
    int argc = 0;

    char *p = line;
    while (*p && argc < 16) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p = '\0'; p++; }
    }

    if (argc == 0) return;

    for (int i = 0; i < NUM_BUILTINS; i++) {
        const struct builtin *b = &g_builtins[i];
        const char *s = argv[0];
        const char *t = b->name;
        while (*s && *t && *s == *t) { s++; t++; }
        if (*s == '\0' && *t == '\0') {
            b->fn(argc, argv);
            return;
        }
    }

    sh_puts("Unknown command. Type 'help'.\n");
}

void shell_run(void *arg)
{
    (void)arg;

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("AIOS Shell — Phase 5.2  |  type 'help' for commands\n");
    vga_puts("type 'startx' to launch the graphical desktop\n\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    char line[SHELL_LINE_MAX];

    for (;;) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_puts("AIOS> ");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

        terminal_readline(line, SHELL_LINE_MAX);
        vga_putchar('\n');

        dispatch(line);
    }
}

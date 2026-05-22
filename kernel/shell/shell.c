/* kernel/shell/shell.c — AIOS Shell (Phase 5.2)
 *
 * Built-in commands:
 *   help, clear, echo, mem, ps, ls, cat, hexdump,
 *   reboot, shutdown, startx / gui
 *
 * Launched as a kthread: task_create(shell_run, 65536, "shell")
 * No libc. No standard I/O.
 */

#include "shell.h"
#include "terminal.h"

#include "../include/vga.h"
#include "../serial.h"
#include "../include/pmm.h"
#include "../heap.h"
#include "../sched.h"
#include "../task.h"
#include "../acpi.h"
#include "../fs/vfs.h"
#include "../gfx/framebuffer.h"
#include "../gui/wm.h"
#include "../gui/input_mode.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Tiny helpers (no libc)                                             */
/* ------------------------------------------------------------------ */

static void sh_puts(const char *s)
{
    while (s && *s) vga_putchar(*s++);
}

static void sh_puthex64(uint64_t v)
{
    static const char *hex = "0123456789ABCDEF";
    sh_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        vga_putchar(hex[(v >> i) & 0xF]);
}

static void sh_putdec(uint64_t v)
{
    char tmp[24];
    int  i = 0;
    if (v == 0) { vga_putchar('0'); return; }
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i-- > 0) vga_putchar(tmp[i]);
}

static int sh_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ------------------------------------------------------------------ */
/* Built-in command handlers                                           */
/* ------------------------------------------------------------------ */

static int g_gui_started = 0;

static void cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    vga_puts_color("AIOS Shell — Built-in Commands\n", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    sh_puts("  help          show this help\n");
    sh_puts("  clear         clear the screen\n");
    sh_puts("  echo [args]   print arguments\n");
    sh_puts("  mem           show memory statistics\n");
    sh_puts("  ps            list kernel tasks\n");
    sh_puts("  ls [path]     list known VFS paths\n");
    sh_puts("  cat FILE      print file contents\n");
    sh_puts("  hexdump FILE  hex dump first 256 bytes of file\n");
    sh_puts("  reboot        reboot the machine\n");
    sh_puts("  shutdown      power off (ACPI)\n");
    sh_puts("  startx        launch graphical desktop\n");
}

static void cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    vga_clear();
}

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        sh_puts(argv[i]);
        if (i + 1 < argc) vga_putchar(' ');
    }
    vga_putchar('\n');
}

static void cmd_mem(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint64_t total = pmm_get_total_pages();
    uint64_t free  = pmm_get_free_pages();
    uint64_t used  = total - free;
    vga_puts_color("Physical Memory:\n", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    sh_puts("  Total : "); sh_putdec(total); sh_puts(" pages ("); sh_putdec(total * 4); sh_puts(" KiB)\n");
    sh_puts("  Used  : "); sh_putdec(used);  sh_puts(" pages ("); sh_putdec(used  * 4); sh_puts(" KiB)\n");
    sh_puts("  Free  : "); sh_putdec(free);  sh_puts(" pages ("); sh_putdec(free  * 4); sh_puts(" KiB)\n");
}

/* callback for task_foreach */
static void ps_print_one(task_t *t, void *ctx)
{
    (void)ctx;
    sh_puts("  PID ");
    sh_putdec((uint64_t)t->pid);
    sh_puts("  ");
    sh_puts(t->name);
    sh_puts("  state=");
    switch (t->state) {
        case TASK_READY:    sh_puts("READY");    break;
        case TASK_RUNNING:  sh_puts("RUNNING");  break;
        case TASK_SLEEPING: sh_puts("SLEEPING"); break;
        case TASK_BLOCKED:  sh_puts("BLOCKED");  break;
        default:            sh_puts("?");         break;
    }
    vga_putchar('\n');
}

static void cmd_ps(int argc, char **argv)
{
    (void)argc; (void)argv;
    vga_puts_color("Kernel Tasks:\n", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    task_foreach(ps_print_one, 0);
}

/*
 * cmd_ls — VFS has no directory-listing API (read-only FAT32, no readdir).
 * Instead, probe a set of well-known paths and report which exist.
 */
static const char *g_known_paths[] = {
    "/TEST.TXT",
    "/tokenizer/vocab.bin",
    "/tokenizer/config.bin",
    "/weights.bin",
    "/kernel.bin",
    "/initrd.img",
    NULL
};

static void cmd_ls(int argc, char **argv)
{
    (void)argc; (void)argv;
    vga_puts_color("VFS known files:\n", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    for (int i = 0; g_known_paths[i]; i++) {
        vfs_stat_t st;
        int r = vfs_stat(g_known_paths[i], &st);
        if (r == 0) {
            sh_puts("  [FILE] ");
            sh_puts(g_known_paths[i]);
            sh_puts("  (");
            sh_putdec((uint64_t)st.size);
            sh_puts(" B)\n");
        }
    }
    sh_puts("  (VFS has no readdir; use 'cat' to read a file)\n");
}

static void cmd_cat(int argc, char **argv)
{
    if (argc < 2) { sh_puts("Usage: cat FILE\n"); return; }
    int fd = vfs_open(argv[1]);
    if (fd < 0) { sh_puts("cat: cannot open '"); sh_puts(argv[1]); sh_puts("'\n"); return; }
    static uint8_t buf[256];
    int got;
    while ((got = vfs_read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < got; i++) vga_putchar((char)buf[i]);
    }
    vga_putchar('\n');
    vfs_close(fd);
}

static void cmd_hexdump(int argc, char **argv)
{
    if (argc < 2) { sh_puts("Usage: hexdump FILE\n"); return; }
    int fd = vfs_open(argv[1]);
    if (fd < 0) { sh_puts("hexdump: cannot open '"); sh_puts(argv[1]); sh_puts("'\n"); return; }
    static uint8_t buf[256];
    int got = vfs_read(fd, buf, 256);
    vfs_close(fd);
    if (got <= 0) { sh_puts("(empty)\n"); return; }
    static const char h[] = "0123456789ABCDEF";
    for (int i = 0; i < got; i++) {
        if ((i & 0xF) == 0) { sh_puthex64((uint64_t)i); sh_puts(": "); }
        vga_putchar(h[(buf[i] >> 4) & 0xF]);
        vga_putchar(h[buf[i] & 0xF]);
        vga_putchar(' ');
        if ((i & 0xF) == 0xF) vga_putchar('\n');
    }
    vga_putchar('\n');
}

static void cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    sh_puts("Rebooting...\n");
    __asm__ volatile (
        "1: inb $0x64, %%al; testb $2, %%al; jnz 1b;\n"
        "movb $0xFE, %%al; outb %%al, $0x64\n" ::: "al");
    __asm__ volatile ("lidt 0; int3" :::);
    for (;;) __asm__ volatile ("hlt");
}

static void cmd_shutdown(int argc, char **argv)
{
    (void)argc; (void)argv;
    sh_puts("Shutting down...\n");
    acpi_shutdown();
    __asm__ volatile ("outw %%ax, %%dx" :: "a"(0x2000), "d"(0x604));
    for (;;) __asm__ volatile ("hlt");
}

static void cmd_startx(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (g_gui_started) { sh_puts("GUI is already running.\n"); return; }
    framebuffer_t *fb = fb_get();
    if (!fb || fb->width == 0) {
        sh_puts("No framebuffer available. Boot with -vga std.\n");
        return;
    }
    g_gui_started = 1;
    sh_puts("Starting AIOS GUI desktop...\n");
    gui_input_enable();
    gui_wm_start();
    sched_sleep(0x7FFFFFFFu);
}

/* ------------------------------------------------------------------ */
/* Command table                                                       */
/* ------------------------------------------------------------------ */

typedef struct { const char *name; void (*fn)(int, char **); } builtin_t;

static const builtin_t g_builtins[] = {
    { "help",     cmd_help     },
    { "clear",    cmd_clear    },
    { "echo",     cmd_echo     },
    { "mem",      cmd_mem      },
    { "ps",       cmd_ps       },
    { "ls",       cmd_ls       },
    { "cat",      cmd_cat      },
    { "hexdump",  cmd_hexdump  },
    { "reboot",   cmd_reboot   },
    { "shutdown", cmd_shutdown },
    { "startx",   cmd_startx   },
    { "gui",      cmd_startx   },
};

#define NUM_BUILTINS ((int)(sizeof(g_builtins)/sizeof(g_builtins[0])))

/* ------------------------------------------------------------------ */
/* Tokenizer                                                           */
/* ------------------------------------------------------------------ */

int shell_tokenize(char *buf, char **argv, int argv_max)
{
    int argc = 0;
    char *p = buf;
    while (*p && argc < argv_max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '\'') {
            p++; argv[argc++] = p;
            while (*p && *p != '\'') p++;
            if (*p) *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
    }
    return argc;
}

static void dispatch(char *line)
{
    char *argv[SHELL_ARGC_MAX];
    int   argc = shell_tokenize(line, argv, SHELL_ARGC_MAX);
    if (argc == 0) return;
    for (int i = 0; i < NUM_BUILTINS; i++) {
        if (sh_strcmp(argv[0], g_builtins[i].name) == 0) {
            g_builtins[i].fn(argc, argv);
            return;
        }
    }
    sh_puts("Unknown command: '"); sh_puts(argv[0]); sh_puts("'. Type 'help'.\n");
}

/* ------------------------------------------------------------------ */
/* Shell main loop                                                     */
/* ------------------------------------------------------------------ */

void shell_run(void)
{
    terminal_init();

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts(
        "\n====================================================\n"
        "  AIOS Shell — Phase 5.2\n"
        "  Type 'help' for commands, 'startx' for GUI.\n"
        "====================================================\n\n");
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

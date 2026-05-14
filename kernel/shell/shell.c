/* ─────────────────────────────────────────────────────────────
   Phase 5.2 — Shell
   Interactive AIOS shell: prompt, tokenizer, built-in commands.

   Constraints:
     - No libc.  Only <stdint.h>, <stddef.h>, <stdbool.h>.
     - kmalloc/kfree for all heap use.
     - Compiled with -ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel
   ───────────────────────────────────────────────────────────── */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "shell.h"
#include "terminal.h"

#include "../vga.h"
#include "../serial.h"
#include "../heap.h"
#include "../pmm.h"
#include "../vmm.h"
#include "../task.h"
#include "../sched.h"
#include "../kthread.h"
#include "../fat32.h"
#include "../fs/vfs.h"
#include "../fs/vfs_initrd.h"
#include "panic.h"

/* ── forward declarations for built-in handlers ────────────── */
static void cmd_help    (int argc, char **argv);
static void cmd_clear   (int argc, char **argv);
static void cmd_mem     (int argc, char **argv);
static void cmd_ps      (int argc, char **argv);
static void cmd_ls      (int argc, char **argv);
static void cmd_cat     (int argc, char **argv);
static void cmd_load    (int argc, char **argv);
static void cmd_ai      (int argc, char **argv);
static void cmd_chat    (int argc, char **argv);
static void cmd_reboot  (int argc, char **argv);
static void cmd_shutdown(int argc, char **argv);
static void cmd_echo    (int argc, char **argv);
static void cmd_hexdump (int argc, char **argv);

/* ── command table ──────────────────────────────────────────── */
static const shell_cmd_t g_cmds[] = {
    { "help",     "help",              "List all available commands",                          cmd_help     },
    { "clear",    "clear",             "Clear the screen",                                     cmd_clear    },
    { "mem",      "mem",               "Show physical / virtual memory usage",                 cmd_mem      },
    { "ps",       "ps",                "List running tasks with PID and state",                cmd_ps       },
    { "ls",       "ls [path]",         "List files in directory (default: VFS root)",          cmd_ls       },
    { "cat",      "cat <file>",        "Print file contents to screen",                        cmd_cat      },
    { "load",     "load <model>",      "Load an LLM model from disk into memory",              cmd_load     },
    { "ai",       "ai <prompt...>",    "Send a one-shot prompt to the loaded LLM",             cmd_ai       },
    { "chat",     "chat",              "Enter interactive multi-turn chat mode",               cmd_chat     },
    { "reboot",   "reboot",            "Reboot the system",                                    cmd_reboot   },
    { "shutdown", "shutdown",          "Power off the system (ACPI)",                          cmd_shutdown },
    { "echo",     "echo <args...>",    "Print arguments to screen",                            cmd_echo     },
    { "hexdump",  "hexdump <file>",    "Hex + ASCII dump of a file (first 256 bytes)",        cmd_hexdump  },
};

#define N_CMDS  (sizeof(g_cmds) / sizeof(g_cmds[0]))

/* ── tiny no-libc string helpers used only in this file ─────── */
static size_t sh_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
static int sh_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static void sh_strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
/* print unsigned decimal to VGA */
static void sh_print_u64(uint64_t v) {
    if (v == 0) { vga_putchar('0'); return; }
    char buf[24]; int i = 0;
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) vga_putchar(buf[i+1-1+1-1]); /* reverse */
    /* cleaner reverse: */
}
static void sh_putu64(uint64_t v) {
    if (v == 0) { vga_putchar('0'); return; }
    char tmp[24]; int len = 0;
    while (v) { tmp[len++] = '0' + (int)(v % 10); v /= 10; }
    for (int i = len - 1; i >= 0; i--) vga_putchar(tmp[i]);
}
static void sh_putx8(uint8_t v) {
    const char hex[] = "0123456789ABCDEF";
    vga_putchar(hex[(v >> 4) & 0xF]);
    vga_putchar(hex[v & 0xF]);
}

/* ── tokenizer ──────────────────────────────────────────────── */
/*
 * Tokenize `buf` in-place (NUL-terminates each token).
 * Rules:
 *   - Splits on space / tab.
 *   - Single-quoted strings: 'hello world' → one token (quotes stripped).
 *   - Max `argv_max` tokens; excess input silently ignored.
 * Returns argc.
 */
int shell_tokenize(char *buf, char **argv, int argv_max) {
    int argc = 0;
    char *p = buf;

    while (*p && argc < argv_max) {
        /* skip leading whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (*p == '\'') {
            /* single-quoted string */
            p++;  /* skip opening quote */
            argv[argc++] = p;
            while (*p && *p != '\'') p++;
            if (*p == '\'') { *p = '\0'; p++; }
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) { *p = '\0'; p++; }
        }
    }
    return argc;
}

/* ── dispatch ───────────────────────────────────────────────── */
static void dispatch(char *line) {
    char *argv[SHELL_ARGC_MAX];
    int argc = shell_tokenize(line, argv, SHELL_ARGC_MAX);
    if (argc == 0) return;

    for (size_t i = 0; i < N_CMDS; i++) {
        if (sh_strcmp(argv[0], g_cmds[i].name) == 0) {
            g_cmds[i].fn(argc, argv);
            return;
        }
    }

    /* unknown command */
    vga_puts_color("Unknown command: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_puts(argv[0]);
    vga_puts("  (type 'help' for a list)\n");
}

/* ── shell_run (kthread entry) ──────────────────────────────── */
void shell_run(void *arg) {
    (void)arg;

    /* banner */
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("\n");
    vga_puts("  ___  ___ ___  ____  \n");
    vga_puts(" / _ \\|_ _/ _ \\/ ___| \n");
    vga_puts("| |_| || | | | \\___ \\ \n");
    vga_puts(" \\__,_|___\\___/|____/ \n");
    vga_puts("\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("AIOS Shell — Phase 5.2  |  type 'help' for commands\n");
    vga_puts("\n");
    klog("[shell] shell_run started\n");

    static char line[SHELL_LINE_MAX];

    for (;;) {
        /* print prompt */
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_puts(SHELL_PROMPT);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

        /* blocking readline — yields CPU via sched_yield() while empty */
        terminal_readline(line, SHELL_LINE_MAX);
        vga_putchar('\n');

        /* dispatch */
        dispatch(line);
    }
}

/* ═══════════════════════════════════════════════════════════════
   Built-in command implementations
   ═══════════════════════════════════════════════════════════════ */

/* ── help ───────────────────────────────────────────────────── */
void shell_print_help(void) {
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("Available commands:\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    for (size_t i = 0; i < N_CMDS; i++) {
        vga_puts("  ");
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        /* pad command name to 10 chars */
        size_t nlen = sh_strlen(g_cmds[i].usage);
        vga_puts(g_cmds[i].usage);
        /* padding */
        for (size_t p = nlen; p < 24; p++) vga_putchar(' ');
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_puts(g_cmds[i].help);
        vga_putchar('\n');
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_print_help();
}

/* ── clear ──────────────────────────────────────────────────── */
static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    vga_clear();
}

/* ── mem ────────────────────────────────────────────────────── */
static void cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;

    uint64_t total   = pmm_total_pages()   * 4096ULL;
    uint64_t used    = pmm_used_pages()    * 4096ULL;
    uint64_t free_b  = pmm_free_pages()    * 4096ULL;

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("Physical Memory:\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    vga_puts("  Total : "); sh_putu64(total  >> 20); vga_puts(" MB\n");
    vga_puts("  Used  : "); sh_putu64(used   >> 20); vga_puts(" MB\n");
    vga_puts("  Free  : "); sh_putu64(free_b >> 20); vga_puts(" MB\n");

    /* heap stats from heap.c */
    size_t heap_used = heap_used_bytes();
    size_t heap_free = heap_free_bytes();
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("Kernel Heap:\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("  Used  : "); sh_putu64(heap_used >> 10); vga_puts(" KB\n");
    vga_puts("  Free  : "); sh_putu64(heap_free >> 10); vga_puts(" KB\n");
}

/* ── ps ─────────────────────────────────────────────────────── */
static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("  PID  STATE    NAME\n");
    vga_puts("  ───  ───────  ──────────────────\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* task_foreach calls our callback for every task in the table */
    task_foreach(^(task_t *t) {
        vga_puts("  ");
        /* PID (3 digits) */
        char pid_buf[8];
        int pi = 0;
        uint32_t pv = t->pid;
        if (pv == 0) { pid_buf[pi++] = '0'; }
        else { while (pv) { pid_buf[pi++] = '0' + (pv % 10); pv /= 10; } }
        /* reverse */
        for (int a = 0, b = pi-1; a < b; a++, b--) {
            char tmp = pid_buf[a]; pid_buf[a] = pid_buf[b]; pid_buf[b] = tmp;
        }
        pid_buf[pi] = '\0';
        /* pad to 4 */
        for (int pad = pi; pad < 4; pad++) vga_putchar(' ');
        vga_puts(pid_buf);

        /* state */
        const char *state_str;
        switch (t->state) {
            case TASK_RUNNING:  state_str = "RUNNING  "; break;
            case TASK_READY:    state_str = "READY    "; break;
            case TASK_BLOCKED:  state_str = "BLOCKED  "; break;
            case TASK_SLEEPING: state_str = "SLEEPING "; break;
            case TASK_DEAD:     state_str = "DEAD     "; break;
            default:            state_str = "UNKNOWN  "; break;
        }
        vga_puts("  "); vga_puts(state_str);
        vga_puts("  "); vga_puts(t->name ? t->name : "(unnamed)");
        vga_putchar('\n');
    });
}

/* ── ls ─────────────────────────────────────────────────────── */
static void cmd_ls(int argc, char **argv) {
    /* default: list initrd files */
    if (argc < 2) {
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        vga_puts("Initrd files (/initrd/):\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        int count = initrd_file_count();
        for (int i = 0; i < count; i++) {
            const initrd_file_t *f = initrd_get_file(i);
            if (!f) continue;
            vga_puts("  ");
            vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
            vga_puts(f->name);
            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            vga_puts("  (");
            sh_putu64(f->size);
            vga_puts(" bytes)\n");
        }
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts("  ");
        sh_putu64((uint64_t)count);
        vga_puts(" file(s)\n");
        return;
    }

    /* FAT32 directory listing */
    const char *path = argv[1];
    uint32_t dir_cluster;
    uint32_t dummy_size;
    if (!fat32_find_file_lfn(fat32_root_cluster(), path + (path[0]=='/'?1:0),
                              &dir_cluster, &dummy_size)) {
        vga_puts_color("ls: path not found: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_puts(path); vga_putchar('\n');
        return;
    }
    fat32_list_dir(dir_cluster, ^(const char *name, uint32_t size, bool is_dir) {
        vga_puts("  ");
        if (is_dir) {
            vga_set_color(VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
            vga_puts("[DIR]  ");
        } else {
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            vga_puts("       ");
        }
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_puts(name);
        if (!is_dir) {
            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            /* pad to 20 */
            size_t nlen = sh_strlen(name);
            for (size_t p = nlen; p < 20; p++) vga_putchar(' ');
            sh_putu64(size);
            vga_puts(" B");
        }
        vga_putchar('\n');
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    });
}

/* ── cat ────────────────────────────────────────────────────── */
static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        vga_puts("Usage: cat <file>\n"); return;
    }
    const char *path = argv[1];

    /* try initrd first (/vocab.bin etc.) */
    const uint8_t *data = NULL;
    size_t size = 0;
    if (vfs_initrd_open(path, &data, &size) == 0) {
        /* print up to 4096 bytes as text */
        size_t lim = size < 4096 ? size : 4096;
        for (size_t i = 0; i < lim; i++) {
            char c = (char)data[i];
            if (c == '\n' || (c >= 0x20 && c < 0x7F)) vga_putchar(c);
            else vga_putchar('.');
        }
        if (size > 4096) { vga_puts("\n... (truncated)\n"); }
        vga_putchar('\n');
        return;
    }

    /* try VFS / FAT32 */
    int fd = vfs_open(path);
    if (fd < 0) {
        vga_puts_color("cat: file not found: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_puts(path); vga_putchar('\n');
        return;
    }
    static uint8_t cat_buf[4096];
    int32_t n = vfs_read(fd, cat_buf, sizeof(cat_buf));
    vfs_close(fd);
    if (n < 0) { vga_puts("cat: read error\n"); return; }
    for (int32_t i = 0; i < n; i++) {
        char c = (char)cat_buf[i];
        if (c == '\n' || (c >= 0x20 && c < 0x7F)) vga_putchar(c);
        else vga_putchar('.');
    }
    vga_putchar('\n');
}

/* ── load ───────────────────────────────────────────────────── */
static void cmd_load(int argc, char **argv) {
    if (argc < 2) {
        vga_puts("Usage: load <model_path>\n"); return;
    }
    /* Phase 7.6 will implement the real loader.
     * For now: open the file, check it exists, print its size. */
    const char *path = argv[1];
    int fd = vfs_open(path);
    if (fd < 0) {
        vga_puts_color("load: model file not found: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_puts(path); vga_putchar('\n');
        return;
    }
    vfs_close(fd);
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("[load] Model file found: ");
    vga_puts(path);
    vga_puts("\n[load] LLM loader not yet implemented (Phase 7.6)\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    klog("[shell] load command: file exists but loader is Phase 7.6\n");
}

/* ── ai ─────────────────────────────────────────────────────── */
static void cmd_ai(int argc, char **argv) {
    if (argc < 2) {
        vga_puts("Usage: ai <prompt...>\n"); return;
    }
    /* Phase 7.9 will implement inference_prompt().
     * Stub: echo the prompt back with a placeholder response. */
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("You: ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    for (int i = 1; i < argc; i++) {
        vga_puts(argv[i]);
        if (i < argc - 1) vga_putchar(' ');
    }
    vga_putchar('\n');
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("AI:  ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("[LLM inference engine not yet loaded — Phase 7.9]\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    klog("[shell] ai command received (inference stub)\n");
}

/* ── chat ───────────────────────────────────────────────────── */
static void cmd_chat(int argc, char **argv) {
    (void)argc; (void)argv;

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("Interactive chat mode — type 'exit' to return to shell.\n");
    vga_puts("LLM engine not yet loaded (Phase 7.9).\n\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    static char chat_line[SHELL_LINE_MAX];
    for (;;) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_puts("You> ");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

        terminal_readline(chat_line, SHELL_LINE_MAX);
        vga_putchar('\n');

        /* check for exit */
        char *cv[2]; int ca = shell_tokenize(chat_line, cv, 2);
        if (ca > 0 && sh_strcmp(cv[0], "exit") == 0) break;

        if (ca == 0) continue;

        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_puts("AI>  ");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_puts("[inference stub — Phase 7.9]\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }

    vga_puts("Exiting chat mode.\n");
}

/* ── echo ───────────────────────────────────────────────────── */
static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        vga_puts(argv[i]);
        if (i < argc - 1) vga_putchar(' ');
    }
    vga_putchar('\n');
}

/* ── hexdump ────────────────────────────────────────────────── */
static void cmd_hexdump(int argc, char **argv) {
    if (argc < 2) { vga_puts("Usage: hexdump <file>\n"); return; }
    const char *path = argv[1];

    const uint8_t *data = NULL;
    size_t size = 0;
    static uint8_t hd_buf[256];
    bool from_initrd = false;

    if (vfs_initrd_open(path, &data, &size) == 0) {
        from_initrd = true;
    } else {
        int fd = vfs_open(path);
        if (fd < 0) {
            vga_puts_color("hexdump: not found: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_puts(path); vga_putchar('\n'); return;
        }
        int32_t n = vfs_read(fd, hd_buf, 256);
        vfs_close(fd);
        if (n < 0) { vga_puts("hexdump: read error\n"); return; }
        data = hd_buf;
        size = (size_t)n;
    }

    size_t lim = size < 256 ? size : 256;
    for (size_t off = 0; off < lim; off += 16) {
        /* offset */
        sh_putx8((uint8_t)(off >> 8)); sh_putx8((uint8_t)off); vga_puts("  ");
        /* hex bytes */
        for (size_t j = 0; j < 16; j++) {
            if (off + j < lim) { sh_putx8(data[off+j]); vga_putchar(' '); }
            else vga_puts("   ");
            if (j == 7) vga_putchar(' ');
        }
        vga_puts(" |");
        /* ASCII */
        for (size_t j = 0; j < 16 && off + j < lim; j++) {
            char c = (char)data[off+j];
            vga_putchar((c >= 0x20 && c < 0x7F) ? c : '.');
        }
        vga_puts("|\n");
    }
    (void)from_initrd;
}

/* ── reboot ─────────────────────────────────────────────────── */
static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    vga_puts("Rebooting...\n");
    klog("[shell] reboot requested\n");
    /* Phase 5.3 (ACPI) will use acpi_reboot().
     * Fallback: PS/2 keyboard controller pulse reset line. */
    __asm__ volatile ("cli");
    /* wait for kbd controller input buffer empty */
    uint8_t status;
    do {
        __asm__ volatile ("inb $0x64, %0" : "=a"(status));
    } while (status & 0x02);
    /* pulse reset */
    __asm__ volatile ("outb %0, $0x64" :: "a"((uint8_t)0xFE));
    /* if that didn't work, triple-fault */
    __asm__ volatile ("lidt 0");
    __asm__ volatile ("int $0");
    for (;;) __asm__ volatile ("hlt");
}

/* ── shutdown ───────────────────────────────────────────────── */
static void cmd_shutdown(int argc, char **argv) {
    (void)argc; (void)argv;
    vga_puts("Shutting down...\n");
    klog("[shell] shutdown requested\n");
    /*
     * Phase 5.3 (ACPI) will implement acpi_shutdown().
     * QEMU ACPI power-off: write 0x2000 to port 0x604 (QEMU-specific ACPI port).
     * Bochs: port 0xB004, value 0x2000.
     * Real hardware: parse FADT → PM1a_CNT.
     */
    __asm__ volatile ("cli");
    /* QEMU: ACPI power off */
    __asm__ volatile ("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    /* Bochs/old QEMU fallback */
    __asm__ volatile ("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));
    /* OVMF / UEFI QEMU fallback */
    __asm__ volatile ("outw %0, %1" :: "a"((uint16_t)0x34), "Nd"((uint16_t)0x600));
    vga_puts("ACPI shutdown failed — halting.\n");
    for (;;) __asm__ volatile ("hlt");
}

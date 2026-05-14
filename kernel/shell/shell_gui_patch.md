# Phase 10.6 — `startx` Shell Command Patch

Add the `startx` / `gui` command to `kernel/shell/shell.c`.

## Changes Required in `shell.c`

### 1. Include headers at top of shell.c

```c
#include "gui/wm.h"
#include "gui/input_wiring.h"
```

### 2. Add handler in `shell_run_command()`

Find the `if/else if` command dispatch chain and add:

```c
} else if (cmd_is(argv[0], "startx") || cmd_is(argv[0], "gui")) {
    /* Phase 10.6: Switch from text-mode shell to GUI */
    vga_puts("[GUI] Switching to graphical mode...\n");
    /* Tell the input wiring to start forwarding events to GUI queue */
    gui_input_wiring_install();
    gui_input_wiring_set_active(true);
    /* Launch the window manager thread */
    gui_wm_start();
    /* Shell task suspends itself; GUI thread now owns keyboard/mouse */
    /* (Return to text mode only by rebooting in Phase 10.6.) */
    sched_sleep(0x7FFFFFFF);   /* effectively block forever */
```

### 3. Add to help text

In the `help` command block add:
```c
vga_puts_color("  startx      ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
vga_puts("Switch to graphical desktop (GUI mode)\n");
```

## Result

After this patch:
- User types `startx` at the shell prompt.
- GUI input wiring is installed (hooks into mouse/keyboard IRQ handlers).
- `gui_wm_start()` spawns the WM kthread which initialises the
  framebuffer, desktop, taskbar, start menu, and enters the event loop.
- The shell kthread sleeps indefinitely (returns to text mode via reboot).

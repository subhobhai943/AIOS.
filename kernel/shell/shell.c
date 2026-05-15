@@ -10,6 +10,7 @@
 #include "shell.h"
 #include "terminal.h"
 
 #include "../vga.h"
+#include "../apps/terminal_gui.h"  /* Phase 11.3 GUI terminal */
 #include "../serial.h"
 #include "../heap.h"
 #include "../pmm.h"
@@ -63,6 +64,14 @@ static void sh_putx8(uint8_t v) {
     vga_putchar(hex[(v >> 4) & 0xF]);
     vga_putchar(hex[v & 0xF]);
 }
+
+/* GUI terminal hook: optional windowed view of shell output.
+ * For now we just ensure there is at least one instance at shell
+ * startup when the GUI is running (startx).  Future work can
+ * add a dedicated shell command or Start-menu entry to spawn
+ * additional terminals.
+ */
+extern terminal_gui_t *terminal_gui_open(void);
@@ -152,6 +161,9 @@ void shell_run(void *arg) {
     vga_puts("AIOS Shell — Phase 5.2  |  type 'help' for commands\n");
     vga_puts("type 'startx' to launch the graphical desktop\n\n");
     klog("[shell] shell_run started\n");
+
+    /* If GUI WM is active, ensure a GUI terminal exists. */
+    terminal_gui_open();
@@ -168,6 +180,10 @@ void shell_run(void *arg) {
         vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
 
         terminal_readline(line, SHELL_LINE_MAX);
+        /* terminal_readline internally uses terminal_write* and
+         * terminal_feed; our GUI adapters mirror those into the
+         * window when present.
+         */
         vga_putchar('\n');
 
         dispatch(line);

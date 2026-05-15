@@ -1,5 +1,6 @@
 #pragma once
 #include <stdint.h>
 #include <stddef.h>
 #include <stdbool.h>
@@ -76,6 +77,11 @@ size_t terminal_readline(char *buf, size_t maxlen);
  */
 void terminal_write(const char *str);
 
+/* GUI adapter will mirror terminal output into a GUI window when
+ * present (Phase 11.3). terminal.c calls both VGA and terminal_gui
+ * functions so text is visible in text-mode and GUI simultaneously.
+ */
+
 /**
  * terminal_write_len(str, len) — same but length-bounded.
  */
 void terminal_write_len(const char *str, size_t len);

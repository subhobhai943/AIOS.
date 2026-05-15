@@ -101,6 +101,9 @@ void term_move_cursor(uint8_t col, uint8_t row)
 {
     cursor_col = (col >= TERM_COLS) ? (TERM_COLS - 1) : col;
     cursor_row = (row >= TERM_ROWS) ? (TERM_ROWS - 1) : row;
+
+    /* Mirror cursor movement into GUI terminal if present. */
+    terminal_gui_move_cursor(cursor_col, cursor_row);
@@ -109,6 +112,10 @@ void term_set_color(uint8_t fg, uint8_t bg)
 {
     term_color = (fg & 0x0F) | ((bg & 0x0F) << 4);
+
+    /* GUI terminal maintains its own colours; we pass logical
+     * values so it can decide how to map them.
+     */
+    terminal_gui_set_color(fg, bg);
@@ -116,6 +123,9 @@ void term_reset_color(void)
 {
     term_color = (TERM_FG_LGRAY | (TERM_BG_BLACK << 4));
+    terminal_gui_reset_color();
@@ -132,6 +140,9 @@ void term_clear_line_to_end(void)
         term_buffer[row][col + 1] = entry;
     }
+
+    terminal_gui_clear_line_to_end();
@@ -139,6 +150,9 @@ void term_clear_line(void)
         term_buffer[row][col] = entry;
     }
+    terminal_gui_clear_line();
@@ -150,6 +164,9 @@ void term_clear_screen(void)
         }
     }
     term_move_cursor(0, 0);
+
+    terminal_gui_clear_screen();
@@ -158,6 +175,10 @@ uint8_t term_cursor_col(void)
 {
     return cursor_col;
 }
@@ -165,6 +186,10 @@ uint8_t term_cursor_row(void)
 {
     return cursor_row;
 }

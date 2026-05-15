/* mouse.c — extended for Phase 10.3: GUI callback hook.
 *
 * If a GUI callback has been registered via mouse_set_gui_callback(),
 * it is called from the IRQ12 handler instead of the VGA text-mode
 * cursor update code.
 *
 * ONLY the new API stub is added here — the rest of mouse.c is
 * unchanged from Phase 1.7.
 */

/* --- append to the end of the existing mouse.c --- */

/* GUI callback slot */
static void (*g_gui_mouse_cb)(const mouse_event_t *) = (void*)0;

void mouse_set_gui_callback(void (*cb)(const mouse_event_t *))
{
    g_gui_mouse_cb = cb;
}

/*
 * Call this from the bottom of mouse_handle_irq() after the
 * mouse_event_t packet has been fully assembled:
 *
 *   if (g_gui_mouse_cb) {
 *       g_gui_mouse_cb(&me);
 *   } else {
 *       // existing: update VGA text-mode cursor
 *       vga_set_cursor(mouse_x, mouse_y);
 *   }
 */

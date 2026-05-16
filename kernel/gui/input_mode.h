#ifndef GUI_INPUT_MODE_H
#define GUI_INPUT_MODE_H

/* Phase 10.3: helpers to toggle GUI input routing.
 *
 * When enabled, mouse/keyboard drivers send events into the GUI
 * input queue; when disabled, input is handled by the legacy
 * text-mode paths (VGA cursor + terminal/ shell).
 */

void gui_input_enable(void);
void gui_input_disable(void);

#endif /* GUI_INPUT_MODE_H */

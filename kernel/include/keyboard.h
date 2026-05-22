#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * AIOS — PS/2 Keyboard Driver (Phase 1)
 * ================================================================ */

/* Decoded key event passed to high-level consumers. */
typedef struct {
    uint8_t  scancode;   /* raw PS/2 scancode           */
    char     ascii;      /* decoded ASCII (0 if none)   */
    bool     pressed;    /* true = down, false = up     */
    bool     shift;      /* Shift modifier held         */
    bool     ctrl;       /* Ctrl modifier held          */
    bool     alt;        /* Alt modifier held           */
} key_event_t;

/* Callback types */
typedef void (*keyboard_gui_cb_t)(const key_event_t *ke);
typedef void (*keyboard_text_cb_t)(char ascii, uint8_t scancode, bool pressed);

void keyboard_init(void);
void keyboard_handle_irq(void);

/* GUI path: when set, all key events go to the GUI queue. */
void keyboard_set_gui_callback(keyboard_gui_cb_t cb);

/* Text path: when set (and GUI callback is NULL), keys feed the terminal. */
void keyboard_set_text_callback(keyboard_text_cb_t cb);

/* Poll / peek (ring-buffer API used by legacy paths). */
bool     keyboard_has_key(void);
char     keyboard_getc(void);        /* blocking */
char     keyboard_poll(void);        /* non-blocking, returns 0 if empty */

#endif /* KEYBOARD_H */

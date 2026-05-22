#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * AIOS — PS/2 Keyboard Driver
 * ================================================================ */

/* I/O port addresses */
#define KBD_DATA_PORT   0x60u
#define KBD_STATUS_PORT 0x64u

/* Status register bits */
#define KBD_STATUS_OBF  0x01u   /* Output buffer full — data ready to read */
#define KBD_STATUS_IBF  0x02u   /* Input buffer full  — wait before write  */

/* Ring-buffer capacity (must be power of 2) */
#define KBD_BUF_SIZE    64u

/* Decoded key event passed to high-level consumers. */
typedef struct {
    uint8_t  scancode;    /* raw PS/2 scancode (0x80 set = extended)  */
    char     ascii;       /* decoded ASCII (0 if non-printable)        */
    uint8_t  pressed;     /* 1 = key down, 0 = key up                  */
    uint8_t  shift;       /* Shift modifier held                        */
    uint8_t  ctrl;        /* Ctrl modifier held                         */
    uint8_t  alt;         /* Alt modifier held                          */
    uint8_t  caps;        /* Caps Lock active                           */
} key_event_t;

/* Callback types */
typedef void (*keyboard_gui_cb_t)(const key_event_t *ke);
typedef void (*keyboard_text_cb_t)(char ascii, uint8_t scancode, bool pressed);

/* ---- Public API ---- */
void keyboard_init(void);
void keyboard_handle_irq(void);

/* GUI path: when set, every key event is delivered to the GUI callback. */
void keyboard_set_gui_callback(keyboard_gui_cb_t cb);

/* Text path: when GUI callback is NULL, keys feed this text callback.
 * terminal.c installs terminal_feed() here. */
void keyboard_set_text_callback(keyboard_text_cb_t cb);

/* Poll / peek — used by legacy blocking paths (shell, early boot). */
bool keyboard_has_key(void);    /* true if ring buffer non-empty        */
char keyboard_getc(void);       /* blocking: sleeps with HLT until key  */
char keyboard_poll(void);       /* non-blocking: returns 0 if no key    */

/* Direct ring-buffer read — returns full event, false if empty. */
bool keyboard_get_event(key_event_t *out);

#endif /* KEYBOARD_H */

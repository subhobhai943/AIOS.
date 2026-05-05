#ifndef KEYBOARD_H
#define KEYBOARD_H

/* ─── basic types ───────────────────────── */
typedef unsigned char uint8_t;

/* ─── constants ─────────────────────────── */
#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64
#define KBD_BUF_SIZE    128

/* ─── key event struct (FIXED) ──────────── */
typedef struct {
    uint8_t scancode;
    uint8_t ascii;
    uint8_t shift;
    uint8_t ctrl;
} key_event_t;

/* ─── functions ─────────────────────────── */
void keyboard_init(void);
void keyboard_handle_irq(void);
int  keyboard_get_event(key_event_t *out);

#endif

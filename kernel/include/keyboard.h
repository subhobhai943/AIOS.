#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/* PS/2 keyboard I/O ports */
#define KBD_DATA_PORT    0x60
#define KBD_STATUS_PORT  0x64
#define KBD_CMD_PORT     0x64

/* Ring buffer size (must be power of 2) */
#define KBD_BUF_SIZE  64

/* A single decoded key event */
typedef struct {
    uint8_t scancode;   /* raw PS/2 scancode (set 1) */
    uint8_t ascii;      /* translated ASCII, 0 if non-printable */
    uint8_t shift;      /* 1 if Shift was held */
    uint8_t ctrl;       /* 1 if Ctrl was held  */
} key_event_t;

void keyboard_init(void);
void keyboard_handle_irq(void);
int  keyboard_get_event(key_event_t *out);

#endif /* KEYBOARD_H */

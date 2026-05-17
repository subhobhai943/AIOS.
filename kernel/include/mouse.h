#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

/* PS/2 ports (shared with keyboard controller) */
#define MOUSE_DATA_PORT    0x60
#define MOUSE_CMD_PORT     0x64   /* write commands here  */
#define MOUSE_STATUS_PORT  0x64   /* read status here     */

/* Status register bits */
#define MOUSE_STATUS_OBF   0x01   /* output buffer full   */
#define MOUSE_STATUS_IBF   0x02   /* input  buffer full   */
#define MOUSE_STATUS_AUX   0x20   /* data is from aux port*/

/* Mouse button bitmask (first packet byte) */
#define MOUSE_BTN_LEFT    0x01
#define MOUSE_BTN_RIGHT   0x02
#define MOUSE_BTN_MIDDLE  0x04

#define MOUSE_BUF_SIZE  64

typedef struct {
    int16_t dx;        /* signed X delta              */
    int16_t dy;        /* signed Y delta (Y-inverted) */
    int32_t abs_x;     /* absolute X position         */
    int32_t abs_y;     /* absolute Y position         */
    uint8_t buttons;   /* button bitmask              */
    uint8_t prev_buttons;
} mouse_event_t;

void mouse_init(void);
void mouse_handle_irq(void);
int  mouse_get_event(mouse_event_t *out);

/* Current absolute position (clamped to screen) */
extern int mouse_x;
extern int mouse_y;

/* Optional GUI callback hook (Phase 10.3): when set, mouse_handle_irq()
 * should call this with each assembled mouse_event_t instead of (or in addition
 * to) updating the VGA text-mode cursor. The GUI bridge will convert these
 * events into gui_event_t structures for the window manager.
 */
void mouse_set_gui_callback(void (*cb)(const mouse_event_t *));

#endif /* MOUSE_H */

#ifndef GUI_INPUT_H
#define GUI_INPUT_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Minimal GUI input abstraction for AIOS.
 *
 * This sits above the low-level keyboard/mouse drivers and exposes a simple
 * event queue that the window manager thread can consume.
 */

typedef enum gui_event_type {
    GUI_EVENT_NONE = 0,
    GUI_EVENT_MOUSE_MOVE,
    GUI_EVENT_MOUSE_DOWN,
    GUI_EVENT_MOUSE_UP,
    GUI_EVENT_KEY_DOWN,
    GUI_EVENT_KEY_UP,
} gui_event_type_t;

/* Mouse button bitmask. */
#define GUI_MOUSE_BUTTON_LEFT   0x01u
#define GUI_MOUSE_BUTTON_RIGHT  0x02u
#define GUI_MOUSE_BUTTON_MIDDLE 0x04u

/* Mouse flags (e.g., double-click). */
#define GUI_MOUSE_FLAG_NONE         0x00u
#define GUI_MOUSE_FLAG_DOUBLE_CLICK 0x01u

/* Modifier bitmask for keyboard events. */
#define GUI_MOD_SHIFT 0x01u
#define GUI_MOD_CTRL  0x02u
#define GUI_MOD_ALT   0x04u

typedef struct gui_event {
    gui_event_type_t type;

    /* Timestamps in milliseconds since boot (PIT ticks at 1 kHz). */
    uint32_t time_ms;

    /* Mouse data (for mouse events). */
    int32_t  x;
    int32_t  y;
    uint8_t  buttons;     /* Current button state bitmask. */
    uint8_t  mouse_flags; /* GUI_MOUSE_FLAG_* */

    /* Keyboard data (for key events). */
    uint8_t  keycode;   /* Implementation-defined: ASCII or scan code. */
    uint8_t  modifiers; /* GUI_MOD_* bitmask. */
} gui_event_t;

/* Global mouse state in framebuffer coordinates. */
typedef struct gui_mouse_state {
    int32_t x;
    int32_t y;
    uint8_t buttons; /* GUI_MOUSE_BUTTON_* */
} gui_mouse_state_t;

/* Initialise GUI input with framebuffer dimensions. */
void gui_input_init(uint32_t screen_width, uint32_t screen_height);

/* Retrieve current mouse state. Safe to call from GUI thread. */
void gui_input_get_mouse_state(gui_mouse_state_t *out_state);

/* Non-blocking poll for the next GUI event. Returns false when queue empty. */
bool gui_input_poll_event(gui_event_t *out_ev);

/* Blocking wait for the next GUI event (yields CPU while waiting). */
void gui_input_wait_event(gui_event_t *out_ev);

/* Low-level producers: these will typically be called from the mouse/keyboard
 * handling paths to feed raw input into the GUI queue.
 */

/* Feed a mouse delta + new button state (relative movement). */
void gui_input_push_mouse_delta(int dx, int dy, uint8_t buttons);

/* Feed an absolute mouse position in framebuffer coordinates + button state. */
void gui_input_push_mouse_absolute(int32_t x, int32_t y, uint8_t buttons);

/* Feed keyboard press/release events. */
void gui_input_push_key_down(uint8_t keycode, uint8_t modifiers);
void gui_input_push_key_up(uint8_t keycode, uint8_t modifiers);

#endif /* GUI_INPUT_H */

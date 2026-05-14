#include "gui/input.h"

#include "sync.h"
#include "sched.h"
#include "pit.h"

/* Simple single-producer/single-consumer ring buffer for GUI events. */

#define GUI_EVENT_QUEUE_SIZE 128

static gui_event_t       g_event_queue[GUI_EVENT_QUEUE_SIZE];
static volatile uint32_t g_event_head = 0; /* next write */
static volatile uint32_t g_event_tail = 0; /* next read */

static gui_mouse_state_t g_mouse_state;
static uint32_t          g_screen_w = 0;
static uint32_t          g_screen_h = 0;

static spinlock_t g_gui_input_lock = SPINLOCK_INIT;

/* Double-click detection. */
static uint32_t g_last_click_time_ms = 0;
static int32_t  g_last_click_x       = 0;
static int32_t  g_last_click_y       = 0;

static uint32_t gui_input_now_ms(void)
{
    /* PIT ticks at 1 kHz in current kernel; convert directly. */
    return (uint32_t)pit_get_ticks();
}

static void gui_input_enqueue(const gui_event_t *ev)
{
    spin_lock(&g_gui_input_lock);

    uint32_t next_head = (g_event_head + 1u) & (GUI_EVENT_QUEUE_SIZE - 1u);
    if (next_head == g_event_tail) {
        /* Queue full: drop the event for now. In future, log or expand. */
        spin_unlock(&g_gui_input_lock);
        return;
    }

    g_event_queue[g_event_head] = *ev;
    g_event_head = next_head;

    spin_unlock(&g_gui_input_lock);
}

void gui_input_init(uint32_t screen_width, uint32_t screen_height)
{
    spin_lock(&g_gui_input_lock);

    g_screen_w = screen_width;
    g_screen_h = screen_height;

    g_mouse_state.x = (int32_t)(screen_width / 2u);
    g_mouse_state.y = (int32_t)(screen_height / 2u);
    g_mouse_state.buttons = 0;

    g_event_head = 0;
    g_event_tail = 0;

    g_last_click_time_ms = 0;
    g_last_click_x = g_mouse_state.x;
    g_last_click_y = g_mouse_state.y;

    spin_unlock(&g_gui_input_lock);
}

void gui_input_get_mouse_state(gui_mouse_state_t *out_state)
{
    if (!out_state) return;
    spin_lock(&g_gui_input_lock);
    *out_state = g_mouse_state;
    spin_unlock(&g_gui_input_lock);
}

bool gui_input_poll_event(gui_event_t *out_ev)
{
    if (!out_ev) return false;

    bool has_event = false;

    spin_lock(&g_gui_input_lock);
    if (g_event_head != g_event_tail) {
        *out_ev = g_event_queue[g_event_tail];
        g_event_tail = (g_event_tail + 1u) & (GUI_EVENT_QUEUE_SIZE - 1u);
        has_event = true;
    }
    spin_unlock(&g_gui_input_lock);

    return has_event;
}

void gui_input_wait_event(gui_event_t *out_ev)
{
    while (!gui_input_poll_event(out_ev)) {
        sched_yield();
    }
}

static void gui_input_update_mouse_pos_locked(int32_t x, int32_t y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (g_screen_w > 0 && x >= (int32_t)g_screen_w) {
        x = (int32_t)g_screen_w - 1;
    }
    if (g_screen_h > 0 && y >= (int32_t)g_screen_h) {
        y = (int32_t)g_screen_h - 1;
    }
    g_mouse_state.x = x;
    g_mouse_state.y = y;
}

void gui_input_push_mouse_delta(int dx, int dy, uint8_t buttons)
{
    gui_event_t ev;

    spin_lock(&g_gui_input_lock);

    int32_t new_x = g_mouse_state.x + dx;
    int32_t new_y = g_mouse_state.y + dy;
    gui_input_update_mouse_pos_locked(new_x, new_y);

    g_mouse_state.buttons = buttons;

    ev.type      = GUI_EVENT_MOUSE_MOVE;
    ev.time_ms   = gui_input_now_ms();
    ev.x         = g_mouse_state.x;
    ev.y         = g_mouse_state.y;
    ev.buttons   = g_mouse_state.buttons;
    ev.mouse_flags = GUI_MOUSE_FLAG_NONE;
    ev.keycode   = 0;
    ev.modifiers = 0;

    spin_unlock(&g_gui_input_lock);

    gui_input_enqueue(&ev);
}

void gui_input_push_mouse_absolute(int32_t x, int32_t y, uint8_t buttons)
{
    gui_event_t ev;

    spin_lock(&g_gui_input_lock);

    gui_input_update_mouse_pos_locked(x, y);
    g_mouse_state.buttons = buttons;

    ev.type      = GUI_EVENT_MOUSE_MOVE;
    ev.time_ms   = gui_input_now_ms();
    ev.x         = g_mouse_state.x;
    ev.y         = g_mouse_state.y;
    ev.buttons   = g_mouse_state.buttons;
    ev.mouse_flags = GUI_MOUSE_FLAG_NONE;
    ev.keycode   = 0;
    ev.modifiers = 0;

    spin_unlock(&g_gui_input_lock);

    gui_input_enqueue(&ev);
}

static void gui_input_push_mouse_button_internal(bool is_down,
                                                 uint8_t button_mask)
{
    gui_event_t ev;

    spin_lock(&g_gui_input_lock);

    uint8_t before = g_mouse_state.buttons;
    if (is_down) {
        g_mouse_state.buttons |= button_mask;
    } else {
        g_mouse_state.buttons &= (uint8_t)~button_mask;
    }

    ev.type      = is_down ? GUI_EVENT_MOUSE_DOWN : GUI_EVENT_MOUSE_UP;
    ev.time_ms   = gui_input_now_ms();
    ev.x         = g_mouse_state.x;
    ev.y         = g_mouse_state.y;
    ev.buttons   = g_mouse_state.buttons;
    ev.mouse_flags = GUI_MOUSE_FLAG_NONE;
    ev.keycode   = 0;
    ev.modifiers = 0;

    /* Naive double-click detection for left button. */
    if (is_down && (button_mask & GUI_MOUSE_BUTTON_LEFT) && !(before & GUI_MOUSE_BUTTON_LEFT)) {
        uint32_t dt = ev.time_ms - g_last_click_time_ms;
        int32_t dx = (ev.x - g_last_click_x);
        int32_t dy = (ev.y - g_last_click_y);
        if (dt <= 300u && (dx*dx + dy*dy) <= 25) {
            ev.mouse_flags |= GUI_MOUSE_FLAG_DOUBLE_CLICK;
        }
        g_last_click_time_ms = ev.time_ms;
        g_last_click_x = ev.x;
        g_last_click_y = ev.y;
    }

    spin_unlock(&g_gui_input_lock);

    gui_input_enqueue(&ev);
}

void gui_input_push_key_down(uint8_t keycode, uint8_t modifiers)
{
    gui_event_t ev;
    ev.type      = GUI_EVENT_KEY_DOWN;
    ev.time_ms   = gui_input_now_ms();
    ev.x         = g_mouse_state.x;
    ev.y         = g_mouse_state.y;
    ev.buttons   = g_mouse_state.buttons;
    ev.mouse_flags = GUI_MOUSE_FLAG_NONE;
    ev.keycode   = keycode;
    ev.modifiers = modifiers;

    gui_input_enqueue(&ev);
}

void gui_input_push_key_up(uint8_t keycode, uint8_t modifiers)
{
    gui_event_t ev;
    ev.type      = GUI_EVENT_KEY_UP;
    ev.time_ms   = gui_input_now_ms();
    ev.x         = g_mouse_state.x;
    ev.y         = g_mouse_state.y;
    ev.buttons   = g_mouse_state.buttons;
    ev.mouse_flags = GUI_MOUSE_FLAG_NONE;
    ev.keycode   = keycode;
    ev.modifiers = modifiers;

    gui_input_enqueue(&ev);
}

#include "include/mouse.h"

#include "sync.h"

#include <stdint.h>

#define MOUSE_CMD_ENABLE_AUX 0xA8u
#define MOUSE_CMD_READ_CB    0x20u
#define MOUSE_CMD_WRITE_CB   0x60u
#define MOUSE_CMD_WRITE_NEXT 0xD4u
#define MOUSE_DEV_ENABLE     0xF4u
#define MOUSE_DEV_DEFAULTS   0xF6u

#define MOUSE_CB_IRQ1        0x01u
#define MOUSE_CB_IRQ12       0x02u
#define MOUSE_CB_AUX_CLOCK   0x20u

int mouse_x = 400;
int mouse_y = 300;

static spinlock_t g_mouse_lock = SPINLOCK_INIT;

static mouse_event_t g_mouse_buf[MOUSE_BUF_SIZE];
static uint32_t g_mouse_head = 0;
static uint32_t g_mouse_tail = 0;

static uint8_t g_packet[3];
static uint8_t g_packet_index = 0;
static uint8_t g_buttons = 0;

static void (*g_gui_mouse_cb)(const mouse_event_t *) = (void *)0;

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static int mouse_wait_input_clear(void)
{
    for (uint32_t i = 0; i < 100000u; i++) {
        if ((inb(MOUSE_STATUS_PORT) & MOUSE_STATUS_IBF) == 0) {
            return 1;
        }
        __asm__ volatile ("pause");
    }
    return 0;
}

static int mouse_wait_output_full(void)
{
    for (uint32_t i = 0; i < 100000u; i++) {
        if (inb(MOUSE_STATUS_PORT) & MOUSE_STATUS_OBF) {
            return 1;
        }
        __asm__ volatile ("pause");
    }
    return 0;
}

static void mouse_write_device(uint8_t value)
{
    if (!mouse_wait_input_clear()) return;
    outb(MOUSE_CMD_PORT, MOUSE_CMD_WRITE_NEXT);
    if (!mouse_wait_input_clear()) return;
    outb(MOUSE_DATA_PORT, value);
    if (mouse_wait_output_full()) {
        (void)inb(MOUSE_DATA_PORT);
    }
}

static uint8_t mouse_read_controller_config(void)
{
    if (!mouse_wait_input_clear()) return 0;
    outb(MOUSE_CMD_PORT, MOUSE_CMD_READ_CB);
    if (!mouse_wait_output_full()) return 0;
    return inb(MOUSE_DATA_PORT);
}

static void mouse_write_controller_config(uint8_t value)
{
    if (!mouse_wait_input_clear()) return;
    outb(MOUSE_CMD_PORT, MOUSE_CMD_WRITE_CB);
    if (!mouse_wait_input_clear()) return;
    outb(MOUSE_DATA_PORT, value);
}

static void mouse_buffer_push(const mouse_event_t *ev)
{
    uint32_t next = (g_mouse_head + 1u) & (MOUSE_BUF_SIZE - 1u);
    if (next == g_mouse_tail) {
        return;
    }

    g_mouse_buf[g_mouse_head] = *ev;
    g_mouse_head = next;
}

static int16_t sign_extend_packet_delta(uint8_t value, uint8_t sign_bit)
{
    int16_t delta = (int16_t)value;
    if (sign_bit) {
        delta |= (int16_t)0xFF00;
    }
    return delta;
}

static void clamp_mouse_position(void)
{
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x > 1023) mouse_x = 1023;
    if (mouse_y > 767) mouse_y = 767;
}

static void mouse_process_packet(void)
{
    uint8_t flags = g_packet[0];
    if ((flags & 0x08u) == 0) {
        g_packet_index = 0;
        return;
    }

    int16_t dx = sign_extend_packet_delta(g_packet[1], flags & 0x10u);
    int16_t raw_dy = sign_extend_packet_delta(g_packet[2], flags & 0x20u);
    int16_t dy = (int16_t)-raw_dy;
    uint8_t prev = g_buttons;
    uint8_t buttons = (uint8_t)(flags & (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT | MOUSE_BTN_MIDDLE));

    mouse_x += dx;
    mouse_y += dy;
    clamp_mouse_position();
    g_buttons = buttons;

    mouse_event_t ev;
    ev.dx = dx;
    ev.dy = dy;
    ev.abs_x = mouse_x;
    ev.abs_y = mouse_y;
    ev.buttons = buttons;
    ev.prev_buttons = prev;

    mouse_buffer_push(&ev);
    if (g_gui_mouse_cb) {
        g_gui_mouse_cb(&ev);
    }
}

void mouse_set_gui_callback(void (*cb)(const mouse_event_t *))
{
    uint64_t flags = spin_lock_irqsave(&g_mouse_lock);
    g_gui_mouse_cb = cb;
    spin_unlock_irqrestore(&g_mouse_lock, flags);
}

void mouse_init(void)
{
    uint64_t flags = spin_lock_irqsave(&g_mouse_lock);
    g_mouse_head = 0;
    g_mouse_tail = 0;
    g_packet_index = 0;
    g_buttons = 0;
    g_gui_mouse_cb = (void *)0;
    mouse_x = 400;
    mouse_y = 300;
    spin_unlock_irqrestore(&g_mouse_lock, flags);

    if (!mouse_wait_input_clear()) return;
    outb(MOUSE_CMD_PORT, MOUSE_CMD_ENABLE_AUX);

    uint8_t cfg = mouse_read_controller_config();
    cfg |= (uint8_t)(MOUSE_CB_IRQ1 | MOUSE_CB_IRQ12);
    cfg &= (uint8_t)~MOUSE_CB_AUX_CLOCK;
    mouse_write_controller_config(cfg);

    mouse_write_device(MOUSE_DEV_DEFAULTS);
    mouse_write_device(MOUSE_DEV_ENABLE);

    while (inb(MOUSE_STATUS_PORT) & MOUSE_STATUS_OBF) {
        (void)inb(MOUSE_DATA_PORT);
    }
}

void mouse_handle_irq(void)
{
    uint64_t flags = spin_lock_irqsave(&g_mouse_lock);

    uint8_t status = inb(MOUSE_STATUS_PORT);
    if ((status & (MOUSE_STATUS_OBF | MOUSE_STATUS_AUX)) ==
        (MOUSE_STATUS_OBF | MOUSE_STATUS_AUX)) {
        uint8_t byte = inb(MOUSE_DATA_PORT);
        if (g_packet_index == 0 && (byte & 0x08u) == 0) {
            spin_unlock_irqrestore(&g_mouse_lock, flags);
            return;
        }

        g_packet[g_packet_index++] = byte;
        if (g_packet_index == 3) {
            g_packet_index = 0;
            mouse_process_packet();
        }
    }

    spin_unlock_irqrestore(&g_mouse_lock, flags);
}

int mouse_get_event(mouse_event_t *out)
{
    if (!out) return 0;

    uint64_t flags = spin_lock_irqsave(&g_mouse_lock);
    if (g_mouse_head == g_mouse_tail) {
        spin_unlock_irqrestore(&g_mouse_lock, flags);
        return 0;
    }

    *out = g_mouse_buf[g_mouse_tail];
    g_mouse_tail = (g_mouse_tail + 1u) & (MOUSE_BUF_SIZE - 1u);
    spin_unlock_irqrestore(&g_mouse_lock, flags);
    return 1;
}

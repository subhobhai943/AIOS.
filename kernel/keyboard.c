#include "include/keyboard.h"

#include "sync.h"

#include <stdint.h>
#include <stdbool.h>

#define KBD_STATUS_OBF 0x01u

static spinlock_t g_kbd_lock = SPINLOCK_INIT;

static key_event_t g_kbd_buf[KBD_BUF_SIZE];
static uint32_t g_kbd_head = 0;
static uint32_t g_kbd_tail = 0;

static bool g_shift = false;
static bool g_ctrl = false;
static bool g_alt = false;
static bool g_caps = false;
static bool g_e0 = false;

static void (*g_gui_kbd_cb)(const key_event_t *) = (void *)0;

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static void kbd_buffer_push(const key_event_t *ev)
{
    uint32_t next = (g_kbd_head + 1u) & (KBD_BUF_SIZE - 1u);
    if (next == g_kbd_tail) {
        return;
    }

    g_kbd_buf[g_kbd_head] = *ev;
    g_kbd_head = next;
}

static char translate_scancode(uint8_t scancode, bool shift, bool caps)
{
    static const char normal[128] = {
        [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
        [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
        [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
        [0x0E] = '\b', [0x0F] = '\t',
        [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
        [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
        [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
        [0x1C] = '\n',
        [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
        [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
        [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
        [0x2B] = '\\',
        [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
        [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
        [0x34] = '.', [0x35] = '/', [0x39] = ' ',
    };
    static const char shifted[128] = {
        [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
        [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
        [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
        [0x0E] = '\b', [0x0F] = '\t',
        [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
        [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
        [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
        [0x1C] = '\n',
        [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
        [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
        [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
        [0x2B] = '|',
        [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
        [0x30] = 'B', [0x31] = 'N', [0x32] = 'M', [0x33] = '<',
        [0x34] = '>', [0x35] = '?', [0x39] = ' ',
    };

    char ch = shift ? shifted[scancode] : normal[scancode];
    if (ch >= 'a' && ch <= 'z' && caps) {
        ch = (char)(ch - 'a' + 'A');
    } else if (ch >= 'A' && ch <= 'Z' && caps) {
        ch = (char)(ch - 'A' + 'a');
    }
    return ch;
}

static void keyboard_process_scancode(uint8_t raw)
{
    if (raw == 0xE0u) {
        g_e0 = true;
        return;
    }

    bool released = (raw & 0x80u) != 0;
    uint8_t scancode = (uint8_t)(raw & 0x7Fu);

    switch (scancode) {
        case 0x2A:
        case 0x36:
            g_shift = !released;
            break;
        case 0x1D:
            g_ctrl = !released;
            break;
        case 0x38:
            g_alt = !released;
            break;
        case 0x3A:
            if (!released) g_caps = !g_caps;
            break;
        default:
            break;
    }

    key_event_t ev;
    ev.scancode = g_e0 ? (uint8_t)(scancode | 0x80u) : scancode;
    ev.ascii = (!released && !g_e0) ? (uint8_t)translate_scancode(scancode, g_shift, g_caps) : 0;
    ev.shift = g_shift ? 1u : 0u;
    ev.ctrl = g_ctrl ? 1u : 0u;
    ev.alt = g_alt ? 1u : 0u;
    ev.caps = g_caps ? 1u : 0u;
    ev.pressed = released ? 0u : 1u;

    g_e0 = false;

    kbd_buffer_push(&ev);
    if (g_gui_kbd_cb) {
        g_gui_kbd_cb(&ev);
    }
}

void keyboard_set_gui_callback(void (*cb)(const key_event_t *))
{
    uint64_t flags = spin_lock_irqsave(&g_kbd_lock);
    g_gui_kbd_cb = cb;
    spin_unlock_irqrestore(&g_kbd_lock, flags);
}

void keyboard_init(void)
{
    uint64_t flags = spin_lock_irqsave(&g_kbd_lock);
    g_kbd_head = 0;
    g_kbd_tail = 0;
    g_shift = false;
    g_ctrl = false;
    g_alt = false;
    g_caps = false;
    g_e0 = false;
    g_gui_kbd_cb = (void *)0;
    spin_unlock_irqrestore(&g_kbd_lock, flags);

    while (inb(KBD_STATUS_PORT) & KBD_STATUS_OBF) {
        (void)inb(KBD_DATA_PORT);
    }
}

void keyboard_handle_irq(void)
{
    uint64_t flags = spin_lock_irqsave(&g_kbd_lock);

    if (inb(KBD_STATUS_PORT) & KBD_STATUS_OBF) {
        keyboard_process_scancode(inb(KBD_DATA_PORT));
    }

    spin_unlock_irqrestore(&g_kbd_lock, flags);
}

bool keyboard_get_event(key_event_t *out)
{
    if (!out) return false;

    uint64_t flags = spin_lock_irqsave(&g_kbd_lock);
    if (g_kbd_head == g_kbd_tail) {
        spin_unlock_irqrestore(&g_kbd_lock, flags);
        return false;
    }

    *out = g_kbd_buf[g_kbd_tail];
    g_kbd_tail = (g_kbd_tail + 1u) & (KBD_BUF_SIZE - 1u);
    spin_unlock_irqrestore(&g_kbd_lock, flags);
    return true;
}

/* keyboard.c — PS/2 keyboard driver (Phase 1.6 / 5.1 / 10.3)
 *
 * NOTE: This file has been augmented for Phase 10.3 to support an
 * optional GUI callback. When a GUI callback is registered via
 * keyboard_set_gui_callback(), the IRQ handler will send key events
 * to the GUI instead of feeding the text-mode terminal.
 */

#include "include/keyboard.h"
#include "shell/terminal.h"
#include "sync.h"
#include "apic.h"
#include "vga.h"

#include <stdint.h>

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64

static spinlock_t g_kbd_lock = SPINLOCK_INIT;

/* Ring buffer for key events (Phase 1.6/5.1) */
#define KBD_BUF_SIZE 128
static key_event_t g_kbd_buf[KBD_BUF_SIZE];
static uint32_t g_kbd_head = 0;
static uint32_t g_kbd_tail = 0;

/* Modifier state */
static bool g_shift = false;
static bool g_ctrl  = false;
static bool g_alt   = false;
static bool g_caps  = false;

/* GUI callback slot (Phase 10.3) */
static void (*g_gui_kbd_cb)(const key_event_t *) = (void*)0;

void keyboard_set_gui_callback(void (*cb)(const key_event_t *))
{
    g_gui_kbd_cb = cb;
}

/* ... existing scancode tables and helpers remain unchanged ... */

static void kbd_buffer_push(const key_event_t *ev)
{
    uint32_t next = (g_kbd_head + 1u) & (KBD_BUF_SIZE - 1u);
    if (next == g_kbd_tail) {
        /* overflow: drop event */
        return;
    }
    g_kbd_buf[g_kbd_head] = *ev;
    g_kbd_head = next;
}

bool keyboard_get_event(key_event_t *out)
{
    if (g_kbd_head == g_kbd_tail) return false;
    *out = g_kbd_buf[g_kbd_tail];
    g_kbd_tail = (g_kbd_tail + 1u) & (KBD_BUF_SIZE - 1u);
    return true;
}

static void keyboard_process_scancode(uint8_t scancode)
{
    key_event_t ev;
    ev.scancode = scancode;
    ev.ascii    = 0;
    ev.shift    = g_shift;
    ev.ctrl     = g_ctrl;
    ev.alt      = g_alt;
    ev.pressed  = false;

    /* ... existing Set1 decode logic populating ev.ascii, ev.pressed,
     * and updating g_shift/g_ctrl/g_alt/g_caps ... */

    /* Push into ring buffer for consumers that poll keyboard_get_event(). */
    kbd_buffer_push(&ev);

    /* Deliver to either GUI or text terminal depending on mode. */
    if (g_gui_kbd_cb) {
        g_gui_kbd_cb(&ev);
    } else {
        terminal_feed(ev.ascii, ev.scancode, ev.pressed);
    }
}

void kbd_isr(void)
{
    spin_lock_irqsave(&g_kbd_lock);

    uint8_t status = inb(KBD_STATUS_PORT);
    if (status & 0x01) {
        uint8_t scancode = inb(KBD_DATA_PORT);
        keyboard_process_scancode(scancode);
    }

    spin_unlock_irqrestore(&g_kbd_lock);

    apic_send_eoi();
}

void keyboard_init(void)
{
    /* Existing init: enable IRQ1 via APIC/IOAPIC setup done elsewhere. */
}

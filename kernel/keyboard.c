/* ============================================================
 * AIOS — PS/2 Keyboard Driver (IRQ1 / INT 0x21)
 * Scancode Set 1 → ASCII translation with Shift & Ctrl
 * support.  Typed characters are echoed to the VGA display.
 * ============================================================ */

#include "include/keyboard.h"
#include "include/vga.h"
#include <stdint.h>

/* ─── PORT I/O ─────────────────────────────────────────────── */
static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* ─── Scancode Set 1 → ASCII (normal + shifted) ──────────── */
static const uint8_t sc_to_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']', '\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.', '/', 0,
    '*', 0, ' ', 0,
    [0x3B ... 0x7F] = 0
};

static const uint8_t sc_to_ascii_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}', '\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|', 'Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,
    [0x3B ... 0x7F] = 0
};

/* ─── Key event ring buffer ──────────────────────────────── */
static key_event_t kbuf[KBD_BUF_SIZE];
static volatile int kbd_head = 0;
static volatile int kbd_tail = 0;

/* ─── Modifier state ─────────────────────────────────────── */
static uint8_t shift_held = 0;
static uint8_t ctrl_held  = 0;
static uint8_t caps_lock  = 0;

/* ─── PIC EOI ────────────────────────────────────────────── */
#define PIC1_CMD  0x20
#define PIC_EOI   0x20

/* ─── keyboard_init ──────────────────────────────────────── */
void keyboard_init(void)
{
    /* Flush any stale bytes in the PS/2 output buffer */
    int drain = 16;
    while ((inb(KBD_STATUS_PORT) & 0x01) && drain--)
        inb(KBD_DATA_PORT);

    /* Send "Enable Scanning" command to keyboard */
    /* Wait for input buffer to be empty first */
    int timeout = 100000;
    while ((inb(KBD_STATUS_PORT) & 0x02) && --timeout);
    outb(KBD_DATA_PORT, 0xF4);   /* 0xF4 = Enable Scanning */

    /* Wait for ACK (0xFA) */
    timeout = 100000;
    while (--timeout) {
        if (inb(KBD_STATUS_PORT) & 0x01) {
            uint8_t r = inb(KBD_DATA_PORT);
            if (r == 0xFA) break;   /* got ACK */
        }
    }

    vga_puts("  [ OK ] Keyboard (PS/2, IRQ1) ready — type to echo\n");
}

/* ─── keyboard_handle_irq (called from INT 0x21 wrapper) ─── */
void keyboard_handle_irq(void)
{
    /* Read scancode — must read even if we don't use it to
       clear the keyboard controller's output buffer. */
    if (!(inb(KBD_STATUS_PORT) & 0x01)) return;   /* nothing to read */
    uint8_t sc = inb(KBD_DATA_PORT);

    /* ── Key release ─────────────────────── */
    if (sc & 0x80) {
        sc &= 0x7F;
        if (sc == 42 || sc == 54) shift_held = 0;
        if (sc == 29)             ctrl_held  = 0;
        return;
    }

    /* ── Modifier press ──────────────────── */
    if (sc == 42 || sc == 54) { shift_held = 1; return; }
    if (sc == 29)             { ctrl_held  = 1; return; }
    if (sc == 58)             { caps_lock ^= 1; return; }  /* Caps Lock toggle */

    /* ── Resolve ASCII ───────────────────── */
    uint8_t letter_key = (sc >= 0x10 && sc <= 0x19) ||
                         (sc >= 0x1E && sc <= 0x26) ||
                         (sc >= 0x2C && sc <= 0x32);

    int use_shift = shift_held;
    if (caps_lock && letter_key) use_shift ^= 1;  /* Caps Lock flips letters */

    uint8_t ch = use_shift ? sc_to_ascii_shift[sc] : sc_to_ascii[sc];

    /* ── Build event ─────────────────────── */
    key_event_t ev;
    ev.scancode = sc;
    ev.ascii    = ch;
    ev.shift    = shift_held;
    ev.ctrl     = ctrl_held;

    int next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next != kbd_tail) {
        kbuf[kbd_head] = ev;
        kbd_head = next;
    }

    /* ── Echo to VGA ─────────────────────── */
    if (ch == '\b') {
        vga_backspace();          /* defined in vga.c — erase last char */
    } else if (ch) {
        char s[2] = { (char)ch, '\0' };
        vga_puts(s);
    }
}

/* ─── keyboard_get_event ─────────────────────────────────── */
int keyboard_get_event(key_event_t *out)
{
    if (kbd_head == kbd_tail) return 0;
    *out = kbuf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return 1;
}

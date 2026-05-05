#include "keyboard.h"
#include "vga.h"

/* ─── basic types ───────────────────── */
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;

/* ─── PORT I/O ─────────────────────── */
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

/* ─── ASCII tables ─────────────────── */
static const uint8_t sc_to_ascii[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,
    '*',0,' ',0,
    [0x3B ... 0x7F] = 0
};

static const uint8_t sc_to_ascii_shift[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,
    '*',0,' ',0,
    [0x3B ... 0x7F] = 0
};

/* ─── buffer ───────────────────────── */
static key_event_t buf[KBD_BUF_SIZE];
static volatile int head = 0;
static volatile int tail = 0;

static uint8_t shift = 0;
static uint8_t ctrl  = 0;

/* ─── init ─────────────────────────── */
void keyboard_init(void)
{
    while (inb(KBD_STATUS_PORT) & 0x01)
        inb(KBD_DATA_PORT);

    vga_puts("Keyboard ready\n");
}

/* ─── IRQ handler ─────────────────── */
void keyboard_handle_irq(void)
{
    uint8_t sc = inb(KBD_DATA_PORT);

    /* release */
    if (sc & 0x80)
    {
        sc &= 0x7F;
        if (sc == 42 || sc == 54) shift = 0;
        if (sc == 29) ctrl = 0;
        return;
    }

    /* press */
    if (sc == 42 || sc == 54) { shift = 1; return; }
    if (sc == 29) { ctrl = 1; return; }

    uint8_t ch = shift ? sc_to_ascii_shift[sc] : sc_to_ascii[sc];

    key_event_t ev;
    ev.scancode = sc;
    ev.ascii    = ch;
    ev.shift    = shift;
    ev.ctrl     = ctrl;

    int next = (head + 1) % KBD_BUF_SIZE;
    if (next != tail)
    {
        buf[head] = ev;
        head = next;
    }

    if (ch)
    {
        char s[2] = { ch, 0 };
        vga_puts(s);
    }
}

/* ─── read ─────────────────────────── */
int keyboard_get_event(key_event_t *out)
{
    if (head == tail) return 0;

    *out = buf[tail];
    tail = (tail + 1) % KBD_BUF_SIZE;
    return 1;
}

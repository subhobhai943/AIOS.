#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stddef.h>

/* Serial Port Driver — COM1 (0x3F8), 115200 baud, 8N1
 * Used for kernel debug logging without requiring a screen.
 * Output also visible in QEMU via: -serial stdio
 */

#define SERIAL_COM1  0x3F8
#define SERIAL_COM2  0x2F8

void   serial_init(uint16_t port, uint32_t baud);
void   serial_putchar(uint16_t port, char c);
void   serial_puts(uint16_t port, const char *str);
void   serial_putdec(uint16_t port, uint64_t val);   /* was serial_putu64 — renamed to match serial.c */
void   serial_puthex(uint16_t port, uint64_t val);
char   serial_getchar(uint16_t port);
int    serial_available(uint16_t port);

/* Convenience macros targeting COM1 */
#define klog(s)       serial_puts(SERIAL_COM1, s)
#define klog_dec(v)   serial_putdec(SERIAL_COM1, v)
#define klog_hex(v)   serial_puthex(SERIAL_COM1, v)

#endif /* SERIAL_H */

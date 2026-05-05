#include "pci.h"
#include "vga.h"

/* ─── basic types (freestanding) ───────────────────── */
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;

/* ─── storage ─────────────────────────────────────── */
static pci_device_t devices[PCI_MAX_DEVICES];
static int device_count = 0;

/* ─── PORT I/O (FIXED — THIS WAS YOUR BUG) ─────────── */
static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* ─── PCI config access ───────────────────────────── */
uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset)
{
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (offset & 0xFC);

    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset, uint32_t val)
{
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (offset & 0xFC);

    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

/* ─── enumerate ───────────────────────────────────── */
void pci_enumerate(void)
{
    device_count = 0;

    for (uint16_t bus = 0; bus < PCI_MAX_BUS; bus++) {
        for (uint8_t dev = 0; dev < PCI_MAX_DEVICE; dev++) {
            for (uint8_t fn = 0; fn < PCI_MAX_FUNCTION; fn++) {

                uint32_t id = pci_read(bus, dev, fn, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;

                if (device_count >= PCI_MAX_DEVICES) goto done;

                pci_device_t *d = &devices[device_count++];

                d->bus       = bus;
                d->device    = dev;
                d->function  = fn;
                d->vendor_id = id & 0xFFFF;
                d->device_id = (id >> 16) & 0xFFFF;

                uint32_t cls = pci_read(bus, dev, fn, 0x08);
                d->class_code = (cls >> 24) & 0xFF;
                d->subclass   = (cls >> 16) & 0xFF;
                d->prog_if    = (cls >>  8) & 0xFF;

                uint32_t hdr = pci_read(bus, dev, fn, 0x0C);
                d->header_type = (hdr >> 16) & 0xFF;

                for (int b = 0; b < 6; b++)
                    d->bar[b] = pci_read(bus, dev, fn, 0x10 + b * 4);

                uint32_t irq = pci_read(bus, dev, fn, 0x3C);
                d->irq_line = irq & 0xFF;

                if (fn == 0 && !(d->header_type & 0x80)) break;
            }
        }
    }

done:
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  [ OK ] PCI enumerated — ");
    vga_putdec(device_count);   // ← FIXED (no u64)
    vga_puts(" device(s) found\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

/* ─── API ─────────────────────────────────────────── */
void pci_init(void)
{
    pci_enumerate();
}

pci_device_t *pci_find_device(uint8_t class_code, uint8_t subclass)
{
    for (int i = 0; i < device_count; i++) {
        if (devices[i].class_code == class_code &&
            devices[i].subclass   == subclass)
            return &devices[i];
    }
    return 0;
}

int pci_get_device_count(void)
{
    return device_count;
}

void pci_dump(void)
{
    for (int i = 0; i < device_count; i++) {
        pci_device_t *d = &devices[i];

        vga_puts("  PCI ");
        vga_putdec(d->bus);
        vga_putchar(':');
        vga_putdec(d->device);
        vga_putchar('.');
        vga_putdec(d->function);

        vga_puts(" Vendor=");
        vga_putdec(d->vendor_id);

        vga_puts(" Class=");
        vga_putdec(d->class_code);
        vga_putchar('.');
        vga_putdec(d->subclass);

        vga_putchar('\n');
    }
}

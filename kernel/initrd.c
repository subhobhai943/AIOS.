#include "initrd.h"
#include "serial.h"
#include "vmm.h"

/* ---- identity-map helper (physical → virtual via PHYS_TO_VIRT) ---- */
#ifndef PHYS_TO_VIRT
#  define PHYS_TO_VIRT(p) ((void *)(uintptr_t)(p))
#endif

/* ---- module-private state ----------------------------------------- */

static initrd_file_t  g_files[INITRD_MAX_FILES];
static uint32_t       g_num_files   = 0;
static bool           g_mounted     = false;

/* ---- tiny no-libc helpers ----------------------------------------- */

static int rd_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void rd_log(const char *s) {
    serial_puts(SERIAL_COM1, s);
}

/* ---- public API ---------------------------------------------------- */

bool initrd_init(uint64_t img_phys, uint32_t img_size) {
    if (img_size < sizeof(initrd_header_t)) {
        rd_log("[initrd] image too small\n");
        return false;
    }

    /* The image was loaded by GRUB into physical RAM inside the first
     * 64 MB identity-mapped window, so PHYS_TO_VIRT is safe here.      */
    const uint8_t *base = (const uint8_t *)PHYS_TO_VIRT(img_phys);

    const initrd_header_t *hdr = (const initrd_header_t *)base;

    if (hdr->magic != INITRD_MAGIC) {
        rd_log("[initrd] bad magic\n");
        return false;
    }
    if (hdr->version != INITRD_VERSION) {
        rd_log("[initrd] unsupported version\n");
        return false;
    }

    uint32_t nf = hdr->num_files;
    if (nf > INITRD_MAX_FILES) {
        rd_log("[initrd] too many files, clamping\n");
        nf = INITRD_MAX_FILES;
    }

    /* Sanity: directory must fit inside the image. */
    uint64_t dir_end = sizeof(initrd_header_t)
                     + (uint64_t)nf * sizeof(initrd_entry_t);
    if (dir_end > img_size) {
        rd_log("[initrd] directory exceeds image size\n");
        return false;
    }

    const initrd_entry_t *entries =
        (const initrd_entry_t *)(base + sizeof(initrd_header_t));

    for (uint32_t i = 0; i < nf; i++) {
        const initrd_entry_t *e = &entries[i];

        /* Validate offset + size stays within image. */
        if ((uint64_t)e->offset + e->size > img_size) {
            rd_log("[initrd] entry exceeds image bounds, skipping\n");
            continue;
        }

        g_files[g_num_files].name = e->name;
        g_files[g_num_files].data = base + e->offset;
        g_files[g_num_files].size = e->size;
        g_num_files++;
    }

    g_mounted = true;
    rd_log("[initrd] mounted: ");
    /* Print file count to serial as decimal */
    char buf[12];
    uint32_t n = g_num_files;
    int pos = 11;
    buf[pos] = '\0';
    if (n == 0) { buf[--pos] = '0'; }
    else { while (n) { buf[--pos] = '0' + (n % 10); n /= 10; } }
    rd_log(buf + pos);
    rd_log(" files\n");
    return true;
}

const initrd_file_t *initrd_find(const char *path) {
    if (!g_mounted) return (void*)0;
    for (uint32_t i = 0; i < g_num_files; i++) {
        if (rd_strcmp(g_files[i].name, path) == 0)
            return &g_files[i];
    }
    return (void*)0;
}

uint32_t initrd_file_count(void) {
    return g_num_files;
}

const initrd_file_t *initrd_get_file(uint32_t index) {
    if (index >= g_num_files) return (void*)0;
    return &g_files[index];
}

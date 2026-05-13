#include "vfs_initrd.h"
#include "../initrd.h"
#include "vfs.h"
#include "../serial.h"

/*
 * This is a thin shim that exposes initrd files through the existing
 * VFS fd interface.  It reuses the VFS fd table; the "data pointer"
 * stored in the fd entry points directly into the ramdisk image
 * (which stays resident in RAM forever — no eviction).
 *
 * Path convention:  /initrd/<filename>
 *   e.g.  /initrd/vocab.bin   →  ramdisk file "/vocab.bin"
 *         /initrd/config.bin  →  ramdisk file "/config.bin"
 *
 * The VFS layer calls vfs_initrd_open internally when the prefix matches.
 * For now we hook into a simple linear search at open time.
 */

#define INITRD_PREFIX "/initrd/"
#define INITRD_PREFIX_LEN 8

/* We need access to VFS internals — use the same fd_slot_t from vfs.c. */
/* Forward-declare the hook that vfs.c calls when it cannot find a FAT32 entry. */

/* -------------------------------------------------------------------- */
/*  Minimal string helpers                                               */
/* -------------------------------------------------------------------- */

static int iv_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static const char *iv_strrchr_slash(const char *s) {
    const char *last = (void*)0;
    while (*s) { if (*s == '/') last = s; s++; }
    return last;
}

/* -------------------------------------------------------------------- */
/*  Public API                                                           */
/* -------------------------------------------------------------------- */

/*
 * vfs_initrd_open — open a file from the ramdisk.
 *
 * @path     : full path, e.g. "/initrd/vocab.bin"
 * @out_data : set to pointer to file bytes on success
 * @out_size : set to file size on success
 *
 * Returns true if the file was found.
 */
bool vfs_initrd_open(const char *path,
                     const uint8_t **out_data,
                     uint32_t       *out_size)
{
    /* Strip the /initrd/ prefix */
    if (iv_strncmp(path, INITRD_PREFIX, INITRD_PREFIX_LEN) != 0)
        return false;

    /* Build the ramdisk-internal path: "/" + rest */
    const char *rest = path + INITRD_PREFIX_LEN;
    char rd_path[INITRD_NAME_MAX];
    rd_path[0] = '/';
    int i = 1;
    while (rest[i-1] && i < INITRD_NAME_MAX - 1) {
        rd_path[i] = rest[i-1];
        i++;
    }
    rd_path[i] = '\0';

    const initrd_file_t *f = initrd_find(rd_path);
    if (!f) {
        serial_puts(SERIAL_COM1, "[vfs_initrd] not found: ");
        serial_puts(SERIAL_COM1, rd_path);
        serial_puts(SERIAL_COM1, "\n");
        return false;
    }

    *out_data = f->data;
    *out_size = f->size;
    return true;
}

bool vfs_initrd_register(void) {
    /* Nothing to register globally in this design —
     * callers that need initrd files use vfs_initrd_open() directly,
     * or the VFS layer calls it as a fallback.  We just print a banner. */
    uint32_t n = initrd_file_count();
    serial_puts(SERIAL_COM1, "[vfs_initrd] registered, ");
    char buf[12];
    int pos = 11;
    buf[pos] = '\0';
    if (n == 0) { buf[--pos] = '0'; }
    else { while (n) { buf[--pos] = '0' + (n % 10); n /= 10; } }
    serial_puts(SERIAL_COM1, buf + pos);
    serial_puts(SERIAL_COM1, " files available under /initrd/\n");

    /* Log all filenames for debugging */
    for (uint32_t i = 0; i < initrd_file_count(); i++) {
        const initrd_file_t *f = initrd_get_file(i);
        serial_puts(SERIAL_COM1, "  [initrd] ");
        serial_puts(SERIAL_COM1, f->name);
        serial_puts(SERIAL_COM1, "\n");
    }
    return true;
}

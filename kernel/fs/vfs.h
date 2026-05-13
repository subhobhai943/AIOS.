#ifndef VFS_H
#define VFS_H

/* ================================================================
 * AIOS — Virtual File System (VFS) Abstraction Layer
 *
 * Thin layer that wraps the FAT32 driver behind a generic
 * file-descriptor API.  Higher-level code (shell, LLM loader)
 * calls vfs_open / vfs_read / vfs_seek / vfs_close and never
 * touches fat32_* directly.
 *
 * Constraints:
 *   - No dynamic allocation; file-descriptor table is a fixed
 *     static array of VFS_MAX_FDS entries.
 *   - Only one mount point supported for now (root "/").
 *   - Read-only.  Write support deferred to FAT32 write phase.
 *   - No libc; only <stdint.h>, <stddef.h>, <stdbool.h>.
 * ================================================================ */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Maximum simultaneously open file descriptors. */
#define VFS_MAX_FDS   8

/* Maximum path length (including NUL). */
#define VFS_MAX_PATH  256

/* ----------------------------------------------------------------
 * vfs_stat_t — basic file metadata
 * ---------------------------------------------------------------- */
typedef struct {
    uint32_t size;         /* file size in bytes          */
    uint32_t first_cluster;/* FAT32 first data cluster    */
    bool     is_dir;       /* true if this is a directory */
} vfs_stat_t;

/* ----------------------------------------------------------------
 * vfs_file_t — open file handle (internal)
 * ---------------------------------------------------------------- */
typedef struct {
    bool     in_use;          /* slot occupied?              */
    uint32_t first_cluster;   /* FAT32 start cluster         */
    uint32_t size;            /* file size in bytes          */
    uint32_t pos;             /* current byte offset         */
} vfs_file_t;

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

/*
 * vfs_init()
 *   Initialise the VFS layer.  Must be called after fat32_init().
 *   The FAT32 root directory is automatically mounted at "/".
 */
void vfs_init(uint32_t root_cluster);

/*
 * vfs_open(path)
 *   Open a file by path (e.g. "/weights.bin" or "TEST.TXT").
 *   Leading '/' is optional.  Uses fat32_find_file_lfn so long
 *   names ("My Weights.bin") work as well as 8.3 names.
 *   Returns a non-negative file descriptor on success, -1 on error.
 */
int  vfs_open(const char *path);

/*
 * vfs_read(fd, buf, count)
 *   Read up to `count` bytes from `fd` into `buf` starting at the
 *   current file position.  Advances the file position.
 *   Returns the number of bytes actually read, 0 at EOF, -1 on error.
 */
int  vfs_read(int fd, void *buf, uint32_t count);

/*
 * vfs_seek(fd, offset)
 *   Set the file position to `offset` bytes from the start.
 *   Returns 0 on success, -1 on error (out-of-range or bad fd).
 */
int  vfs_seek(int fd, uint32_t offset);

/*
 * vfs_tell(fd)
 *   Return current file position, or (uint32_t)-1 on error.
 */
uint32_t vfs_tell(int fd);

/*
 * vfs_close(fd)
 *   Release the file descriptor.
 *   Returns 0 on success, -1 on bad fd.
 */
int  vfs_close(int fd);

/*
 * vfs_stat(path, out)
 *   Fill *out with metadata for the named file without opening it.
 *   Returns 0 on success, -1 if not found.
 */
int  vfs_stat(const char *path, vfs_stat_t *out);

#endif /* VFS_H */

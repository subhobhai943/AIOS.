#ifndef AIOS_APPS_EXPLORER_H
#define AIOS_APPS_EXPLORER_H

/* kernel/apps/explorer.h — Phase 11.2
 *
 * File Explorer — two-pane VFS browser.
 *
 * Layout
 * ──────
 *   • Left pane  : directory tree (just current path components for now)
 *   • Right pane : entries in current directory (dirs first, then files)
 *
 * Interaction
 * ───────────
 *   • Arrow Up/Down : move selection in right pane
 *   • Enter / double-click on directory : descend into that directory
 *   • Backspace     : go up one level
 *   • Enter / double-click on file     : open in Notepad (if text)
 *
 * Freestanding C. No libc. All heap via kmalloc/kfree.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define EXPL_MAX_ENTRIES   128
#define EXPL_MAX_NAME      64
#define EXPL_PATH_MAX      256

typedef struct explorer_entry {
    char     name[EXPL_MAX_NAME];
    bool     is_dir;
    uint32_t size;       /* bytes (files only) */
} explorer_entry_t;

typedef struct explorer {
    char   cwd[EXPL_PATH_MAX];
    explorer_entry_t entries[EXPL_MAX_ENTRIES];
    uint32_t        entry_count;
    int32_t         selected;       /* index into entries, -1 if none */
    int32_t         scroll;         /* first visible row */
    uint32_t        win_id;         /* WM window id */
} explorer_t;

/* Create a new File Explorer window rooted at `start_path` or "/". */
explorer_t *explorer_open(const char *start_path);

/* Destroy explorer, close window and free memory. */
void explorer_close(explorer_t *exp);

#endif /* AIOS_APPS_EXPLORER_H */

#ifndef AIOS_APPS_SETTINGS_H
#define AIOS_APPS_SETTINGS_H

/* kernel/apps/settings.h — Phase 11.4
 *
 * Simple GUI Settings window for AIOS.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct settings_s {
    uint32_t win_id;
} settings_t;

settings_t *settings_open(void);
void        settings_close(settings_t *s);

#endif /* AIOS_APPS_SETTINGS_H */

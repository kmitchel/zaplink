#ifndef TUNE_CONFIG_H
#define TUNE_CONFIG_H
#include <stddef.h>
/* Create a private, single-section config preserving all tuning properties.
 * A NULL service_id selects the first service on the exact multiplex.
 * Caller must unlink path after the tuner child has exited. */
int tune_config_create(const char *source, const char *frequency,
                       const char *service_id, char *path, size_t size);
#define TUNE_SECTION "zaplink-selected"
#endif

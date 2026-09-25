#ifndef SEED0BOOT_BOOTFS_READER_H
#define SEED0BOOT_BOOTFS_READER_H

#include <stdint.h>

/* SEED0_BOOTFS_NO_DIAGNOSTICS removes only stdio logging for freestanding
 * callers. All validation and error returns remain identical; the caller
 * must report/handle failures. */
int seed0_bootfs_open_file(const char *path, const unsigned char **out_data, uint32_t *out_size);

#endif

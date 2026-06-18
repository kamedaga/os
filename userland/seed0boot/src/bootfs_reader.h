#ifndef SEED0BOOT_BOOTFS_READER_H
#define SEED0BOOT_BOOTFS_READER_H

#include <stdint.h>

int seed0_bootfs_open_file(const char *path, const unsigned char **out_data, uint32_t *out_size);

#endif

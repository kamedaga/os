#include <pacha/abi.h>
#pragma once

#include <stdint.h>

int storage_bootfs_read_file(
    pacha_fd_t archive_fd,
    uint64_t archive_size,
    const char *path,
    unsigned char **out_data,
    uint64_t *out_size);

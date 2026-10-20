#include <pacha/abi.h>
/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX2_OWNER_BOOTFS_H
#define PACHA_KOBOX2_OWNER_BOOTFS_H

#include <stddef.h>
#include <stdint.h>

struct ph_bootfs {
    const unsigned char *image;
    size_t size, mapped_size;
    uint32_t entry_count;
    uint64_t entries, strings, string_size, data, data_size;
};

/* Selected files borrow the immutable archive mapping. The archive is never
 * copied or opened through filed, since filed needs it to start storage. */
int ph_bootfs_view(struct ph_bootfs *archive, const void *image, size_t size);
int ph_bootfs_open(struct ph_bootfs *archive, pacha_fd_t fd, uint64_t size);
int ph_bootfs_find(const struct ph_bootfs *archive, const char *path,
                  const unsigned char **bytes, size_t *size);
int ph_bootfs_close(struct ph_bootfs *archive);

#endif

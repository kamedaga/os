/* SPDX-License-Identifier: MIT */
#ifndef PACHA_LPR_BLOCK_CLIENT_H
#define PACHA_LPR_BLOCK_CLIENT_H

#include <stdint.h>

int64_t lpr_block_open_path(const char *path, uint64_t flags);
int64_t lpr_block_io(uint64_t fd, uint64_t buffer, uint64_t count,
    uint64_t offset, int writing);
int64_t lpr_block_flush(uint64_t fd);
int64_t lpr_block_ioctl(uint64_t fd, uint64_t request, uint64_t arg);
int64_t lpr_block_close_state(const void *state);
int64_t lpr_block_size(uint64_t fd);

#endif

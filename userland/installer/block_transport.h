/* SPDX-License-Identifier: MIT */
#ifndef PACHA_INSTALLER_BLOCK_TRANSPORT_H
#define PACHA_INSTALLER_BLOCK_TRANSPORT_H

#include "block_protocol.h"

struct installer_block_source {
    int channel_fd;
    int shared_fd;
    uint64_t resource_id;
    uint64_t generation;
    uint64_t sequence;
    struct installer_block_page *page;
};

struct installer_block_target {
    struct installer_block_source *source;
    uint64_t generation;
    kb2_block_disk_t disk;
};

int installer_block_source_open(struct installer_block_source *source,
    int channel_fd, uint64_t resource_id);
void installer_block_source_close(struct installer_block_source *source);
int installer_block_list(struct installer_block_source *source,
    uint64_t start, kb2_block_disk_t *disks, size_t capacity,
    size_t *count, uint64_t *next);
int installer_block_read(struct installer_block_target *target,
    uint64_t offset, void *bytes, size_t length);
int installer_block_write(struct installer_block_target *target,
    uint64_t offset, const void *bytes, size_t length);
int installer_block_flush(struct installer_block_target *target);
int installer_block_arm(struct installer_block_target *target);
int installer_block_disarm(struct installer_block_target *target);

#endif

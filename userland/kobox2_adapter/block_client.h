/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_BLOCK_CLIENT_H
#define PACHA_KOBOX_BLOCK_CLIENT_H

#include "block_ipc.h"
#include "ipc.h"

/* Single-owner client. The VMO belongs to the caller and is never shared
 * with another Linux sandbox generation. Disk keys come from snapshot and
 * are revalidated by the Linux block port on every operation. */
struct ph_block_client {
    uint8_t *shared;
    int vmo_fd;
    uint64_t generation;
    uint64_t sequence;
};

int ph_block_client_open(struct ph_block_client *client, struct ph_ipc *ipc,
    int process_fd, uint64_t generation);
int ph_block_client_snapshot(struct ph_block_client *client, struct ph_ipc *ipc,
    int process_fd, uint64_t start, kb2_block_disk_t *disks,
    size_t capacity, size_t *count, uint64_t *next);
int ph_block_client_read(struct ph_block_client *client, struct ph_ipc *ipc,
    int process_fd, const kb2_block_disk_t *disk, uint64_t offset,
    void *buffer, size_t length);
int ph_block_client_write(struct ph_block_client *client, struct ph_ipc *ipc,
    int process_fd, const kb2_block_disk_t *disk, uint64_t offset,
    const void *buffer, size_t length);
int ph_block_client_flush(struct ph_block_client *client, struct ph_ipc *ipc,
    int process_fd, const kb2_block_disk_t *disk);
int ph_block_client_close(struct ph_block_client *client);

#endif

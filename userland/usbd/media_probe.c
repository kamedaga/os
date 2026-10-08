/* SPDX-License-Identifier: MIT */
#include "media_probe.h"
#include "../installer/gpt.h"

#include <errno.h>
#include <stdint.h>

struct media_reader {
    struct ph_block_client *block;
    struct ph_ipc *ipc;
    int process_fd;
    const kb2_block_disk_t *disk;
};

static int read_at(void *context, uint64_t offset, void *buffer, size_t bytes) {
    struct media_reader *reader = context;
    if (!reader || !buffer || !reader->disk->logical_block_size ||
        offset % reader->disk->logical_block_size ||
        bytes % reader->disk->logical_block_size) return -EINVAL;
    uint8_t *cursor = buffer;
    while (bytes) {
        size_t chunk = bytes > KB2_BLOCK_DEVICE_MAX_TRANSFER ?
            KB2_BLOCK_DEVICE_MAX_TRANSFER : bytes;
        int result = ph_block_client_read(reader->block, reader->ipc,
            reader->process_fd, reader->disk, offset, cursor, chunk);
        if (result) return result;
        cursor += chunk;
        offset += chunk;
        bytes -= chunk;
    }
    return 0;
}

int usbd_media_probe(struct ph_block_client *block, struct ph_ipc *ipc,
    int process_fd, const kb2_block_disk_t *disk, int *is_install_media) {
    if (!block || !ipc || !disk || !is_install_media) return -EINVAL;
    *is_install_media = 0;
    struct media_reader reader = {.block = block, .ipc = ipc,
        .process_fd = process_fd, .disk = disk};
    struct installer_gpt gpt;
    int result = installer_read_gpt(read_at, &reader, disk->bytes,
        disk->logical_block_size, &gpt);
    if (!result) *is_install_media = gpt.install_media;
    return result;
}

int usbd_media_verify_blank(struct ph_block_client *block, struct ph_ipc *ipc,
    int process_fd, const kb2_block_disk_t *disk) {
    if (!block || !ipc || !disk) return -EINVAL;
    struct media_reader reader = {.block = block, .ipc = ipc,
        .process_fd = process_fd, .disk = disk};
    return installer_verify_blank(read_at, &reader, disk->bytes,
        disk->logical_block_size);
}

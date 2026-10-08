/* SPDX-License-Identifier: MIT */
#include "block_transport.h"

#include <pacha/ipc.h>
#include <errno.h>
#include <string.h>

static int transfer(struct installer_block_source *source,
    enum installer_block_operation operation) {
    if (!source || source->channel_fd < 16 || !source->page ||
        !source->resource_id) return -EINVAL;
    struct installer_block_page *page = source->page;
    page->magic = INSTALLER_BLOCK_MAGIC;
    page->sequence = ++source->sequence;
    page->generation = source->generation;
    page->resource_id = source->resource_id;
    page->operation = operation;
    page->reserved = 0;
    const struct pacha_ipc_msg request = {
        .word0 = operation, .word1 = source->sequence,
        .word2 = source->generation, .word3 = source->resource_id};
    int status = pacha_ipc_send(source->channel_fd, &request);
    if (status) return status;
    struct pacha_ipc_msg reply = {0};
    status = pacha_ipc_recv_wait(source->channel_fd, &reply,
        PACHA_FD_WAIT_FOREVER);
    if (status) return status;
    if (reply.fd_count || reply.word0 != (uint64_t)operation ||
        reply.word1 != source->sequence || !reply.word3 ||
        (int64_t)reply.word2 != page->status) return -EPROTO;
    if (reply.word3 != source->generation) {
        source->generation = reply.word3;
        return -ESTALE;
    }
    return (int)(int64_t)reply.word2;
}

int installer_block_source_open(struct installer_block_source *source,
    int channel_fd, uint64_t resource_id) {
    if (!source || channel_fd < 16 || !resource_id) return -EINVAL;
    *source = (struct installer_block_source){.channel_fd = channel_fd,
        .shared_fd = -1, .resource_id = resource_id};
    const uint64_t rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const int fd = pacha_vmo_create(INSTALLER_BLOCK_PAGE_BYTES, rights, 0);
    if (fd < 16) return fd < 0 ? fd : -EIO;
    source->shared_fd = fd;
    source->page = pacha_mmap(fd, INSTALLER_BLOCK_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!source->page) { installer_block_source_close(source); return -ENOMEM; }
    const uint64_t sequence = ++source->sequence;
    struct pacha_ipc_fd attached = {.fd = fd, .rights = rights};
    const struct pacha_ipc_msg request = {
        .word0 = INSTALLER_BLOCK_ATTACH, .word1 = sequence,
        .word3 = resource_id, .fds = &attached, .fd_count = 1};
    int status = pacha_ipc_send(channel_fd, &request);
    if (!status) {
        struct pacha_ipc_msg reply = {0};
        status = pacha_ipc_recv_wait(channel_fd, &reply,
            PACHA_FD_WAIT_FOREVER);
        if (!status && (reply.fd_count ||
            reply.word0 != INSTALLER_BLOCK_ATTACH ||
            reply.word1 != sequence || !reply.word3)) status = -EPROTO;
        if (!status) status = (int)(int64_t)reply.word2;
        if (!status) source->generation = reply.word3;
    }
    if (status) installer_block_source_close(source);
    return status;
}

void installer_block_source_close(struct installer_block_source *source) {
    if (!source) return;
    if (source->page)
        (void)pacha_munmap(source->page, INSTALLER_BLOCK_PAGE_BYTES);
    if (source->shared_fd >= 16) (void)pacha_fd_close(source->shared_fd);
    *source = (struct installer_block_source){.channel_fd = -1,
        .shared_fd = -1};
}

int installer_block_list(struct installer_block_source *source,
    uint64_t start, kb2_block_disk_t *disks, size_t capacity,
    size_t *count, uint64_t *next) {
    if (!source || !disks || !count || !next ||
        !capacity || capacity > KB2_BLOCK_DEVICE_MAX_DISKS) return -EINVAL;
    struct installer_block_page *page = source->page;
    if (!page) return -EINVAL;
    page->offset = start;
    page->length = 0;
    page->count = (uint32_t)capacity;
    int status = transfer(source, INSTALLER_BLOCK_LIST);
    if (status) return status;
    if (page->count > capacity) return -EPROTO;
    memcpy(disks, page->disks, page->count * sizeof(*disks));
    *count = page->count;
    *next = page->next;
    return 0;
}

static int target_call(struct installer_block_target *target,
    enum installer_block_operation operation, uint64_t offset,
    void *bytes, size_t length) {
    if (!target || !target->source || !target->source->page ||
        target->generation != target->source->generation ||
        length > KB2_BLOCK_DEVICE_MAX_TRANSFER ||
        (length && !bytes)) return -ESTALE;
    struct installer_block_page *page = target->source->page;
    page->key = target->disk;
    page->offset = offset;
    page->length = (uint32_t)length;
    if (operation == INSTALLER_BLOCK_WRITE && length)
        memcpy(page->data, bytes, length);
    int status = transfer(target->source, operation);
    if (!status && operation == INSTALLER_BLOCK_READ && length)
        memcpy(bytes, page->data, length);
    return status;
}

int installer_block_read(struct installer_block_target *target,
    uint64_t offset, void *bytes, size_t length) {
    return target_call(target, INSTALLER_BLOCK_READ, offset, bytes, length);
}
int installer_block_write(struct installer_block_target *target,
    uint64_t offset, const void *bytes, size_t length) {
    return target_call(target, INSTALLER_BLOCK_WRITE, offset,
        (void *)bytes, length);
}
int installer_block_flush(struct installer_block_target *target) {
    return target_call(target, INSTALLER_BLOCK_FLUSH, 0, NULL, 0);
}
int installer_block_arm(struct installer_block_target *target) {
    return target_call(target, INSTALLER_BLOCK_ARM, 0, NULL, 0);
}
int installer_block_disarm(struct installer_block_target *target) {
    return target_call(target, INSTALLER_BLOCK_DISARM, 0, NULL, 0);
}

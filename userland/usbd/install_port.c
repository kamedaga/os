/* SPDX-License-Identifier: MIT */
#include "install_port.h"
#include "media_probe.h"

#include <pacha/ipc.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int same_disk(const kb2_block_disk_t *left,
    const kb2_block_disk_t *right) {
    return left->diskseq == right->diskseq &&
        left->major == right->major && left->minor == right->minor &&
        left->bytes == right->bytes &&
        left->logical_block_size == right->logical_block_size &&
        !memcmp(left->name, right->name, sizeof(left->name));
}

static int find_disk(struct ph_block_client *block, struct ph_ipc *ipc,
    int process_fd, const kb2_block_disk_t *requested,
    kb2_block_disk_t *fresh) {
    uint64_t next = 0;
    do {
        kb2_block_disk_t snapshot[KB2_BLOCK_DEVICE_MAX_DISKS];
        size_t count = 0;
        int result = ph_block_client_snapshot(block, ipc, process_fd, next,
            snapshot, KB2_BLOCK_DEVICE_MAX_DISKS, &count, &next);
        if (result) return result;
        for (size_t i = 0; i < count; ++i)
            if (same_disk(&snapshot[i], requested)) {
                *fresh = snapshot[i];
                return 0;
            }
    } while (next);
    return -ESTALE;
}

static int attach(struct usbd_install_port *port,
    const struct pacha_ipc_msg *request) {
    if (port->shared || request->fd_count != 1 ||
        request->fds[0].fd < 16) return -EPROTO;
    const int fd = (int)request->fds[0].fd;
    struct pacha_fd_info info = {0};
    if (pacha_fd_get_info(fd, &info) ||
        info.kind != PACHA_FD_KIND_VMO ||
        info.size < INSTALLER_BLOCK_PAGE_BYTES ||
        (info.rights & (PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE)) !=
            (PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE))
        return -EPROTO;
    void *mapped = pacha_mmap(fd, INSTALLER_BLOCK_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!mapped) return -ENOMEM;
    port->shared = mapped;
    port->shared_fd = fd;
    return 0;
}

static int dispatch(struct usbd_install_port *port,
    struct ph_block_client *block, struct ph_ipc *ipc, int process_fd,
    uint64_t resource_id, kb2_block_disk_t *write_disk,
    int *restart_requested) {
    struct installer_block_page *page = port->shared;
    if (!page || page->resource_id != resource_id ||
        page->generation != ipc->generation || page->reserved ||
        page->length > KB2_BLOCK_DEVICE_MAX_TRANSFER) return -EPROTO;
    if (page->operation == INSTALLER_BLOCK_LIST) {
        if (!page->count || page->count > KB2_BLOCK_DEVICE_MAX_DISKS ||
            page->length) return -EINVAL;
        size_t count = 0;
        uint64_t next = 0;
        int status = ph_block_client_snapshot(block, ipc, process_fd,
            page->offset, page->disks, page->count, &count, &next);
        if (!status) { page->count = (uint32_t)count; page->next = next; }
        return status;
    }
    if (page->operation == INSTALLER_BLOCK_DISARM) {
        /* Revocation must work even after unplug: the old key is only a
         * guard against a different installer request, never a prerequisite
         * for removing write authority. */
        if (!write_disk->diskseq || !same_disk(write_disk, &page->key) ||
            page->length || page->offset) return -EACCES;
        *write_disk = (kb2_block_disk_t){0};
        *restart_requested = 1;
        return 0;
    }
    kb2_block_disk_t disk = {0};
    int status = find_disk(block, ipc, process_fd, &page->key, &disk);
    if (status) return status;
    if (page->operation == INSTALLER_BLOCK_READ)
        return ph_block_client_read(block, ipc, process_fd, &disk,
            page->offset, page->data, page->length);
    if (page->operation == INSTALLER_BLOCK_WRITE) {
        if (!write_disk->diskseq || !same_disk(write_disk, &disk))
            return -EACCES;
        return ph_block_client_write(block, ipc, process_fd, &disk,
            page->offset, page->data, page->length);
    }
    if (page->operation == INSTALLER_BLOCK_FLUSH) {
        if (!write_disk->diskseq || !same_disk(write_disk, &disk))
            return -EACCES;
        status = ph_block_client_flush(block, ipc, process_fd, &disk);
        if (status)
            printf("[usbd] installer flush failed disk=%s diskseq=%llu status=%d\n",
                disk.name, (unsigned long long)disk.diskseq, status);
        return status;
    }
    if (page->operation == INSTALLER_BLOCK_ARM) {
        if (write_disk->diskseq || disk.read_only || page->length ||
            page->offset) return -EACCES;
        int install_media = 0;
        status = usbd_media_probe(block, ipc, process_fd, &disk,
            &install_media);
        if (install_media) return -EACCES;
        if (status == -EINVAL)
            status = usbd_media_verify_blank(block, ipc, process_fd, &disk);
        if (status) return status;
        *write_disk = disk;
        *restart_requested = 1;
        return 0;
    }
    return -EOPNOTSUPP;
}

int usbd_install_port_pump(struct usbd_install_port *port,
    struct ph_block_client *block, struct ph_ipc *ipc, int process_fd,
    uint64_t resource_id, kb2_block_disk_t *write_disk,
    int *restart_requested) {
    if (!port || port->fd < 16 || !block || !ipc || process_fd < 16 ||
        !resource_id || !write_disk || !restart_requested) return -EINVAL;
    struct pacha_ipc_fd received_fd[1] = {0};
    struct pacha_ipc_msg request = {.fds = received_fd, .fd_capacity = 1};
    int status = pacha_ipc_recv(port->fd, &request);
    if (status) return status;
    const uint64_t sequence = request.word1;
    const uint32_t operation = (uint32_t)request.word0;
    if (request.word0 != operation || !sequence ||
        sequence <= port->sequence ||
        (operation != INSTALLER_BLOCK_ATTACH &&
            request.word2 != ipc->generation) ||
        request.word3 != resource_id)
        status = -EPROTO;
    else if (operation == INSTALLER_BLOCK_ATTACH)
        status = attach(port, &request);
    else if (request.fd_count)
        status = -EPROTO;
    else if (!port->shared || port->shared->magic != INSTALLER_BLOCK_MAGIC ||
             port->shared->sequence != sequence ||
             port->shared->operation != operation)
        status = -EPROTO;
    else
        status = dispatch(port, block, ipc, process_fd, resource_id,
            write_disk, restart_requested);
    if (operation != INSTALLER_BLOCK_ATTACH && request.fd_count)
        (void)pacha_fd_close((int)request.fds[0].fd);
    if (operation == INSTALLER_BLOCK_ATTACH && status && request.fd_count)
        (void)pacha_fd_close((int)request.fds[0].fd);
    port->sequence = sequence > port->sequence ? sequence : port->sequence;
    if (port->shared) port->shared->status = status;
    const struct pacha_ipc_msg reply = {
        .word0 = operation, .word1 = sequence,
        .word2 = (uint64_t)(int64_t)status,
        .word3 = ipc->generation};
    return pacha_ipc_send(port->fd, &reply);
}

void usbd_install_port_close(struct usbd_install_port *port) {
    if (!port) return;
    if (port->shared)
        (void)pacha_munmap(port->shared, INSTALLER_BLOCK_PAGE_BYTES);
    if (port->shared_fd >= 16) (void)pacha_fd_close(port->shared_fd);
    if (port->fd >= 16) (void)pacha_fd_close(port->fd);
    *port = (struct usbd_install_port){.fd = -1, .shared_fd = -1};
}

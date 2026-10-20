/* SPDX-License-Identifier: MIT */
#include "disk_port.h"

#include <pacha/ipc.h>
#include <errno.h>
#include <string.h>

static size_t minimum(size_t a, size_t b) { return a < b ? a : b; }

static int read_bytes(struct installer_block_target *target,
    uint64_t offset, uint8_t *bytes, size_t count) {
    const size_t block = target->disk.logical_block_size;
    uint8_t sector[4096];
    size_t done = 0;
    while (done < count) {
        uint64_t pos = offset + done;
        if (pos % block || count - done < block) {
            const uint64_t base = pos - pos % block;
            const size_t within = (size_t)(pos - base);
            const size_t take = minimum(count - done, block - within);
            int status = installer_block_read(target, base, sector, block);
            if (status) return status;
            memcpy(bytes + done, sector + within, take);
            done += take;
        } else {
            size_t take = minimum(count - done,
                KB2_BLOCK_DEVICE_MAX_TRANSFER);
            take -= take % block;
            int status = installer_block_read(target, pos,
                bytes + done, take);
            if (status) return status;
            done += take;
        }
    }
    return 0;
}

static int write_bytes(struct installer_block_target *target,
    uint64_t offset, const uint8_t *bytes, size_t count) {
    const size_t block = target->disk.logical_block_size;
    uint8_t sector[4096];
    size_t done = 0;
    while (done < count) {
        uint64_t pos = offset + done;
        if (pos % block || count - done < block) {
            const uint64_t base = pos - pos % block;
            const size_t within = (size_t)(pos - base);
            const size_t take = minimum(count - done, block - within);
            int status = installer_block_read(target, base, sector, block);
            if (status) return status;
            memcpy(sector + within, bytes + done, take);
            status = installer_block_write(target, base, sector, block);
            if (status) return status;
            done += take;
        } else {
            size_t take = minimum(count - done,
                KB2_BLOCK_DEVICE_MAX_TRANSFER);
            take -= take % block;
            int status = installer_block_write(target, pos,
                bytes + done, take);
            if (status) return status;
            done += take;
        }
    }
    return 0;
}

int installer_disk_port_dispatch(struct installer_disk_port *port,
    struct installer_disk_port_page *page) {
    if (!port || !port->target || !page ||
        page->magic != INSTALLER_DISK_PORT_MAGIC ||
        page->length > sizeof(page->data)) return -EPROTO;
    struct installer_block_target *target = port->target;
    const uint64_t disk_bytes = target->disk.bytes;
    const uint32_t block = target->disk.logical_block_size;
    const uint64_t view_first = port->visible_first;
    const uint64_t view_end = port->visible_end ?
        port->visible_end : disk_bytes;
    const uint64_t bytes = view_end >= view_first ?
        view_end - view_first : 0;
    if (block < 512 || block > 4096 || (block & (block - 1)) ||
        !disk_bytes || disk_bytes % block || !bytes || bytes % block ||
        view_first % block || view_end % block || view_end > disk_bytes ||
        port->writable_first > port->writable_end ||
        port->writable_end > bytes ||
        port->writable_first % block || port->writable_end % block)
        return -EPROTO;
    page->transferred = 0;
    if (page->operation == INSTALLER_DISK_PORT_INFO) {
        if (page->length || page->offset) return -EINVAL;
        page->disk_bytes = bytes;
        page->logical_block = block;
        page->writable_first = port->writable_first;
        page->writable_end = port->writable_end;
        return 0;
    }
    if (page->operation == INSTALLER_DISK_PORT_FLUSH) {
        if (page->length || page->offset) return -EINVAL;
        if (port->writable_first == port->writable_end) return 0;
        return installer_block_flush(target);
    }
    if (page->operation == INSTALLER_DISK_PORT_PROGRESS) {
        if (!port->allow_progress || page->offset ||
            page->length != sizeof(struct installer_progress_wire))
            return -EACCES;
        const struct installer_progress_wire *progress =
            (const void *)page->data;
        if (progress->magic != INSTALLER_PROGRESS_MAGIC ||
            progress->reserved ||
            progress->phase < INSTALLER_PROGRESS_PREFLIGHT_FILES ||
            progress->phase > INSTALLER_PROGRESS_COMPLETE ||
            (progress->total_bytes &&
             progress->completed_bytes > progress->total_bytes) ||
            progress->total_bytes > disk_bytes)
            return -EINVAL;
        return 0;
    }
    if (page->operation != INSTALLER_DISK_PORT_READ &&
        page->operation != INSTALLER_DISK_PORT_WRITE) return -EOPNOTSUPP;
    if (page->offset > bytes) return -EINVAL;
    const uint32_t count = (uint32_t)minimum(page->length,
        (size_t)(bytes - page->offset));
    if (page->operation == INSTALLER_DISK_PORT_WRITE &&
        (page->length != count ||
         page->offset < port->writable_first ||
         page->offset > port->writable_end ||
         count > port->writable_end - page->offset)) return -EACCES;
    int status = page->operation == INSTALLER_DISK_PORT_READ ?
        read_bytes(target, view_first + page->offset, page->data, count) :
        write_bytes(target, view_first + page->offset, page->data, count);
    if (!status) page->transferred = count;
    return status;
}

int installer_disk_port_pump(struct installer_disk_port *port) {
    if (!port || port->fd < 16 || !port->target) return -EINVAL;
    port->completed_status = -EAGAIN;
    port->completed_operation = 0;
    port->completed_transferred = 0;
    memset(&port->completed_progress, 0,
        sizeof(port->completed_progress));
    struct pacha_ipc_fd received[1] = {0};
    struct pacha_ipc_msg request = {.fds = received, .fd_capacity = 1};
    int status = pacha_ipc_recv(port->fd, &request);
    if (status) return status;
    const uint64_t sequence = request.word1;
    const uint32_t operation = (uint32_t)request.word0;
    if (!sequence ||
        (operation != INSTALLER_DISK_PORT_ATTACH &&
         sequence <= port->sequence) ||
        request.word0 != operation || request.word2 || request.word3)
        status = -EPROTO;
    else if (operation == INSTALLER_DISK_PORT_ATTACH) {
        if (request.fd_count != 1 || request.fds[0].fd < 16)
            status = -EPROTO;
        else {
            const pacha_fd_t fd = (pacha_fd_t)request.fds[0].fd;
            struct pacha_fd_info info = {0};
            if (pacha_fd_get_info(fd, &info) ||
                info.kind != PACHA_FD_KIND_VMO ||
                info.size < INSTALLER_DISK_PORT_BYTES ||
                (info.rights & (PACHA_FD_RIGHT_MAP_READ |
                    PACHA_FD_RIGHT_MAP_WRITE)) !=
                    (PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE))
                status = -EPROTO;
            else {
                struct installer_disk_port_page *page = pacha_mmap(fd,
                    INSTALLER_DISK_PORT_BYTES,
                    PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
                if (!page) status = -ENOMEM;
                else {
                    /* The bounded port belongs to an ash process tree, while
                     * each unmodified filesystem utility has its own LPR
                     * runtime and transport VMO.  Replace the previous
                     * transport page at the serialized ATTACH boundary; disk
                     * identity and writable extents remain immutable. */
                    if (port->page)
                        (void)pacha_munmap(port->page,
                            INSTALLER_DISK_PORT_BYTES);
                    if (port->page_fd >= 16)
                        (void)pacha_fd_close(port->page_fd);
                    port->page = page;
                    port->page_fd = fd;
                }
            }
        }
    } else if (request.fd_count || !port->page ||
        port->page->sequence != sequence ||
        port->page->operation != operation)
        status = -EPROTO;
    else status = installer_disk_port_dispatch(port, port->page);
    if (request.fd_count &&
        (operation != INSTALLER_DISK_PORT_ATTACH || status))
        (void)pacha_fd_close((pacha_fd_t)request.fds[0].fd);
    if (operation != INSTALLER_DISK_PORT_ATTACH && port->page) {
        port->completed_status = status;
        port->completed_operation = operation;
        port->completed_transferred = port->page->transferred;
        if (operation == INSTALLER_DISK_PORT_PROGRESS &&
            port->page->length == sizeof(port->completed_progress))
            memcpy(&port->completed_progress, port->page->data,
                sizeof(port->completed_progress));
    }
    if (operation == INSTALLER_DISK_PORT_ATTACH ||
        sequence > port->sequence) port->sequence = sequence;
    if (port->page) port->page->status = status;
    const struct pacha_ipc_msg reply = {.word0 = operation,
        .word1 = sequence, .word2 = (uint64_t)(int64_t)status};
    return pacha_ipc_send(port->fd, &reply);
}

void installer_disk_port_close(struct installer_disk_port *port) {
    if (!port) return;
    if (port->page) (void)pacha_munmap(port->page,
        INSTALLER_DISK_PORT_BYTES);
    if (port->page_fd >= 16) (void)pacha_fd_close(port->page_fd);
    if (port->fd >= 16) (void)pacha_fd_close(port->fd);
    *port = (struct installer_disk_port){.fd = -1, .page_fd = -1};
}

#include "filed/kobox_backend.h"

#include <stdio.h>
#include <string.h>

#include "filed/fd_ipc.h"
#include "pacha/abi.h"

static int filed_kobox_call_with_fd(
    filed_kobox_backend_t *backend,
    uint64_t op,
    uint64_t object_id,
    int transfer_fd,
    uint64_t *out_result)
{
    if (backend == NULL || backend->fs_fd < 16 || out_result == NULL) {
        return -1;
    }

    struct pacha_ipc_fd fd_item;
    memset(&fd_item, 0, sizeof(fd_item));
    if (transfer_fd >= 16) {
        fd_item.fd = (uint64_t)(uint32_t)transfer_fd;
        fd_item.rights =
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE;
        fd_item.flags = 0;
        fd_item.transfer_flags = 0;
    }

    const uint64_t request_id = ++backend->calls;
    const struct pacha_ipc_msg request = {
        .word0 = KOBOXD_WIRE_ENDPOINT_MAGIC,
        .word1 = op,
        .word2 = object_id,
        .word3 = request_id,
        .fds = transfer_fd >= 16 ? &fd_item : NULL,
        .fd_count = transfer_fd >= 16 ? 1 : 0,
    };

    int status = filed_ipc_send_wait(backend->fs_fd, &request);
    if (status != 0) {
        return status;
    }

    for (unsigned int attempt = 0; attempt < 128; ++attempt) {
        struct pacha_ipc_msg reply;
        memset(&reply, 0, sizeof(reply));

        status = filed_ipc_recv_wait(backend->fs_fd, &reply);
        if (status != 0) {
            return status;
        }
        if (reply.word0 == KOBOXD_WIRE_REPLY_MAGIC &&
            reply.word3 == request_id)
        {
            if ((int64_t)reply.word1 < 0) {
                return (int)(int64_t)reply.word1;
            }
            *out_result = reply.word2;
            return 0;
        }
    }

    return -2;
}

void filed_kobox_backend_init(filed_kobox_backend_t *backend, int fs_fd)
{
    if (backend == NULL) {
        return;
    }

    memset(backend, 0, sizeof(*backend));
    backend->fs_fd = fs_fd;
    backend->root_object_id = KOBOXD_WIRE_FS_ROOT_OBJECT_ID;
}

int filed_kobox_backend_mount_root(filed_kobox_backend_t *backend)
{
    uint64_t magic = 0;

    if (backend == NULL) {
        return -1;
    }

    const int status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_MOUNT_ROOT,
        0,
        -1,
        &magic);
    if (status != 0) {
        return status;
    }

    backend->ext4_magic = magic;
    return magic == 0xef53u ? 0 : -3;
}

int filed_kobox_backend_lookup(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint64_t *out_object_id)
{
    filed_wire_page_t page;
    uint64_t object_id = 0;

    if (backend == NULL || parent_object_id == 0 || name == NULL || out_object_id == NULL) {
        return -1;
    }

    int status = filed_ipc_create_wire_page(KOBOXD_WIRE_FS_PAGE_BYTES, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_lookup_t *lookup = (koboxd_wire_fs_lookup_t *)page.addr;
    lookup->parent_object_id = parent_object_id;
    snprintf(lookup->name, sizeof(lookup->name), "%s", name);

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_LOOKUP,
        0,
        page.fd,
        &object_id);
    filed_ipc_destroy_wire_page(&page);
    if (status != 0) {
        return status;
    }

    *out_object_id = object_id;
    return 0;
}

int filed_kobox_backend_statx(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    koboxd_wire_fs_statx_t *out_stat)
{
    filed_wire_page_t page;
    uint64_t ignored = 0;

    if (backend == NULL || object_id == 0 || out_stat == NULL) {
        return -1;
    }

    int status = filed_ipc_create_wire_page(KOBOXD_WIRE_FS_PAGE_BYTES, &page);
    if (status != 0) {
        return status;
    }

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_STATX,
        object_id,
        page.fd,
        &ignored);
    if (status == 0) {
        *out_stat = *(koboxd_wire_fs_statx_t *)page.addr;
    }
    filed_ipc_destroy_wire_page(&page);
    return status;
}

int filed_kobox_backend_pread(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    uint64_t offset,
    void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    filed_wire_page_t page;
    uint64_t bytes = 0;

    if (backend == NULL || object_id == 0 || buffer == NULL || out_bytes == NULL) {
        return -1;
    }
    *out_bytes = 0;

    int status = filed_ipc_create_wire_page(KOBOXD_WIRE_FS_PAGE_BYTES, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_io_t *io = (koboxd_wire_fs_io_t *)page.addr;
    io->object_id = object_id;
    io->offset = offset;
    io->length = length > KOBOXD_WIRE_FS_IO_BYTES ? KOBOXD_WIRE_FS_IO_BYTES : length;

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_PREAD,
        0,
        page.fd,
        &bytes);
    if (status == 0 && bytes != 0) {
        if (bytes > length) {
            bytes = length;
        }
        memcpy(buffer, io->data, (size_t)bytes);
        backend->bytes_read += bytes;
        *out_bytes = bytes;
    }

    filed_ipc_destroy_wire_page(&page);
    return status;
}

int filed_kobox_backend_getdents(
    filed_kobox_backend_t *backend,
    uint64_t dir_object_id,
    uint64_t offset,
    koboxd_wire_fs_getdents_t *out_entries)
{
    filed_wire_page_t page;
    uint64_t ignored = 0;

    if (backend == NULL || dir_object_id == 0 || out_entries == NULL) {
        return -1;
    }

    int status = filed_ipc_create_wire_page(KOBOXD_WIRE_FS_PAGE_BYTES, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_getdents_t *request = (koboxd_wire_fs_getdents_t *)page.addr;
    request->dir_object_id = dir_object_id;
    request->offset = offset;
    request->capacity = KOBOXD_WIRE_FS_DIRENT_CAPACITY;

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_GETDENTS,
        0,
        page.fd,
        &ignored);
    if (status == 0) {
        *out_entries = *request;
    }

    filed_ipc_destroy_wire_page(&page);
    return status;
}

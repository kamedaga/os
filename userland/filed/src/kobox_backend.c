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

int filed_kobox_backend_pwrite(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    uint64_t offset,
    const void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    filed_wire_page_t page;
    uint64_t bytes = 0;

    if (backend == NULL || object_id == 0 || out_bytes == NULL) {
        return -1;
    }
    if (buffer == NULL && length != 0) {
        return -1;
    }
    *out_bytes = 0;
    if (length == 0) {
        return 0;
    }

    int status = filed_ipc_create_wire_page(KOBOXD_WIRE_FS_PAGE_BYTES, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_io_t *io = (koboxd_wire_fs_io_t *)page.addr;
    io->object_id = object_id;
    io->offset = offset;
    io->length = length > KOBOXD_WIRE_FS_IO_BYTES ? KOBOXD_WIRE_FS_IO_BYTES : length;
    memcpy(io->data, buffer, (size_t)io->length);

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_PWRITE,
        0,
        page.fd,
        &bytes);
    if (status == 0) {
        if (bytes > length) {
            bytes = length;
        }
        backend->bytes_written += bytes;
        *out_bytes = bytes;
    }

    filed_ipc_destroy_wire_page(&page);
    return status;
}

int filed_kobox_backend_fsync(
    filed_kobox_backend_t *backend,
    uint64_t object_id)
{
    uint64_t ignored = 0;

    if (backend == NULL || object_id == 0) {
        return -1;
    }

    return filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_FSYNC,
        object_id,
        -1,
        &ignored);
}

int filed_kobox_backend_create(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint64_t mode,
    uint64_t *out_object_id)
{
    filed_wire_page_t page;
    uint64_t object_id = 0;

    if (backend == NULL || parent_object_id == 0 || name == NULL || out_object_id == NULL) {
        return -1;
    }
    *out_object_id = 0;

    int status = filed_ipc_create_wire_page(KOBOXD_WIRE_FS_PAGE_BYTES, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_create_t *create = (koboxd_wire_fs_create_t *)page.addr;
    create->parent_object_id = parent_object_id;
    create->mode = mode;
    snprintf(create->name, sizeof(create->name), "%s", name);

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_CREATE,
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

int filed_kobox_backend_truncate(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    uint64_t size)
{
    filed_wire_page_t page;
    uint64_t ignored = 0;

    if (backend == NULL || object_id == 0) {
        return -1;
    }

    int status = filed_ipc_create_wire_page(KOBOXD_WIRE_FS_PAGE_BYTES, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_truncate_t *truncate = (koboxd_wire_fs_truncate_t *)page.addr;
    truncate->object_id = object_id;
    truncate->size = size;

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_TRUNCATE,
        0,
        page.fd,
        &ignored);
    filed_ipc_destroy_wire_page(&page);
    return status;
}

int filed_kobox_backend_unlink(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name)
{
    filed_wire_page_t page;
    uint64_t ignored = 0;

    if (backend == NULL || parent_object_id == 0 || name == NULL) {
        return -1;
    }

    int status = filed_ipc_create_wire_page(KOBOXD_WIRE_FS_PAGE_BYTES, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_unlink_t *unlink = (koboxd_wire_fs_unlink_t *)page.addr;
    unlink->parent_object_id = parent_object_id;
    snprintf(unlink->name, sizeof(unlink->name), "%s", name);

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_UNLINK,
        0,
        page.fd,
        &ignored);
    filed_ipc_destroy_wire_page(&page);
    return status;
}

int filed_kobox_backend_mkdir(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint64_t mode,
    uint64_t *out_object_id)
{
    filed_wire_page_t page;
    uint64_t object_id = 0;

    if (backend == NULL || parent_object_id == 0 || name == NULL || out_object_id == NULL) {
        return -1;
    }
    *out_object_id = 0;

    int status = filed_ipc_create_wire_page(KOBOXD_WIRE_FS_PAGE_BYTES, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_mkdir_t *mkdir = (koboxd_wire_fs_mkdir_t *)page.addr;
    mkdir->parent_object_id = parent_object_id;
    mkdir->mode = mode;
    snprintf(mkdir->name, sizeof(mkdir->name), "%s", name);

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_MKDIR,
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

int filed_kobox_backend_rmdir(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name)
{
    filed_wire_page_t page;
    uint64_t ignored = 0;

    if (backend == NULL || parent_object_id == 0 || name == NULL) {
        return -1;
    }

    int status = filed_ipc_create_wire_page(KOBOXD_WIRE_FS_PAGE_BYTES, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_rmdir_t *rmdir = (koboxd_wire_fs_rmdir_t *)page.addr;
    rmdir->parent_object_id = parent_object_id;
    snprintf(rmdir->name, sizeof(rmdir->name), "%s", name);

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_RMDIR,
        0,
        page.fd,
        &ignored);
    filed_ipc_destroy_wire_page(&page);
    return status;
}

int filed_kobox_backend_rename(
    filed_kobox_backend_t *backend,
    uint64_t old_parent_object_id,
    const char *old_name,
    uint64_t new_parent_object_id,
    const char *new_name,
    uint64_t *out_object_id)
{
    filed_wire_page_t page;
    uint64_t object_id = 0;

    if (backend == NULL ||
        old_parent_object_id == 0 ||
        old_name == NULL ||
        new_parent_object_id == 0 ||
        new_name == NULL ||
        out_object_id == NULL)
    {
        return -1;
    }
    *out_object_id = 0;

    int status = filed_ipc_create_wire_page(KOBOXD_WIRE_FS_PAGE_BYTES, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_rename_t *rename = (koboxd_wire_fs_rename_t *)page.addr;
    rename->old_parent_object_id = old_parent_object_id;
    rename->new_parent_object_id = new_parent_object_id;
    snprintf(rename->old_name, sizeof(rename->old_name), "%s", old_name);
    snprintf(rename->new_name, sizeof(rename->new_name), "%s", new_name);

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_RENAME,
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

int filed_kobox_backend_release_object(
    filed_kobox_backend_t *backend,
    uint64_t object_id)
{
    uint64_t ignored = 0;

    if (backend == NULL || object_id == 0) {
        return -1;
    }

    return filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_RELEASE_OBJECT,
        object_id,
        -1,
        &ignored);
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

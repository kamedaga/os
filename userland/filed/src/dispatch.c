#include "filed/dispatch.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "filed/fd_ipc.h"
#include "filed/exec.h"
#include "filed/ipc_protocol.h"
#include "pacha/abi.h"
#include "pacha/ipc.h"
#include "pacha/syscall.h"

enum {
    FILED_BOOTSTRAP_PATCH_BYTES = 4096,
};

static int filed_send_reply(int reply_fd, uint64_t request_id, int64_t status, uint64_t result)
{
    const struct pacha_ipc_msg reply = {
        .word0 = FILED_WIRE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = result,
        .word3 = request_id,
    };
    const int reply_status = pacha_ipc_reply(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    return reply_status;
}

static int filed_send_exec_reply(
    int reply_fd,
    uint64_t request_id,
    int process_fd,
    int thread_fd)
{
    struct pacha_ipc_fd fds[2] = {
        {
            .fd = (uint64_t)(uint32_t)process_fd,
            .rights =
                PACHA_FD_RIGHT_INSPECT |
                PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_WAIT |
                PACHA_FD_RIGHT_POLL |
                PACHA_FD_RIGHT_KILL,
            .flags = 0,
            .transfer_flags = PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_CLOEXEC,
        },
        {
            .fd = (uint64_t)(uint32_t)thread_fd,
            .rights =
                PACHA_FD_RIGHT_INSPECT |
                PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_WAIT |
                PACHA_FD_RIGHT_KILL,
            .flags = 0,
            .transfer_flags = PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_CLOEXEC,
        },
    };
    struct pacha_ipc_msg reply = {
        .word0 = FILED_WIRE_REPLY_MAGIC,
        .word1 = 0,
        .word2 = (uint64_t)(uint32_t)process_fd,
        .word3 = request_id,
        .fds = fds,
        .fd_count = 2,
    };
    const int status = pacha_ipc_reply(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    if (status != 0) {
        (void)pacha_syscall2(
            PACHA_PROCESS_SYSCALL_KILL,
            (uint64_t)(uint32_t)process_fd,
            1);
        (void)pacha_fd_close(thread_fd);
        (void)pacha_fd_close(process_fd);
    }
    return status;
}

static int64_t filed_status_to_wire(filed_status_t status)
{
    switch (status) {
    case FILED_OK:
        return 0;
    case FILED_ERR_NOT_FOUND:
        return -2;
    case FILED_ERR_NOT_DIR:
        return -20;
    case FILED_ERR_IS_DIR:
        return -21;
    case FILED_ERR_EXISTS:
        return -17;
    case FILED_ERR_DENIED:
        return -13;
    case FILED_ERR_INVALID:
        return -22;
    case FILED_ERR_CROSS_MOUNT:
        return -18;
    case FILED_ERR_NOT_EMPTY:
        return -39;
    case FILED_ERR_IO:
        return -5;
    case FILED_ERR_UNSUPPORTED:
        return -95;
    case FILED_ERR_BAD_FORMAT:
    case FILED_ERR_INVALID_IMAGE:
        return -8;
    case FILED_ERR_LOOP:
        return -40;
    case FILED_ERR_OVERFLOW:
        return -75;
    case FILED_ERR_FULL:
        return -28;
    }
    return -22;
}

static void filed_write_u64_le(void *base, uint64_t offset, uint64_t value)
{
    unsigned char *p = (unsigned char *)base + offset;
    for (unsigned int i = 0; i < 8; ++i) {
        p[i] = (unsigned char)(value >> (i * 8u));
    }
}

static filed_vnode_kind_t filed_kind_from_unix_type(uint64_t kind)
{
    switch (kind & 0170000u) {
    case 0040000u:
        return FILED_VNODE_DIRECTORY;
    case 0100000u:
        return FILED_VNODE_REGULAR;
    case 0120000u:
        return FILED_VNODE_SYMLINK;
    case 0010000u:
        return FILED_VNODE_FIFO;
    case 0020000u:
    case 0060000u:
        return FILED_VNODE_DEVICE;
    default:
        return FILED_VNODE_REGULAR;
    }
}

static uint32_t filed_wire_rights_to_vfs(uint64_t rights)
{
    const uint64_t known =
        FILED_WIRE_RIGHT_LOOKUP |
        FILED_WIRE_RIGHT_READ |
        FILED_WIRE_RIGHT_WRITE |
        FILED_WIRE_RIGHT_EXEC |
        FILED_WIRE_RIGHT_STAT |
        FILED_WIRE_RIGHT_GETDENTS |
        FILED_WIRE_RIGHT_CREATE |
        FILED_WIRE_RIGHT_REMOVE |
        FILED_WIRE_RIGHT_RENAME;
    return (uint32_t)(rights & known);
}

static uint32_t filed_wire_open_flags_to_vfs(uint64_t flags)
{
    const uint64_t known =
        FILED_WIRE_OPEN_CREATE |
        FILED_WIRE_OPEN_EXCLUSIVE |
        FILED_WIRE_OPEN_TRUNCATE |
        FILED_WIRE_OPEN_DIRECTORY |
        FILED_WIRE_OPEN_NOFOLLOW |
        FILED_WIRE_OPEN_CLOEXEC |
        FILED_WIRE_OPEN_APPEND |
        FILED_WIRE_OPEN_NONBLOCK |
        FILED_WIRE_OPEN_SYNC;
    return (uint32_t)(flags & known);
}

static uint32_t filed_wire_fd_flags_to_vfs(uint64_t flags)
{
    return (uint32_t)(flags & FILED_WIRE_FD_CLOEXEC);
}

static uint32_t filed_wire_file_status_flags_to_vfs(uint64_t flags)
{
    const uint64_t known =
        FILED_WIRE_FILE_APPEND |
        FILED_WIRE_FILE_NONBLOCK |
        FILED_WIRE_FILE_SYNC;
    return (uint32_t)(flags & known);
}

static uint64_t filed_vfs_fd_flags_to_wire(uint32_t flags)
{
    return (uint64_t)(flags & FILED_FD_CLOEXEC);
}

static uint64_t filed_vfs_file_status_flags_to_wire(uint32_t flags)
{
    const uint32_t known =
        FILED_FILE_APPEND |
        FILED_FILE_NONBLOCK |
        FILED_FILE_SYNC;
    return (uint64_t)(flags & known);
}

static int filed_wire_flags_are_known(uint64_t fd_flags, uint64_t status_flags)
{
    const uint64_t known_fd = FILED_WIRE_FD_CLOEXEC;
    const uint64_t known_status =
        FILED_WIRE_FILE_APPEND |
        FILED_WIRE_FILE_NONBLOCK |
        FILED_WIRE_FILE_SYNC;
    return (fd_flags & ~known_fd) == 0 && (status_flags & ~known_status) == 0;
}

static void *filed_map_request_page(
    const struct pacha_ipc_msg *request,
    uint64_t size,
    int *out_fd)
{
    if (request == NULL ||
        out_fd == NULL ||
        request->fd_count < 1 ||
        request->fds == NULL ||
        request->fds[0].fd < 16)
    {
        return NULL;
    }

    *out_fd = (int)request->fds[0].fd;
    return pacha_mmap(
        *out_fd,
        size,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
}

static int filed_write_stat_from_backend(
    filed_wire_statx_t *out,
    const koboxd_wire_fs_statx_t *stat,
    uint64_t handle_id)
{
    if (out == NULL || stat == NULL) {
        return -22;
    }
    memset(out, 0, sizeof(*out));
    out->handle = handle_id;
    out->mode = stat->mode;
    out->size = stat->size;
    out->blocks = stat->blocks;
    out->nlink = stat->nlink;
    out->kind = stat->kind;
    return 0;
}

static int filed_dispatch_root_stat(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_WIRE_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply(reply_fd, request->word3, -22, 0);
    }

    filed_wire_statx_t *stat = (filed_wire_statx_t *)page;
    filed_vfs_io_decision_t decision;
    koboxd_wire_fs_statx_t backend_stat;
    filed_status_t vfs_status = filed_vfs_stat_prepare(
        &runtime->vfs,
        runtime->root_handle_id,
        &decision);
    int64_t reply_status = filed_status_to_wire(vfs_status);
    uint64_t result = 0;
    if (vfs_status == FILED_OK) {
        memset(&backend_stat, 0, sizeof(backend_stat));
        reply_status = filed_kobox_backend_statx(
            &runtime->backend,
            decision.backend_object,
            &backend_stat);
        if (reply_status == 0) {
            (void)filed_write_stat_from_backend(stat, &backend_stat, runtime->root_handle_id);
            result = backend_stat.size;
        }
    }

    (void)pacha_munmap(page, FILED_WIRE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return filed_send_reply(reply_fd, request->word3, reply_status, result);
}

static int filed_dispatch_root_getdents(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_WIRE_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply(reply_fd, request->word3, -22, 0);
    }

    filed_wire_getdents_t *out = (filed_wire_getdents_t *)page;
    koboxd_wire_fs_getdents_t backend_entries;
    memset(&backend_entries, 0, sizeof(backend_entries));

    filed_vfs_io_decision_t decision;
    filed_status_t vfs_status = filed_vfs_getdents_prepare(
        &runtime->vfs,
        runtime->root_handle_id,
        &decision);
    int64_t reply_status = filed_status_to_wire(vfs_status);
    uint64_t result = 0;
    if (vfs_status == FILED_OK) {
        reply_status = filed_kobox_backend_getdents(
            &runtime->backend,
            decision.backend_object,
            decision.offset,
            &backend_entries);
    }
    if (reply_status == 0) {
        const uint64_t count =
            backend_entries.count > FILED_WIRE_DIRENT_CAPACITY ?
                FILED_WIRE_DIRENT_CAPACITY :
                backend_entries.count;
        memset(out, 0, sizeof(*out));
        out->dir_handle = runtime->root_handle_id;
        out->offset = decision.offset;
        out->count = count;
        for (uint64_t i = 0; i < count; ++i) {
            out->entries[i].handle = 0;
            out->entries[i].kind = backend_entries.entries[i].kind;
            out->entries[i].name_len = backend_entries.entries[i].name_len;
            snprintf(
                out->entries[i].name,
                sizeof(out->entries[i].name),
                "%s",
                backend_entries.entries[i].name);
        }
        result = count;
        vfs_status = filed_vfs_getdents_commit(&runtime->vfs, runtime->root_handle_id, count);
        if (vfs_status != FILED_OK) {
            reply_status = filed_status_to_wire(vfs_status);
        }
    }

    (void)pacha_munmap(page, FILED_WIRE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return filed_send_reply(reply_fd, request->word3, reply_status, result);
}

static int filed_name_is_terminated(const char *name, size_t capacity)
{
    return name != NULL && memchr(name, '\0', capacity) != NULL;
}

enum {
    FILED_WALK_RIGHTS =
        FILED_RIGHT_LOOKUP |
        FILED_RIGHT_STAT |
        FILED_RIGHT_GETDENTS,
};

static const char *filed_skip_slashes(const char *path)
{
    while (path != NULL && *path == '/') {
        ++path;
    }
    return path;
}

static void filed_close_walk_handle(
    filed_runtime_t *runtime,
    filed_handle_id_t handle_id,
    int owned)
{
    if (runtime != NULL &&
        owned &&
        handle_id != 0 &&
        handle_id != runtime->root_handle_id)
    {
        (void)filed_vfs_close_handle(&runtime->vfs, handle_id);
    }
}

static int64_t filed_lookup_and_open_component(
    filed_runtime_t *runtime,
    filed_handle_id_t parent_handle,
    const char *name,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open)
{
    filed_vfs_io_decision_t parent_decision;
    uint64_t object_id = 0;
    koboxd_wire_fs_statx_t backend_stat;
    filed_status_t status;
    int64_t reply_status;

    status = filed_vfs_lookup_prepare(&runtime->vfs, parent_handle, &parent_decision);
    reply_status = filed_status_to_wire(status);
    if (status != FILED_OK) {
        return reply_status;
    }

    reply_status = filed_kobox_backend_lookup(
        &runtime->backend,
        parent_decision.backend_object,
        name,
        &object_id);
    if (reply_status != 0) {
        return reply_status;
    }

    memset(&backend_stat, 0, sizeof(backend_stat));
    reply_status = filed_kobox_backend_statx(
        &runtime->backend,
        object_id,
        &backend_stat);
    if (reply_status != 0) {
        return reply_status;
    }

    status = filed_vfs_open_backend_child(
        &runtime->vfs,
        parent_handle,
        object_id,
        filed_kind_from_unix_type(backend_stat.kind),
        name,
        rights,
        open_flags,
        out_open);
    return filed_status_to_wire(status);
}

static int64_t filed_openat_path(
    filed_runtime_t *runtime,
    const filed_wire_openat_t *openat,
    filed_vfs_open_result_t *out_open)
{
    const uint32_t rights = filed_wire_rights_to_vfs(openat->rights);
    const uint32_t open_flags = filed_wire_open_flags_to_vfs(openat->open_flags);
    const char *path = openat->name;
    const int absolute = path[0] == '/';
    filed_handle_id_t current_handle =
        absolute || openat->dir_handle == 0 ?
            runtime->root_handle_id :
            (filed_handle_id_t)(uint32_t)openat->dir_handle;
    int current_owned = 0;

    if (path[0] == '\0') {
        return filed_status_to_wire(FILED_ERR_INVALID);
    }

    if (absolute) {
        path = filed_skip_slashes(path);
        if (*path == '\0') {
            const filed_status_t status = filed_vfs_open_root(
                &runtime->vfs,
                runtime->root_mount_id,
                rights,
                open_flags | FILED_OPEN_DIRECTORY,
                out_open);
            return filed_status_to_wire(status);
        }
    }

    for (;;) {
        char component[FILED_WIRE_NAME_BYTES];
        const char *component_start;
        const char *after_slashes;
        size_t component_len;
        int has_more;
        int require_directory;
        int final_component;

        path = filed_skip_slashes(path);
        if (*path == '\0') {
            filed_close_walk_handle(runtime, current_handle, current_owned);
            return filed_status_to_wire(FILED_ERR_INVALID);
        }

        component_start = path;
        while (*path != '\0' && *path != '/') {
            ++path;
        }
        component_len = (size_t)(path - component_start);
        if (component_len == 0 || component_len >= sizeof(component)) {
            filed_close_walk_handle(runtime, current_handle, current_owned);
            return filed_status_to_wire(FILED_ERR_INVALID);
        }

        after_slashes = filed_skip_slashes(path);
        has_more = *after_slashes != '\0';
        require_directory = (*path == '/');
        final_component = !has_more;

        memset(component, 0, sizeof(component));
        memcpy(component, component_start, component_len);

        if (component_len == 1 && component[0] == '.') {
            if (final_component) {
                const filed_status_t status = filed_vfs_open_existing(
                    &runtime->vfs,
                    current_handle,
                    rights,
                    open_flags | (require_directory ? FILED_OPEN_DIRECTORY : 0),
                    out_open);
                filed_close_walk_handle(runtime, current_handle, current_owned);
                return filed_status_to_wire(status);
            }
            path = after_slashes;
            continue;
        }

        if (component_len == 2 && component[0] == '.' && component[1] == '.') {
            filed_vfs_open_result_t parent_open;
            const uint32_t next_rights = final_component ? rights : FILED_WALK_RIGHTS;
            const uint32_t next_flags =
                final_component ?
                    (open_flags | (require_directory ? FILED_OPEN_DIRECTORY : 0)) :
                    FILED_OPEN_DIRECTORY;
            filed_status_t status;

            memset(&parent_open, 0, sizeof(parent_open));
            status = filed_vfs_open_parent(
                &runtime->vfs,
                current_handle,
                next_rights,
                next_flags,
                &parent_open);
            filed_close_walk_handle(runtime, current_handle, current_owned);
            if (status != FILED_OK) {
                return filed_status_to_wire(status);
            }
            if (final_component) {
                *out_open = parent_open;
                return 0;
            }
            current_handle = parent_open.handle_id;
            current_owned = 1;
            path = after_slashes;
            continue;
        }

        if (final_component) {
            const int64_t reply_status = filed_lookup_and_open_component(
                runtime,
                current_handle,
                component,
                rights,
                open_flags | (require_directory ? FILED_OPEN_DIRECTORY : 0),
                out_open);
            filed_close_walk_handle(runtime, current_handle, current_owned);
            return reply_status;
        } else {
            filed_vfs_open_result_t next_open;
            const int64_t reply_status = filed_lookup_and_open_component(
                runtime,
                current_handle,
                component,
                FILED_WALK_RIGHTS,
                FILED_OPEN_DIRECTORY,
                &next_open);
            filed_close_walk_handle(runtime, current_handle, current_owned);
            if (reply_status != 0) {
                return reply_status;
            }
            current_handle = next_open.handle_id;
            current_owned = 1;
            path = after_slashes;
        }
    }
}

static int filed_dispatch_openat(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_WIRE_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply(reply_fd, request->word3, -22, 0);
    }

    filed_wire_openat_t *openat = (filed_wire_openat_t *)page;
    filed_vfs_open_result_t open_result;
    int64_t reply_status = -22;
    uint64_t result = 0;

    memset(&open_result, 0, sizeof(open_result));
    if (filed_name_is_terminated(openat->name, sizeof(openat->name))) {
        reply_status = filed_openat_path(runtime, openat, &open_result);
    }
    if (reply_status == 0) {
        result = open_result.handle_id;
    }

    (void)pacha_munmap(page, FILED_WIRE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return filed_send_reply(reply_fd, request->word3, reply_status, result);
}

static int filed_dispatch_stat(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_WIRE_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply(reply_fd, request->word3, -22, 0);
    }

    filed_wire_statx_t *wire_stat = (filed_wire_statx_t *)page;
    filed_vfs_io_decision_t decision;
    koboxd_wire_fs_statx_t backend_stat;
    const uint64_t handle_id = wire_stat->handle;
    filed_status_t status = filed_vfs_stat_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)handle_id,
        &decision);
    int64_t reply_status = filed_status_to_wire(status);
    uint64_t result = 0;
    if (status == FILED_OK) {
        memset(&backend_stat, 0, sizeof(backend_stat));
        reply_status = filed_kobox_backend_statx(
            &runtime->backend,
            decision.backend_object,
            &backend_stat);
        if (reply_status == 0) {
            (void)filed_write_stat_from_backend(wire_stat, &backend_stat, handle_id);
            result = backend_stat.size;
        }
    }

    (void)pacha_munmap(page, FILED_WIRE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return filed_send_reply(reply_fd, request->word3, reply_status, result);
}

static int filed_dispatch_pread(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_WIRE_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply(reply_fd, request->word3, -22, 0);
    }

    filed_wire_io_t *io = (filed_wire_io_t *)page;
    filed_vfs_io_decision_t decision;
    uint64_t bytes = 0;
    filed_status_t status = filed_vfs_pread_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)io->handle,
        io->offset,
        io->length,
        &decision);
    int64_t reply_status = filed_status_to_wire(status);
    if (status == FILED_OK) {
        uint64_t length = decision.length;
        if (length > FILED_WIRE_IO_BYTES) {
            length = FILED_WIRE_IO_BYTES;
        }
        reply_status = filed_kobox_backend_pread(
            &runtime->backend,
            decision.backend_object,
            decision.offset,
            io->data,
            length,
            &bytes);
        if (reply_status == 0) {
            io->length = bytes;
        }
    }

    (void)pacha_munmap(page, FILED_WIRE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return filed_send_reply(reply_fd, request->word3, reply_status, bytes);
}

static int filed_dispatch_read(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_WIRE_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply(reply_fd, request->word3, -22, 0);
    }

    filed_wire_io_t *io = (filed_wire_io_t *)page;
    filed_vfs_io_decision_t decision;
    uint64_t bytes = 0;
    filed_status_t status = filed_vfs_read_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)io->handle,
        io->length,
        &decision);
    int64_t reply_status = filed_status_to_wire(status);
    if (status == FILED_OK) {
        uint64_t length = decision.length;
        if (length > FILED_WIRE_IO_BYTES) {
            length = FILED_WIRE_IO_BYTES;
        }
        reply_status = filed_kobox_backend_pread(
            &runtime->backend,
            decision.backend_object,
            decision.offset,
            io->data,
            length,
            &bytes);
        if (reply_status == 0) {
            io->offset = decision.offset;
            io->length = bytes;
            status = filed_vfs_read_commit(
                &runtime->vfs,
                (filed_handle_id_t)(uint32_t)io->handle,
                bytes);
            if (status != FILED_OK) {
                reply_status = filed_status_to_wire(status);
            }
        }
    }

    (void)pacha_munmap(page, FILED_WIRE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return filed_send_reply(reply_fd, request->word3, reply_status, bytes);
}

static int filed_dispatch_pwrite(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_WIRE_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply(reply_fd, request->word3, -22, 0);
    }

    filed_wire_io_t *io = (filed_wire_io_t *)page;
    filed_vfs_io_decision_t decision;
    uint64_t bytes = 0;
    filed_status_t status = filed_vfs_pwrite_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)io->handle,
        io->offset,
        io->length,
        &decision);
    int64_t reply_status = filed_status_to_wire(status);
    if (status == FILED_OK) {
        uint64_t length = decision.length;
        if (length > FILED_WIRE_IO_BYTES) {
            length = FILED_WIRE_IO_BYTES;
        }
        reply_status = filed_kobox_backend_pwrite(
            &runtime->backend,
            decision.backend_object,
            decision.offset,
            io->data,
            length,
            &bytes);
        if (reply_status == 0) {
            io->offset = decision.offset;
            io->length = bytes;
        }
    }

    (void)pacha_munmap(page, FILED_WIRE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return filed_send_reply(reply_fd, request->word3, reply_status, bytes);
}

static int filed_dispatch_write(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_WIRE_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply(reply_fd, request->word3, -22, 0);
    }

    filed_wire_io_t *io = (filed_wire_io_t *)page;
    filed_vfs_io_decision_t decision;
    uint64_t bytes = 0;
    filed_status_t status = filed_vfs_write_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)io->handle,
        io->length,
        &decision);
    int64_t reply_status = filed_status_to_wire(status);
    if (status == FILED_OK) {
        uint64_t length = decision.length;
        if (length > FILED_WIRE_IO_BYTES) {
            length = FILED_WIRE_IO_BYTES;
        }
        reply_status = filed_kobox_backend_pwrite(
            &runtime->backend,
            decision.backend_object,
            decision.offset,
            io->data,
            length,
            &bytes);
        if (reply_status == 0) {
            io->offset = decision.offset;
            io->length = bytes;
            status = filed_vfs_write_commit(
                &runtime->vfs,
                (filed_handle_id_t)(uint32_t)io->handle,
                bytes);
            if (status != FILED_OK) {
                reply_status = filed_status_to_wire(status);
            }
        }
    }

    (void)pacha_munmap(page, FILED_WIRE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return filed_send_reply(reply_fd, request->word3, reply_status, bytes);
}

static int filed_dispatch_fsync(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    filed_vfs_io_decision_t decision;
    const filed_handle_id_t handle_id = (filed_handle_id_t)(uint32_t)request->word2;
    filed_status_t status = filed_vfs_fsync_prepare(
        &runtime->vfs,
        handle_id,
        &decision);
    int64_t reply_status = filed_status_to_wire(status);
    if (status == FILED_OK) {
        reply_status = filed_kobox_backend_fsync(
            &runtime->backend,
            decision.backend_object);
    }
    return filed_send_reply(reply_fd, request->word3, reply_status, 0);
}

static int filed_dispatch_getdents(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_WIRE_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply(reply_fd, request->word3, -22, 0);
    }

    filed_wire_getdents_t *out = (filed_wire_getdents_t *)page;
    koboxd_wire_fs_getdents_t backend_entries;
    filed_vfs_io_decision_t decision;
    filed_status_t status = filed_vfs_getdents_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)out->dir_handle,
        &decision);
    int64_t reply_status = filed_status_to_wire(status);
    uint64_t result = 0;
    memset(&backend_entries, 0, sizeof(backend_entries));
    if (status == FILED_OK) {
        reply_status = filed_kobox_backend_getdents(
            &runtime->backend,
            decision.backend_object,
            decision.offset,
            &backend_entries);
    }
    if (reply_status == 0) {
        const uint64_t count =
            backend_entries.count > FILED_WIRE_DIRENT_CAPACITY ?
                FILED_WIRE_DIRENT_CAPACITY :
                backend_entries.count;
        const uint64_t dir_handle = out->dir_handle;
        memset(out, 0, sizeof(*out));
        out->dir_handle = dir_handle;
        out->offset = decision.offset;
        out->count = count;
        for (uint64_t i = 0; i < count; ++i) {
            out->entries[i].handle = 0;
            out->entries[i].kind = backend_entries.entries[i].kind;
            out->entries[i].name_len = backend_entries.entries[i].name_len;
            snprintf(
                out->entries[i].name,
                sizeof(out->entries[i].name),
                "%s",
                backend_entries.entries[i].name);
        }
        result = count;
        status = filed_vfs_getdents_commit(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)dir_handle,
            count);
        if (status != FILED_OK) {
            reply_status = filed_status_to_wire(status);
        }
    }

    (void)pacha_munmap(page, FILED_WIRE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return filed_send_reply(reply_fd, request->word3, reply_status, result);
}

static int filed_dispatch_close(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    const filed_handle_id_t handle_id = (filed_handle_id_t)(uint32_t)request->word2;
    if (handle_id == runtime->root_handle_id) {
        return filed_send_reply(reply_fd, request->word3, -13, 0);
    }
    const filed_status_t status = filed_vfs_close_handle(&runtime->vfs, handle_id);
    return filed_send_reply(reply_fd, request->word3, filed_status_to_wire(status), 0);
}

static int filed_dispatch_dup(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_WIRE_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply(reply_fd, request->word3, -22, 0);
    }

    filed_wire_handle_flags_t *wire_flags = (filed_wire_handle_flags_t *)page;
    filed_handle_id_t dup_handle = 0;
    int64_t reply_status = -22;
    uint64_t result = 0;
    if (wire_flags->reserved0 == 0 &&
        filed_wire_flags_are_known(wire_flags->fd_flags, wire_flags->status_flags))
    {
        const filed_status_t status = filed_vfs_dup_handle(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)wire_flags->handle,
            filed_wire_fd_flags_to_vfs(wire_flags->fd_flags),
            &dup_handle);
        reply_status = filed_status_to_wire(status);
        if (status == FILED_OK) {
            wire_flags->handle = dup_handle;
            result = dup_handle;
        }
    }

    (void)pacha_munmap(page, FILED_WIRE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return filed_send_reply(reply_fd, request->word3, reply_status, result);
}

static int filed_dispatch_get_flags(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_WIRE_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply(reply_fd, request->word3, -22, 0);
    }

    filed_wire_handle_flags_t *wire_flags = (filed_wire_handle_flags_t *)page;
    filed_vfs_handle_flags_t flags;
    const uint64_t handle = wire_flags->handle;
    uint64_t result = 0;
    filed_status_t status;
    memset(&flags, 0, sizeof(flags));
    status = filed_vfs_get_handle_flags(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)handle,
        &flags);
    if (status == FILED_OK) {
        memset(wire_flags, 0, sizeof(*wire_flags));
        wire_flags->handle = handle;
        wire_flags->fd_flags = filed_vfs_fd_flags_to_wire(flags.fd_flags);
        wire_flags->status_flags =
            filed_vfs_file_status_flags_to_wire(flags.status_flags);
        result = wire_flags->fd_flags;
    }

    (void)pacha_munmap(page, FILED_WIRE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return filed_send_reply(
        reply_fd,
        request->word3,
        filed_status_to_wire(status),
        result);
}

static int filed_dispatch_set_flags(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_WIRE_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply(reply_fd, request->word3, -22, 0);
    }

    filed_wire_handle_flags_t *wire_flags = (filed_wire_handle_flags_t *)page;
    filed_vfs_handle_flags_t flags;
    filed_status_t status = FILED_ERR_INVALID;
    if (wire_flags->reserved0 == 0 &&
        filed_wire_flags_are_known(wire_flags->fd_flags, wire_flags->status_flags))
    {
        flags.fd_flags = filed_wire_fd_flags_to_vfs(wire_flags->fd_flags);
        flags.status_flags =
            filed_wire_file_status_flags_to_vfs(wire_flags->status_flags);
        status = filed_vfs_set_handle_flags(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)wire_flags->handle,
            &flags);
    }

    (void)pacha_munmap(page, FILED_WIRE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return filed_send_reply(reply_fd, request->word3, filed_status_to_wire(status), 0);
}

static int filed_dispatch_exec_path(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_WIRE_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply(reply_fd, request->word3, -22, 0);
    }

    filed_wire_exec_path_t *exec = (filed_wire_exec_path_t *)page;
    const uint64_t known_flags =
        FILED_WIRE_EXEC_BOOTSTRAP_FD |
        FILED_WIRE_EXEC_INHERIT_FDS |
        FILED_WIRE_EXEC_PATCH_BOOTSTRAP_FDS |
        FILED_WIRE_EXEC_INHERIT_HANDLES;
    int64_t reply_status = -22;
    int process_fd = -1;
    int thread_fd = -1;
    int bootstrap_fd = -1;
    int inherit_fds[FILED_WIRE_EXEC_MAX_INHERIT_FDS];
    filed_handle_id_t inherit_handles[FILED_WIRE_EXEC_MAX_INHERIT_HANDLES];
    memset(inherit_fds, 0, sizeof(inherit_fds));
    memset(inherit_handles, 0, sizeof(inherit_handles));

    if ((exec->flags & ~known_flags) != 0 ||
        exec->inherit_fd_count > FILED_WIRE_EXEC_MAX_INHERIT_FDS ||
        exec->inherit_handle_count > FILED_WIRE_EXEC_MAX_INHERIT_HANDLES ||
        exec->fd_patch_count > FILED_WIRE_EXEC_MAX_FD_PATCHES ||
        exec->reserved1 != 0 ||
        !filed_name_is_terminated(exec->path, sizeof(exec->path)) ||
        !filed_name_is_terminated(exec->argv0, sizeof(exec->argv0)))
    {
        goto out;
    }
    const uint64_t inherit_fd_count = exec->inherit_fd_count;
    const uint64_t inherit_handle_count = exec->inherit_handle_count;

    const uint64_t has_bootstrap =
        (exec->flags & FILED_WIRE_EXEC_BOOTSTRAP_FD) != 0 ? 1u : 0u;
    if ((exec->flags & FILED_WIRE_EXEC_INHERIT_FDS) == 0 && inherit_fd_count != 0) {
        goto out;
    }
    if ((exec->flags & FILED_WIRE_EXEC_PATCH_BOOTSTRAP_FDS) == 0 && exec->fd_patch_count != 0) {
        goto out;
    }
    if ((exec->flags & FILED_WIRE_EXEC_PATCH_BOOTSTRAP_FDS) != 0 && has_bootstrap == 0) {
        goto out;
    }
    if ((exec->flags & FILED_WIRE_EXEC_INHERIT_HANDLES) == 0 && inherit_handle_count != 0) {
        goto out;
    }

    const uint64_t expected_fd_count = 1u + inherit_fd_count + has_bootstrap + 1u;
    if (request->fd_count != expected_fd_count || request->fds == NULL) {
        goto out;
    }

    for (uint64_t i = 0; i < inherit_fd_count; ++i) {
        const uint64_t fd_index = 1u + i;
        if (request->fds[fd_index].fd < 16) {
            goto out;
        }
        inherit_fds[i] = (int)request->fds[fd_index].fd;
    }

    if ((exec->flags & FILED_WIRE_EXEC_BOOTSTRAP_FD) != 0) {
        const uint64_t fd_index = 1u + inherit_fd_count;
        if (request->fds[fd_index].fd < 16) {
            goto out;
        }
        bootstrap_fd = (int)request->fds[fd_index].fd;
    }

    for (uint64_t i = 0; i < inherit_handle_count; ++i) {
        filed_handle_id_t dup_handle = 0;
        const filed_status_t dup_status = filed_vfs_dup_handle_for_exec(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)exec->inherit_handles[i],
            &dup_handle);
        if (dup_status != FILED_OK) {
            reply_status = filed_status_to_wire(dup_status);
            goto out;
        }
        inherit_handles[i] = dup_handle;
    }

    if ((exec->flags & FILED_WIRE_EXEC_PATCH_BOOTSTRAP_FDS) != 0) {
        void *bootstrap_page = pacha_mmap(
            bootstrap_fd,
            FILED_BOOTSTRAP_PATCH_BYTES,
            PACHA_PROT_READ | PACHA_PROT_WRITE,
            PACHA_MMAP_SHARED,
            0);
        if (bootstrap_page == NULL) {
            goto out;
        }
        reply_status = 0;
        for (uint64_t i = 0; i < exec->fd_patch_count; ++i) {
            const filed_wire_exec_fd_patch_t *patch = &exec->fd_patches[i];
            uint64_t value = 0;
            if (patch->reserved0 != 0 || patch->offset > FILED_BOOTSTRAP_PATCH_BYTES - 8u) {
                reply_status = -22;
                break;
            }
            if (patch->kind == FILED_WIRE_EXEC_PATCH_INHERIT_FD) {
                if (patch->index >= inherit_fd_count) {
                    reply_status = -22;
                    break;
                }
                value = (uint64_t)(uint32_t)inherit_fds[patch->index];
            } else if (patch->kind == FILED_WIRE_EXEC_PATCH_BOOTSTRAP_FD) {
                if (patch->index != 0) {
                    reply_status = -22;
                    break;
                }
                value = (uint64_t)(uint32_t)bootstrap_fd;
            } else if (patch->kind == FILED_WIRE_EXEC_PATCH_INHERIT_HANDLE) {
                if (patch->index >= inherit_handle_count) {
                    reply_status = -22;
                    break;
                }
                value = (uint64_t)(uint32_t)inherit_handles[patch->index];
            } else {
                reply_status = -22;
                break;
            }
            filed_write_u64_le(bootstrap_page, patch->offset, value);
            reply_status = 0;
        }
        (void)pacha_munmap(bootstrap_page, FILED_BOOTSTRAP_PATCH_BYTES);
        if (reply_status != 0) {
            goto out;
        }
    }

    filed_wire_openat_t openat;
    memset(&openat, 0, sizeof(openat));
    openat.dir_handle = exec->dir_handle;
    openat.rights =
        FILED_WIRE_RIGHT_READ |
        FILED_WIRE_RIGHT_EXEC |
        FILED_WIRE_RIGHT_STAT;
    snprintf(openat.name, sizeof(openat.name), "%s", exec->path);

    filed_vfs_open_result_t open_result;
    memset(&open_result, 0, sizeof(open_result));
    reply_status = filed_openat_path(runtime, &openat, &open_result);
    if (reply_status != 0) {
        goto out;
    }

    reply_status = filed_exec_handle(
        runtime,
        open_result.handle_id,
        exec,
        inherit_fds,
        inherit_fd_count,
        bootstrap_fd,
        &process_fd,
        &thread_fd);
    filed_close_walk_handle(runtime, open_result.handle_id, 1);
    if (reply_status != 0) {
        goto out;
    }

out:
    (void)pacha_munmap(page, FILED_WIRE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    if (bootstrap_fd >= 16) {
        (void)pacha_fd_close(bootstrap_fd);
    }
    for (uint64_t i = 0; i < FILED_WIRE_EXEC_MAX_INHERIT_FDS; ++i) {
        if (inherit_fds[i] >= 16) {
            (void)pacha_fd_close(inherit_fds[i]);
        }
    }
    if (reply_status != 0) {
        for (uint64_t i = 0; i < FILED_WIRE_EXEC_MAX_INHERIT_HANDLES; ++i) {
            if (inherit_handles[i] != 0) {
                (void)filed_vfs_close_handle(&runtime->vfs, inherit_handles[i]);
            }
        }
    }
    if (reply_status == 0 && process_fd >= 16 && thread_fd >= 16) {
        const int send_status = filed_send_exec_reply(reply_fd, request->word3, process_fd, thread_fd);
        if (send_status != 0) {
            for (uint64_t i = 0; i < FILED_WIRE_EXEC_MAX_INHERIT_HANDLES; ++i) {
                if (inherit_handles[i] != 0) {
                    (void)filed_vfs_close_handle(&runtime->vfs, inherit_handles[i]);
                }
            }
        }
        return send_status;
    }
    return filed_send_reply(reply_fd, request->word3, reply_status, 0);
}

int filed_dispatch_client_once(filed_runtime_t *runtime, int client_fd)
{
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
    struct pacha_ipc_msg request;

    if (runtime == NULL || client_fd < 16) {
        return -1;
    }

    memset(fds, 0, sizeof(fds));
    memset(&request, 0, sizeof(request));
    request.fds = fds;
    request.fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS;

    int status = filed_ipc_recv_wait(client_fd, &request);
    if (status != 0) {
        return status;
    }
    if (request.fd_count < 1 || fds[request.fd_count - 1].fd < 16) {
        return -22;
    }
    const int reply_fd = (int)fds[request.fd_count - 1].fd;
    if (request.word0 != FILED_WIRE_REQUEST_MAGIC ||
        request.word3 == 0)
    {
        return filed_send_reply(reply_fd, request.word3, -22, 0);
    }

    switch (request.word1) {
    case FILED_WIRE_OP_HELLO:
        return filed_send_reply(reply_fd, request.word3, 0, FILED_WIRE_VERSION);
    case FILED_WIRE_OP_ROOT_STAT:
        return filed_dispatch_root_stat(runtime, reply_fd, &request);
    case FILED_WIRE_OP_ROOT_GETDENTS:
        return filed_dispatch_root_getdents(runtime, reply_fd, &request);
    case FILED_WIRE_OP_OPENAT:
        return filed_dispatch_openat(runtime, reply_fd, &request);
    case FILED_WIRE_OP_STAT:
        return filed_dispatch_stat(runtime, reply_fd, &request);
    case FILED_WIRE_OP_PREAD:
        return filed_dispatch_pread(runtime, reply_fd, &request);
    case FILED_WIRE_OP_READ:
        return filed_dispatch_read(runtime, reply_fd, &request);
    case FILED_WIRE_OP_PWRITE:
        return filed_dispatch_pwrite(runtime, reply_fd, &request);
    case FILED_WIRE_OP_WRITE:
        return filed_dispatch_write(runtime, reply_fd, &request);
    case FILED_WIRE_OP_FSYNC:
        return filed_dispatch_fsync(runtime, reply_fd, &request);
    case FILED_WIRE_OP_GETDENTS:
        return filed_dispatch_getdents(runtime, reply_fd, &request);
    case FILED_WIRE_OP_CLOSE:
        return filed_dispatch_close(runtime, reply_fd, &request);
    case FILED_WIRE_OP_DUP:
        return filed_dispatch_dup(runtime, reply_fd, &request);
    case FILED_WIRE_OP_GET_FLAGS:
        return filed_dispatch_get_flags(runtime, reply_fd, &request);
    case FILED_WIRE_OP_SET_FLAGS:
        return filed_dispatch_set_flags(runtime, reply_fd, &request);
    case FILED_WIRE_OP_EXEC_PATH:
        return filed_dispatch_exec_path(runtime, reply_fd, &request);
    default:
        return filed_send_reply(reply_fd, request.word3, -95, 0);
    }
}

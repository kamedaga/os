#include "common.h"
#include "filed/unix_path.h"
#include "../vfs/private.h"

struct filed_unix_hold {
    struct filed_unix_hold *next;
    uint64_t id;
    filed_file_t *file;
};

static int release(filed_runtime_t *runtime, uint64_t id)
{
    struct filed_unix_hold **cursor = &runtime->unix_holds;
    while (*cursor && (*cursor)->id != id) cursor = &(*cursor)->next;
    if (!*cursor) return -9;
    struct filed_unix_hold *hold = *cursor;
    *cursor = hold->next;
    filed_vfs_reclaim_result_t reclaim = {0};
    filed_release_open_file(&runtime->vfs, hold->file, &reclaim);
    free(hold);
    int status = 0;
    if (reclaim.released) {
        status = filed_cache_flush_object(runtime, reclaim.backend_object);
        if (status != 0) return status;
        filed_cache_release_object(runtime, reclaim.backend_object);
        status = filed_release_reclaimed_object(runtime, &reclaim);
    }
    runtime->vnode_eviction_pending = 1;
    return status;
}

void filed_unix_path_disconnect(filed_runtime_t *runtime)
{
    while (runtime->unix_holds) (void)release(runtime, runtime->unix_holds->id);
    if (runtime->unix_path_fd >= 16) (void)pacha_fd_close(runtime->unix_path_fd);
    runtime->unix_path_fd = -1;
}

static int path_open(filed_runtime_t *runtime, struct filed_unix_path *request)
{
    if (request->operation == FILED_UNIX_PATH_RELEASE) return release(runtime, request->hold);
    struct filed_client *client = runtime->clients;
    while (client && (client->identity.generation != request->generation ||
        client->identity.pid != request->pid)) client = client->next;
    if (!client || request->reserved) return -13;
    runtime->actor = client;
    runtime->vfs.actor_client = client->id;
    runtime->vfs.actor_rights = client->identity.rights;
    filed_openat_t check = { .dir_handle = request->directory,
        .rights = FILED_RIGHT_STAT };
    int allowed = filed_client_authorize(runtime,
        request->operation == FILED_UNIX_PATH_CREATE ? FILED_OP_VFS_MKNOD : FILED_OP_VFS_OPENAT,
        &check, sizeof(check), 0);
    if (allowed) return allowed;
    if (request->operation > FILED_UNIX_PATH_CREATE || request->directory > UINT32_MAX ||
        !memchr(request->path, 0, sizeof(request->path)) || !request->path[0]) return -22;
    if (runtime->unix_hold_sequence == UINT64_MAX) return -75;
    struct filed_unix_hold *hold = calloc(1, sizeof(*hold));
    if (!hold) return -12;
    int status = 0;
    /* Filed dispatch is serialized: creation, lookup and retained handle
     * installation below cannot interleave with another pathname mutation. */
    if (request->operation == FILED_UNIX_PATH_CREATE) {
        filed_mknod_t node = { .dir_handle = request->directory,
            .mode = 0140000u | (request->mode & 0777u) };
        memcpy(node.name, request->path, sizeof(request->path));
        status = (int)filed_dispatch_mknod_page(runtime, &node).status;
        if (status != 0) { free(hold); return status == -17 ? -98 : status; }
    }
    filed_openat_t open = { .dir_handle = request->directory, .rights = FILED_RIGHT_STAT };
    memcpy(open.name, request->path, sizeof(request->path));
    filed_vfs_open_result_t opened = {0};
    status = (int)filed_openat_path(runtime, &open, &opened);
    if (status != 0) goto fail;
    filed_statx_t stat = { .handle = opened.handle_id };
    status = (int)filed_dispatch_stat_page(runtime, &stat).status;
    if (status == 0 && (stat.kind & 0170000u) != 0140000u) status = -111;
    filed_vnode_t *vnode = NULL;
    for (unsigned i = 0; i < FILED_MAX_VNODES; i++) {
        if (runtime->vfs.vnodes[i].active && runtime->vfs.vnodes[i].id == opened.vnode_id) {
            vnode = &runtime->vfs.vnodes[i]; break;
        }
    }
    if (status == 0 && (!vnode || !stat.inode_number)) status = -5;
    filed_handle_t *handle = filed_find_handle(&runtime->vfs, opened.handle_id);
    filed_file_t *file = handle && handle->target_kind == FILED_HANDLE_FILE ?
        filed_find_file(&runtime->vfs, handle->target_id) : NULL;
    if (status == 0) status = filed_status_to_wire(filed_file_ref_inc(file));
    if (status != 0) {
        (void)filed_close_handle_runtime(runtime, opened.handle_id);
        goto fail;
    }
    /* Keep an open-file reference, not a publicly addressable VFS handle. */
    (void)filed_vfs_close_handle(&runtime->vfs, opened.handle_id);
    hold->file = file;
    hold->id = ++runtime->unix_hold_sequence;
    hold->next = runtime->unix_holds;
    runtime->unix_holds = hold;
    request->hold = hold->id;
    request->filesystem = vnode->mount_id;
    request->inode = stat.inode_number;
    return 0;
fail:
    /* No other request has run since CREATE; remove only the node made by
     * this operation if retaining its identity failed. */
    if (request->operation == FILED_UNIX_PATH_CREATE) {
        filed_unlink_t node = { .dir_handle = request->directory };
        memcpy(node.name, request->path, sizeof(request->path));
        (void)filed_dispatch_unlink_page(runtime, &node);
    }
    free(hold);
    return status;
}

int filed_unix_path_receive(filed_runtime_t *runtime)
{
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS] = {{0}};
    struct pacha_ipc_msg message = { .fds = fds, .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS };
    int status = pacha_ipc_recv(runtime->unix_path_fd, &message);
    if (status != 0) return status;
    int reply = -1;
    struct filed_unix_path *page = NULL;
    if (message.fd_count) {
        struct pacha_fd_info info;
        int last = (int)fds[message.fd_count - 1].fd;
        if (pacha_fd_get_info(last, &info) == 0 && info.kind == PACHA_FD_KIND_REPLY) reply = last;
    }
    status = -22;
    if (reply >= 16 && message.fd_count == 2 && message.word0 == FILED_UNIX_PATH_MAGIC) {
        struct pacha_fd_info info;
        if (pacha_fd_get_info((int)fds[0].fd, &info) == 0 && info.kind == PACHA_FD_KIND_VMO && info.size >= 4096)
            page = pacha_mmap((int)fds[0].fd, 4096, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
        if (page) {
            struct filed_unix_path request = *page;
            status = request.magic == FILED_UNIX_PATH_MAGIC ? path_open(runtime, &request) : -22;
            request.status = status;
            *page = request;
        }
    }
    if (reply >= 16) {
        struct pacha_ipc_msg response = { .word0 = FILED_UNIX_PATH_MAGIC, .word1 = (uint64_t)(int64_t)status };
        (void)pacha_ipc_reply(reply, &response);
    }
    if (page) (void)pacha_munmap(page, 4096);
    for (unsigned i = 0; i < message.fd_count; i++) (void)pacha_fd_close((int)fds[i].fd);
    return 0;
}

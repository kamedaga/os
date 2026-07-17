#include "common.h"

int64_t filed_status_to_wire(filed_status_t status)
{
    switch (status) {
    case FILED_OK:
        return 0;
    case FILED_ERR_NOT_FOUND:
        return -PACHA_LINUX_ENOENT;
    case FILED_ERR_NOT_DIR:
        return -PACHA_LINUX_ENOTDIR;
    case FILED_ERR_IS_DIR:
        return -PACHA_LINUX_EISDIR;
    case FILED_ERR_EXISTS:
        return -PACHA_LINUX_EEXIST;
    case FILED_ERR_DENIED:
        return -PACHA_LINUX_EACCES;
    case FILED_ERR_INVALID:
        return -PACHA_LINUX_EINVAL;
    case FILED_ERR_CROSS_MOUNT:
        return -PACHA_LINUX_EXDEV;
    case FILED_ERR_NOT_EMPTY:
        return -PACHA_LINUX_ENOTEMPTY;
    case FILED_ERR_IO:
        return -PACHA_LINUX_EIO;
    case FILED_ERR_UNSUPPORTED:
        return -PACHA_LINUX_ENOTSUP;
    case FILED_ERR_BAD_FORMAT:
    case FILED_ERR_INVALID_IMAGE:
        return -PACHA_LINUX_ENOEXEC;
    case FILED_ERR_LOOP:
        return -PACHA_LINUX_ELOOP;
    case FILED_ERR_OVERFLOW:
        return -PACHA_LINUX_EOVERFLOW;
    case FILED_ERR_FULL:
        return -PACHA_LINUX_ENOSPC;
    }
    return -PACHA_LINUX_EINVAL;
}

int filed_release_reclaimed_object(
    filed_runtime_t *runtime,
    const filed_vfs_reclaim_result_t *reclaim)
{
    if (runtime == NULL || reclaim == NULL || !reclaim->released || reclaim->backend_object == 0) {
        return 0;
    }
    return filed_backend_release_object(runtime, reclaim->backend_object);
}

static bool filed_runtime_backend_object_evictable(
    void *context,
    filed_backend_object_id_t backend_object)
{
    return filed_cache_object_evictable((filed_runtime_t *)context, backend_object);
}

static int filed_evict_unused_linked_vnodes(filed_runtime_t *runtime)
{
    enum { FILED_LINKED_VNODE_CACHE_LIMIT = 48 };
    for (;;) {
        filed_vfs_reclaim_result_t reclaim;
        memset(&reclaim, 0, sizeof(reclaim));
        const filed_status_t status = filed_vfs_evict_lru_unused_linked(
            &runtime->vfs,
            FILED_LINKED_VNODE_CACHE_LIMIT,
            filed_runtime_backend_object_evictable,
            runtime,
            &reclaim);
        if (status != FILED_OK) {
            return filed_status_to_wire(status);
        }
        if (!reclaim.released || reclaim.backend_object == 0) {
            return 0;
        }
        int flush_status = filed_cache_flush_object(runtime, reclaim.backend_object);
        if (flush_status != 0) {
            return flush_status;
        }
        filed_cache_release_object(runtime, reclaim.backend_object);
        const int release_status = filed_backend_release_object(runtime, reclaim.backend_object);
        if (release_status != 0) {
            return release_status;
        }
    }
}

static int64_t filed_close_handle_runtime_impl(
    filed_runtime_t *runtime,
    filed_handle_id_t handle_id,
    bool defer_eviction)
{
    filed_vfs_reclaim_result_t reclaim;
    if (runtime == NULL) {
        return filed_status_to_wire(FILED_ERR_INVALID);
    }

    const int lease_fd = filed_vfs_get_handle_lease(&runtime->vfs, handle_id);
    memset(&reclaim, 0, sizeof(reclaim));
    const filed_status_t status = filed_vfs_close_handle_ex(&runtime->vfs, handle_id, &reclaim);
    if (status != FILED_OK) {
        return filed_status_to_wire(status);
    }
    if (lease_fd >= 16) (void)pacha_fd_close(lease_fd);
    if (reclaim.released && reclaim.backend_object != 0) {
        const int flush_status = filed_cache_flush_object(runtime, reclaim.backend_object);
        if (flush_status != 0) {
            return flush_status;
        }
        filed_cache_release_object(runtime, reclaim.backend_object);
    }
    const int release_status = filed_release_reclaimed_object(runtime, &reclaim);
    if (release_status != 0) {
        return release_status;
    }
    if (defer_eviction) {
        runtime->vnode_eviction_pending = 1;
        return 0;
    }
    runtime->vnode_eviction_pending = 0;
    return filed_evict_unused_linked_vnodes(runtime);
}

int64_t filed_close_handle_runtime(
    filed_runtime_t *runtime,
    filed_handle_id_t handle_id)
{
    return filed_close_handle_runtime_impl(runtime, handle_id, false);
}

int64_t filed_close_handle_runtime_deferred(
    filed_runtime_t *runtime,
    filed_handle_id_t handle_id)
{
    return filed_close_handle_runtime_impl(runtime, handle_id, true);
}

int filed_maintain_vnode_cache(filed_runtime_t *runtime)
{
    if (runtime == NULL || !runtime->vnode_eviction_pending) {
        return 0;
    }
    runtime->vnode_eviction_pending = 0;
    return filed_evict_unused_linked_vnodes(runtime);
}

void filed_write_u64_le(void *base, uint64_t offset, uint64_t value)
{
    unsigned char *p = (unsigned char *)base + offset;
    for (unsigned int i = 0; i < 8; ++i) {
        p[i] = (unsigned char)(value >> (i * 8u));
    }
}

filed_vnode_kind_t filed_kind_from_unix_type(uint64_t kind)
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
    case 0140000u:
        return FILED_VNODE_SOCKET;
    default:
        return FILED_VNODE_REGULAR;
    }
}

uint32_t filed_rights_to_vfs(uint64_t rights)
{
    const uint64_t known =
        FILED_RIGHT_LOOKUP |
        FILED_RIGHT_READ |
        FILED_RIGHT_WRITE |
        FILED_RIGHT_EXEC |
        FILED_RIGHT_STAT |
        FILED_RIGHT_GETDENTS |
        FILED_RIGHT_CREATE |
        FILED_RIGHT_REMOVE |
        FILED_RIGHT_RENAME;
    return (uint32_t)(rights & known);
}

uint32_t filed_open_flags_to_vfs(uint64_t flags)
{
    const uint64_t known =
        FILED_OPEN_CREATE |
        FILED_OPEN_EXCLUSIVE |
        FILED_OPEN_TRUNCATE |
        FILED_OPEN_DIRECTORY |
        FILED_OPEN_NOFOLLOW |
        FILED_OPEN_CLOEXEC |
        FILED_OPEN_APPEND |
        FILED_OPEN_NONBLOCK |
        FILED_OPEN_SYNC;
    return (uint32_t)(flags & known);
}

uint32_t filed_fd_flags_to_vfs(uint64_t flags)
{
    return (uint32_t)(flags & FILED_FD_CLOEXEC);
}

uint32_t filed_file_status_flags_to_vfs(uint64_t flags)
{
    const uint64_t known =
        FILED_FILE_APPEND |
        FILED_FILE_NONBLOCK |
        FILED_FILE_SYNC;
    return (uint32_t)(flags & known);
}

uint64_t filed_vfs_fd_flags_to_wire(uint32_t flags)
{
    return (uint64_t)(flags & FILED_FD_CLOEXEC);
}

uint64_t filed_vfs_file_status_flags_to_wire(uint32_t flags)
{
    const uint32_t known =
        FILED_FILE_APPEND |
        FILED_FILE_NONBLOCK |
        FILED_FILE_SYNC;
    return (uint64_t)(flags & known);
}

int filed_flags_are_known(uint64_t fd_flags, uint64_t status_flags)
{
    const uint64_t known_fd = FILED_FD_CLOEXEC;
    const uint64_t known_status =
        FILED_FILE_APPEND |
        FILED_FILE_NONBLOCK |
        FILED_FILE_SYNC;
    return (fd_flags & ~known_fd) == 0 && (status_flags & ~known_status) == 0;
}

void *filed_map_request_page(
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

filed_page_dispatch_result_t filed_page_result(int64_t status, uint64_t result)
{
    filed_page_dispatch_result_t out;
    memset(&out, 0, sizeof(out));
    out.status = status;
    out.result = result;
    out.process_fd = -1;
    out.thread_fd = -1;
    return out;
}

int filed_write_stat_from_backend(
    filed_statx_t *out,
    const storage_statx_reply_t *stat,
    uint64_t handle_id,
    uint64_t object_generation,
    uint64_t dir_generation)
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
    out->atime_sec = stat->atime_sec;
    out->atime_nsec = stat->atime_nsec;
    out->mtime_sec = stat->mtime_sec;
    out->mtime_nsec = stat->mtime_nsec;
    out->ctime_sec = stat->ctime_sec;
    out->ctime_nsec = stat->ctime_nsec;
    out->object_generation = object_generation;
    out->dir_generation = dir_generation;
    out->rdev = stat->rdev;
    return 0;
}

filed_vfs_stat_snapshot_t filed_stat_snapshot_from_backend(
    const storage_statx_reply_t *stat,
    uint64_t handle_id,
    uint64_t object_generation,
    uint64_t dir_generation)
{
    filed_vfs_stat_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (stat == NULL) {
        return snapshot;
    }
    snapshot.valid = true;
    snapshot.handle_id = handle_id;
    snapshot.mode = stat->mode;
    snapshot.size = stat->size;
    snapshot.blocks = stat->blocks;
    snapshot.nlink = stat->nlink;
    snapshot.kind = stat->kind;
    snapshot.rdev = stat->rdev;
    snapshot.times_valid = true;
    snapshot.atime_sec = stat->atime_sec;
    snapshot.atime_nsec = stat->atime_nsec;
    snapshot.mtime_sec = stat->mtime_sec;
    snapshot.mtime_nsec = stat->mtime_nsec;
    snapshot.ctime_sec = stat->ctime_sec;
    snapshot.ctime_nsec = stat->ctime_nsec;
    snapshot.object_generation = (filed_generation_t)object_generation;
    snapshot.dir_generation = (filed_generation_t)dir_generation;
    return snapshot;
}

filed_vfs_stat_snapshot_t filed_directory_snapshot_from_create(
    uint64_t handle_id,
    uint64_t mode,
    uint64_t object_generation,
    uint64_t dir_generation)
{
    filed_vfs_stat_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.valid = true;
    snapshot.handle_id = handle_id;
    snapshot.mode = 0040000u | (mode & 07777u);
    snapshot.size = 0;
    snapshot.blocks = 0;
    snapshot.nlink = 2;
    snapshot.kind = 0040000u;
    snapshot.times_valid = true;
    snapshot.object_generation = (filed_generation_t)object_generation;
    snapshot.dir_generation = (filed_generation_t)dir_generation;
    return snapshot;
}

filed_vfs_stat_snapshot_t filed_symlink_snapshot_from_create(
    uint64_t handle_id,
    uint64_t target_length,
    uint64_t object_generation,
    uint64_t dir_generation)
{
    filed_vfs_stat_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.valid = true;
    snapshot.handle_id = handle_id;
    snapshot.mode = 0120000u | 0777u;
    snapshot.size = target_length;
    snapshot.blocks = 0;
    snapshot.nlink = 1;
    snapshot.kind = 0120000u;
    snapshot.times_valid = true;
    snapshot.object_generation = (filed_generation_t)object_generation;
    snapshot.dir_generation = (filed_generation_t)dir_generation;
    return snapshot;
}

int filed_write_stat_from_snapshot(
    filed_statx_t *out,
    const filed_vfs_stat_snapshot_t *snapshot,
    uint64_t handle_id)
{
    if (out == NULL || snapshot == NULL || !snapshot->valid) {
        return -22;
    }
    memset(out, 0, sizeof(*out));
    out->handle = handle_id;
    out->mode = snapshot->mode;
    out->size = snapshot->size;
    out->blocks = snapshot->blocks;
    out->nlink = snapshot->nlink;
    out->kind = snapshot->kind;
    out->rdev = snapshot->rdev;
    if (snapshot->times_valid) {
        out->atime_sec = snapshot->atime_sec;
        out->atime_nsec = snapshot->atime_nsec;
        out->mtime_sec = snapshot->mtime_sec;
        out->mtime_nsec = snapshot->mtime_nsec;
        out->ctime_sec = snapshot->ctime_sec;
        out->ctime_nsec = snapshot->ctime_nsec;
    }
    out->object_generation = snapshot->object_generation;
    out->dir_generation = snapshot->dir_generation;
    return 0;
}

int filed_backend_object_for_handle(
    filed_runtime_t *runtime,
    filed_handle_id_t handle_id,
    filed_vfs_io_decision_t *out_decision)
{
    if (runtime == NULL || out_decision == NULL) {
        return -22;
    }
    filed_status_t status = filed_vfs_stat_prepare(&runtime->vfs, handle_id, out_decision);
    return filed_status_to_wire(status);
}

int filed_name_is_terminated(const char *name, size_t capacity)
{
    return name != NULL && memchr(name, '\0', capacity) != NULL;
}

enum {
    FILED_WALK_RIGHTS =
        FILED_RIGHT_LOOKUP |
        FILED_RIGHT_STAT |
        FILED_RIGHT_GETDENTS,
};

const char *filed_skip_slashes(const char *path)
{
    while (path != NULL && *path == '/') {
        ++path;
    }
    return path;
}

int filed_path_is_single_component(const char *path)
{
    path = filed_skip_slashes(path);
    if (path == NULL || *path == '\0') {
        return 0;
    }
    while (*path != '\0' && *path != '/') {
        ++path;
    }
    path = filed_skip_slashes(path);
    return path != NULL && *path == '\0';
}

int filed_path_component_is_tmp(const char *component, size_t len)
{
    return len == 3u &&
        component[0] == 't' &&
        component[1] == 'm' &&
        component[2] == 'p';
}

static int filed_path_component_is_run(const char *component, size_t len)
{
    return len == 3u &&
        component[0] == 'r' &&
        component[1] == 'u' &&
        component[2] == 'n';
}

void filed_close_walk_handle(
    filed_runtime_t *runtime,
    filed_handle_id_t handle_id,
    int owned)
{
    if (runtime != NULL &&
        owned &&
        handle_id != 0 &&
        handle_id != runtime->root_handle_id &&
        (!runtime->tmpfs_root_handle_valid || handle_id != runtime->tmpfs_root_handle_id) &&
        (!runtime->run_tmpfs_root_handle_valid || handle_id != runtime->run_tmpfs_root_handle_id))
    {
        (void)filed_close_handle_runtime_deferred(runtime, handle_id);
    }
}

int64_t filed_lookup_and_open_component(
    filed_runtime_t *runtime,
    filed_handle_id_t parent_handle,
    const char *name,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open);

int64_t filed_lookup_component_stat(
    filed_runtime_t *runtime,
    filed_handle_id_t parent_handle,
    const char *name,
    uint64_t *out_object_id,
    storage_statx_reply_t *out_stat,
    bool *out_lookup_owned)
{
    filed_vfs_io_decision_t parent_decision;
    filed_status_t status;
    int64_t reply_status;

    if (runtime == NULL || name == NULL || out_object_id == NULL ||
        out_stat == NULL || out_lookup_owned == NULL)
    {
        return filed_status_to_wire(FILED_ERR_INVALID);
    }
    *out_object_id = 0;
    *out_lookup_owned = false;
    memset(out_stat, 0, sizeof(*out_stat));
    status = filed_vfs_lookup_prepare(&runtime->vfs, parent_handle, &parent_decision);
    reply_status = filed_status_to_wire(status);
    if (status != FILED_OK) {
        return reply_status;
    }
    reply_status = filed_backend_lookup(runtime, parent_decision.backend_object, name, out_object_id);
    if (reply_status != 0) {
        return reply_status;
    }
    *out_lookup_owned = true;
    reply_status = filed_backend_statx(runtime, *out_object_id, out_stat);
    if (reply_status != 0) {
        (void)filed_backend_release_object(runtime, *out_object_id);
        *out_lookup_owned = false;
    }
    return reply_status;
}

int64_t filed_splice_symlink_target(
    filed_runtime_t *runtime,
    uint64_t object_id,
    const char *rest,
    char *out_path,
    size_t out_path_size)
{
    char target[FILED_SYMLINK_TARGET_BYTES];
    uint64_t target_length = 0;
    int64_t reply_status;
    size_t rest_length;

    if (runtime == NULL || object_id == 0 || rest == NULL || out_path == NULL || out_path_size == 0) {
        return filed_status_to_wire(FILED_ERR_INVALID);
    }
    memset(target, 0, sizeof(target));
    reply_status = filed_backend_readlink(
        runtime,
        object_id,
        target,
        sizeof(target) - 1u,
        &target_length);
    if (reply_status != 0) {
        return reply_status;
    }
    if (target_length == 0 || target_length >= sizeof(target)) {
        return filed_status_to_wire(FILED_ERR_INVALID);
    }
    target[target_length] = '\0';
    rest = filed_skip_slashes(rest);
    rest_length = strlen(rest);
    if (rest_length != 0) {
        if (target_length + 1u + rest_length >= out_path_size) {
            return filed_status_to_wire(FILED_ERR_INVALID);
        }
        /* rest may point into out_path while resolving a second symlink. */
        memmove(out_path + target_length + 1u, rest, rest_length + 1u);
        memcpy(out_path, target, (size_t)target_length);
        out_path[target_length] = '/';
    } else {
        if (target_length >= out_path_size) {
            return filed_status_to_wire(FILED_ERR_INVALID);
        }
        memset(out_path, 0, out_path_size);
        memcpy(out_path, target, (size_t)target_length + 1u);
    }
    return 0;
}

int64_t filed_resolve_parent_path(
    filed_runtime_t *runtime,
    filed_handle_id_t base_dir_handle,
    const char *path,
    uint32_t parent_rights,
    filed_handle_id_t *out_parent_handle,
    int *out_parent_owned,
    char *out_name,
    size_t out_name_size)
{
    int absolute;
    filed_handle_id_t current_handle;
    int current_owned = 0;
    unsigned int symlink_budget = 16;
    char symlink_path[FILED_PATH_BYTES];

    if (runtime == NULL ||
        path == NULL ||
        out_parent_handle == NULL ||
        out_parent_owned == NULL ||
        out_name == NULL ||
        out_name_size == 0 ||
        path[0] == '\0')
    {
        return filed_status_to_wire(FILED_ERR_INVALID);
    }

    absolute = path[0] == '/';
    current_handle =
        absolute || base_dir_handle == 0 ?
            runtime->root_handle_id :
            base_dir_handle;

    *out_parent_handle = 0;
    *out_parent_owned = 0;
    out_name[0] = '\0';

    if (absolute) {
        path = filed_skip_slashes(path);
        if (*path == '\0') {
            return filed_status_to_wire(FILED_ERR_INVALID);
        }
    }

    for (;;) {
        char component[FILED_NAME_BYTES];
        const char *component_start;
        const char *after_slashes;
        size_t component_len;
        int has_more;
        int trailing_slash;

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
        trailing_slash = *path == '/' && !has_more;

        memset(component, 0, sizeof(component));
        memcpy(component, component_start, component_len);

        filed_handle_id_t overlay_root_handle = 0;
        if (runtime->tmpfs_root_handle_valid &&
            filed_path_component_is_tmp(component, component_len))
        {
            overlay_root_handle = runtime->tmpfs_root_handle_id;
        } else if (runtime->run_tmpfs_root_handle_valid &&
            filed_path_component_is_run(component, component_len))
        {
            overlay_root_handle = runtime->run_tmpfs_root_handle_id;
        }
        if (current_handle == runtime->root_handle_id &&
            !current_owned &&
            has_more &&
            overlay_root_handle != 0)
        {
            current_handle = overlay_root_handle;
            path = after_slashes;
            continue;
        }

        if (component_len == 1 && component[0] == '.') {
            if (!has_more) {
                filed_close_walk_handle(runtime, current_handle, current_owned);
                return filed_status_to_wire(FILED_ERR_INVALID);
            }
            path = after_slashes;
            continue;
        }

        if (component_len == 2 && component[0] == '.' && component[1] == '.') {
            filed_vfs_open_result_t parent_open;
            filed_status_t status;
            uint32_t next_rights = FILED_WALK_RIGHTS;

            if (filed_path_is_single_component(after_slashes)) {
                next_rights |= parent_rights;
            }

            if (!has_more) {
                filed_close_walk_handle(runtime, current_handle, current_owned);
                return filed_status_to_wire(FILED_ERR_INVALID);
            }

            memset(&parent_open, 0, sizeof(parent_open));
            status = filed_vfs_open_parent(
                &runtime->vfs,
                current_handle,
                next_rights,
                FILED_OPEN_DIRECTORY,
                &parent_open);
            filed_close_walk_handle(runtime, current_handle, current_owned);
            if (status != FILED_OK) {
                return filed_status_to_wire(status);
            }
            current_handle = parent_open.handle_id;
            current_owned = 1;
            path = after_slashes;
            continue;
        }

        if (!has_more) {
            if (trailing_slash || component_len >= out_name_size) {
                filed_close_walk_handle(runtime, current_handle, current_owned);
                return filed_status_to_wire(FILED_ERR_INVALID);
            }
            memset(out_name, 0, out_name_size);
            memcpy(out_name, component, component_len);
            *out_parent_handle = current_handle;
            *out_parent_owned = current_owned;
            return 0;
        } else {
            filed_vfs_open_result_t next_open;
            uint32_t next_rights = FILED_WALK_RIGHTS;
            uint64_t object_id = 0;
            bool lookup_owned = false;
            bool component_is_symlink = false;
            storage_statx_reply_t stat;
            int64_t symlink_status;
            if (filed_path_is_single_component(after_slashes)) {
                next_rights |= parent_rights;
            }
            memset(&next_open, 0, sizeof(next_open));
            const filed_status_t cached_status = filed_vfs_open_cached_child(
                &runtime->vfs,
                current_handle,
                component,
                next_rights,
                FILED_OPEN_DIRECTORY,
                &next_open);
            if (cached_status == FILED_OK) {
                filed_close_walk_handle(runtime, current_handle, current_owned);
                current_handle = next_open.handle_id;
                current_owned = 1;
                path = after_slashes;
                continue;
            }
            if (cached_status != FILED_ERR_NOT_FOUND &&
                cached_status != FILED_ERR_NOT_DIR)
            {
                filed_close_walk_handle(runtime, current_handle, current_owned);
                return filed_status_to_wire(cached_status);
            }
            if (cached_status == FILED_ERR_NOT_DIR) {
                filed_vnode_kind_t cached_kind = 0;
                if (filed_vfs_cached_child_info(
                        &runtime->vfs,
                        current_handle,
                        component,
                        &object_id,
                        &cached_kind) == FILED_OK)
                {
                    if (cached_kind != FILED_VNODE_SYMLINK) {
                        filed_close_walk_handle(runtime, current_handle, current_owned);
                        return filed_status_to_wire(FILED_ERR_NOT_DIR);
                    }
                    component_is_symlink = true;
                }
            }
            if (!component_is_symlink) {
                symlink_status = filed_lookup_component_stat(
                    runtime,
                    current_handle,
                    component,
                    &object_id,
                    &stat,
                    &lookup_owned);
                if (symlink_status != 0) {
                    filed_close_walk_handle(runtime, current_handle, current_owned);
                    return symlink_status;
                }
                component_is_symlink =
                    filed_kind_from_unix_type(stat.kind) == FILED_VNODE_SYMLINK;
            }
            if (component_is_symlink) {
                if (symlink_budget == 0) {
                    if (lookup_owned) {
                        (void)filed_backend_release_object(runtime, object_id);
                    }
                    filed_close_walk_handle(runtime, current_handle, current_owned);
                    return filed_status_to_wire(FILED_ERR_LOOP);
                }
                --symlink_budget;
                symlink_status = filed_splice_symlink_target(
                    runtime,
                    object_id,
                    after_slashes,
                    symlink_path,
                    sizeof(symlink_path));
                if (lookup_owned) {
                    (void)filed_backend_release_object(runtime, object_id);
                }
                if (symlink_status != 0) {
                    filed_close_walk_handle(runtime, current_handle, current_owned);
                    return symlink_status;
                }
                if (symlink_path[0] == '/') {
                    filed_close_walk_handle(runtime, current_handle, current_owned);
                    current_handle = runtime->root_handle_id;
                    current_owned = 0;
                }
                path = symlink_path;
                continue;
            }
            if (lookup_owned) {
                (void)filed_backend_release_object(runtime, object_id);
            }
            const int64_t reply_status = filed_lookup_and_open_component(
                runtime,
                current_handle,
                component,
                next_rights,
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

int64_t filed_lookup_and_open_component(
    filed_runtime_t *runtime,
    filed_handle_id_t parent_handle,
    const char *name,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open)
{
    filed_vfs_io_decision_t parent_decision;
    uint64_t object_id = 0;
    storage_statx_reply_t backend_stat;
    filed_status_t status;
    int64_t reply_status;
    bool lookup_acquired = false;
    bool existing_vnode_owns_object = false;
    const bool can_use_negative_lookup_cache =
        (open_flags & (FILED_OPEN_CREATE | FILED_OPEN_EXCLUSIVE | FILED_OPEN_TRUNCATE)) == 0;

    if (can_use_negative_lookup_cache) {
        status = filed_vfs_open_cached_child(
            &runtime->vfs,
            parent_handle,
            name,
            rights,
            open_flags,
            out_open);
        if (status == FILED_OK) {
            return 0;
        }
        if (status != FILED_ERR_NOT_FOUND) {
            return filed_status_to_wire(status);
        }
    }

    status = filed_vfs_lookup_prepare(&runtime->vfs, parent_handle, &parent_decision);
    reply_status = filed_status_to_wire(status);
    if (status != FILED_OK) {
        return reply_status;
    }

    if (can_use_negative_lookup_cache &&
        filed_negative_lookup_cache_get(runtime, 
            parent_decision.backend_object,
            parent_decision.dir_generation,
            name,
            &reply_status))
    {
        return reply_status;
    }

    reply_status = filed_backend_lookup(
        runtime,
        parent_decision.backend_object,
        name,
        &object_id);
    if (reply_status != 0 &&
        (open_flags & FILED_OPEN_CREATE) != 0)
    {
        reply_status = filed_backend_create(
            runtime,
            parent_decision.backend_object,
            name,
            0100644u,
            &object_id);
        if (reply_status != 0) {
            return reply_status;
        }
        lookup_acquired = true;
        filed_cache_invalidate(runtime, parent_decision.backend_object);
        memset(&backend_stat, 0, sizeof(backend_stat));
        reply_status = filed_backend_statx(
            runtime,
            object_id,
            &backend_stat);
        if (reply_status != 0) {
            (void)filed_backend_release_object(runtime, object_id);
            return reply_status;
        }
        status = filed_vfs_create_backend_child(
            &runtime->vfs,
            parent_handle,
            object_id,
            filed_kind_from_unix_type(backend_stat.kind),
            name,
            rights,
            open_flags,
            out_open);
        if (status == FILED_OK) {
            const filed_vfs_stat_snapshot_t snapshot =
                filed_stat_snapshot_from_backend(
                    &backend_stat,
                    out_open->handle_id,
                    out_open->object_generation,
                    out_open->dir_generation);
            (void)filed_vfs_update_stat_snapshot(
                &runtime->vfs,
                object_id,
                &snapshot);
        }
        if (status != FILED_OK) {
            (void)filed_backend_release_object(runtime, object_id);
        }
        return filed_status_to_wire(status);
    }
    if (reply_status != 0) {
        if (can_use_negative_lookup_cache && reply_status == filed_status_to_wire(FILED_ERR_NOT_FOUND)) {
            filed_negative_lookup_cache_store(runtime, 
                parent_decision.backend_object,
                parent_decision.dir_generation,
                name,
                reply_status);
        }
        return reply_status;
    }
    lookup_acquired = true;
    if ((open_flags & (FILED_OPEN_CREATE | FILED_OPEN_EXCLUSIVE)) ==
        (FILED_OPEN_CREATE | FILED_OPEN_EXCLUSIVE))
    {
        (void)filed_backend_release_object(runtime, object_id);
        return filed_status_to_wire(FILED_ERR_EXISTS);
    }

    memset(&backend_stat, 0, sizeof(backend_stat));
    reply_status = filed_backend_statx(
        runtime,
        object_id,
        &backend_stat);
    if (reply_status != 0) {
        (void)filed_backend_release_object(runtime, object_id);
        return reply_status;
    }

    filed_backend_object_id_t cached_object = 0;
    existing_vnode_owns_object =
        filed_vfs_cached_child_backend_object(
            &runtime->vfs,
            parent_handle,
            name,
            &cached_object) == FILED_OK &&
        cached_object == object_id;

    status = filed_vfs_open_backend_child(
        &runtime->vfs,
        parent_handle,
        object_id,
        filed_kind_from_unix_type(backend_stat.kind),
        name,
        rights,
        open_flags,
        out_open);
    if (lookup_acquired && (status != FILED_OK || existing_vnode_owns_object)) {
        (void)filed_backend_release_object(runtime, object_id);
    }
    if (status == FILED_OK) {
        filed_vfs_stat_snapshot_t current_snapshot;
        memset(&current_snapshot, 0, sizeof(current_snapshot));
        if (filed_cache_object_dirty(runtime, object_id) &&
            filed_vfs_get_stat_snapshot(
                &runtime->vfs,
                out_open->handle_id,
                &current_snapshot) == FILED_OK &&
            current_snapshot.valid)
        {
            out_open->object_generation = current_snapshot.object_generation;
            out_open->dir_generation = current_snapshot.dir_generation;
        } else {
            const filed_vfs_stat_snapshot_t snapshot =
                filed_stat_snapshot_from_backend(
                    &backend_stat,
                    out_open->handle_id,
                    out_open->object_generation,
                    out_open->dir_generation);
            (void)filed_vfs_update_stat_snapshot(
                &runtime->vfs,
                object_id,
                &snapshot);
        }
    }
    if (status == FILED_OK &&
        (open_flags & FILED_OPEN_TRUNCATE) != 0 &&
        (backend_stat.kind & 0170000u) == 0100000u)
    {
        reply_status = filed_cache_flush_object(runtime, object_id);
        if (reply_status != 0) {
            (void)filed_vfs_close_handle(&runtime->vfs, out_open->handle_id);
            memset(out_open, 0, sizeof(*out_open));
            return reply_status;
        }
        reply_status = filed_backend_truncate(
            runtime,
            object_id,
            0);
        if (reply_status != 0) {
            (void)filed_vfs_close_handle(&runtime->vfs, out_open->handle_id);
            memset(out_open, 0, sizeof(*out_open));
            return reply_status;
        }
        filed_cache_invalidate(runtime, object_id);
        (void)filed_vfs_note_truncate(&runtime->vfs, out_open->handle_id, 0);
        {
            filed_vfs_stat_snapshot_t snapshot;
            memset(&snapshot, 0, sizeof(snapshot));
            if (filed_vfs_get_stat_snapshot(&runtime->vfs, out_open->handle_id, &snapshot) == FILED_OK) {
                out_open->object_generation = snapshot.object_generation;
                out_open->dir_generation = snapshot.dir_generation;
            }
        }
    }
    return filed_status_to_wire(status);
}

int64_t filed_openat_path(
    filed_runtime_t *runtime,
    const filed_openat_t *openat,
    filed_vfs_open_result_t *out_open)
{
    const uint32_t rights = filed_rights_to_vfs(openat->rights);
    const uint32_t open_flags = filed_open_flags_to_vfs(openat->open_flags);
    const char *path = openat->name;
    const int absolute = path[0] == '/';
    filed_handle_id_t current_handle =
        absolute || openat->dir_handle == 0 ?
            runtime->root_handle_id :
            (filed_handle_id_t)(uint32_t)openat->dir_handle;
    int current_owned = 0;
    unsigned int symlink_budget = 16;
    char symlink_path[FILED_PATH_BYTES];

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
        char component[FILED_NAME_BYTES];
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
            uint32_t next_rights = FILED_WALK_RIGHTS;
            uint64_t object_id = 0;
            bool lookup_owned = false;
            bool component_is_symlink = false;
            storage_statx_reply_t stat;
            int64_t symlink_status;
            if ((open_flags & FILED_OPEN_CREATE) != 0 &&
                filed_path_is_single_component(after_slashes))
            {
                next_rights |= FILED_RIGHT_CREATE;
            }
            memset(&next_open, 0, sizeof(next_open));
            const filed_status_t cached_status = filed_vfs_open_cached_child(
                &runtime->vfs,
                current_handle,
                component,
                next_rights,
                FILED_OPEN_DIRECTORY,
                &next_open);
            if (cached_status == FILED_OK) {
                filed_close_walk_handle(runtime, current_handle, current_owned);
                current_handle = next_open.handle_id;
                current_owned = 1;
                path = after_slashes;
                continue;
            }
            if (cached_status != FILED_ERR_NOT_FOUND &&
                cached_status != FILED_ERR_NOT_DIR)
            {
                filed_close_walk_handle(runtime, current_handle, current_owned);
                return filed_status_to_wire(cached_status);
            }
            if (cached_status == FILED_ERR_NOT_DIR) {
                filed_vnode_kind_t cached_kind = 0;
                if (filed_vfs_cached_child_info(
                        &runtime->vfs,
                        current_handle,
                        component,
                        &object_id,
                        &cached_kind) == FILED_OK)
                {
                    if (cached_kind != FILED_VNODE_SYMLINK) {
                        filed_close_walk_handle(runtime, current_handle, current_owned);
                        return filed_status_to_wire(FILED_ERR_NOT_DIR);
                    }
                    component_is_symlink = true;
                }
            }
            if (!component_is_symlink) {
                symlink_status = filed_lookup_component_stat(
                    runtime,
                    current_handle,
                    component,
                    &object_id,
                    &stat,
                    &lookup_owned);
                if (symlink_status != 0) {
                    filed_close_walk_handle(runtime, current_handle, current_owned);
                    return symlink_status;
                }
                component_is_symlink =
                    filed_kind_from_unix_type(stat.kind) == FILED_VNODE_SYMLINK;
            }
            if (component_is_symlink) {
                if (symlink_budget == 0) {
                    if (lookup_owned) {
                        (void)filed_backend_release_object(runtime, object_id);
                    }
                    filed_close_walk_handle(runtime, current_handle, current_owned);
                    return filed_status_to_wire(FILED_ERR_LOOP);
                }
                --symlink_budget;
                symlink_status = filed_splice_symlink_target(
                    runtime,
                    object_id,
                    after_slashes,
                    symlink_path,
                    sizeof(symlink_path));
                if (lookup_owned) {
                    (void)filed_backend_release_object(runtime, object_id);
                }
                if (symlink_status != 0) {
                    filed_close_walk_handle(runtime, current_handle, current_owned);
                    return symlink_status;
                }
                if (symlink_path[0] == '/') {
                    filed_close_walk_handle(runtime, current_handle, current_owned);
                    current_handle = runtime->root_handle_id;
                    current_owned = 0;
                }
                path = symlink_path;
                continue;
            }
            if (lookup_owned) {
                (void)filed_backend_release_object(runtime, object_id);
            }
            const int64_t reply_status = filed_lookup_and_open_component(
                runtime,
                current_handle,
                component,
                next_rights,
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

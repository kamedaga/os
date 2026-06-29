#include "internal.h"

#include <stdlib.h>
#include <string.h>

#include "filed/page_cache.h"

static const char *skip_slashes(const char *path)
{
    while (path != NULL && *path == '/') {
        ++path;
    }
    return path;
}

static filed_vnode_kind_t kind_from_unix_type(uint64_t kind)
{
    switch (kind & 0170000u) {
    case 0040000u:
        return FILED_VNODE_DIRECTORY;
    case 0120000u:
        return FILED_VNODE_SYMLINK;
    case 0010000u:
        return FILED_VNODE_FIFO;
    case 0100000u:
    default:
        return FILED_VNODE_REGULAR;
    }
}

static void close_walk_handle(filed_runtime_t *runtime, filed_handle_id_t handle_id, int owned)
{
    if (runtime != NULL && owned && handle_id != 0 && handle_id != runtime->root_handle_id) {
        (void)filed_vfs_close_handle(&runtime->vfs, handle_id);
    }
}

static int lookup_and_open_component(
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

    if (runtime == NULL || name == NULL || out_open == NULL) {
        return -22;
    }
    filed_status_t status = filed_vfs_lookup_prepare(&runtime->vfs, parent_handle, &parent_decision);
    if (status != FILED_OK) {
        return lpr_exec_status_to_errno(status);
    }
    int result = filed_kobox_backend_lookup(&runtime->backend, parent_decision.backend_object, name, &object_id);
    if (result != 0) {
        return result;
    }
    memset(&backend_stat, 0, sizeof(backend_stat));
    result = filed_kobox_backend_statx(&runtime->backend, object_id, &backend_stat);
    if (result != 0) {
        return result;
    }
    status = filed_vfs_open_backend_child(
        &runtime->vfs,
        parent_handle,
        object_id,
        kind_from_unix_type(backend_stat.kind),
        name,
        rights,
        open_flags,
        out_open);
    return lpr_exec_status_to_errno(status);
}

static int open_absolute_path(
    filed_runtime_t *runtime,
    const char *path,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open)
{
    filed_handle_id_t current_handle;
    int current_owned = 0;

    if (runtime == NULL || path == NULL || path[0] != '/' || out_open == NULL) {
        return -22;
    }
    path = skip_slashes(path);
    if (*path == '\0') {
        return -22;
    }
    memset(out_open, 0, sizeof(*out_open));
    current_handle = runtime->root_handle_id;

    for (;;) {
        char component[FILED_WIRE_NAME_BYTES];
        const char *component_start;
        const char *after_slashes;
        size_t component_len;
        int has_more;
        int require_directory;
        int final_component;

        path = skip_slashes(path);
        if (*path == '\0') {
            close_walk_handle(runtime, current_handle, current_owned);
            return -22;
        }
        component_start = path;
        while (*path != '\0' && *path != '/') {
            ++path;
        }
        component_len = (size_t)(path - component_start);
        if (component_len == 0 || component_len >= sizeof(component)) {
            close_walk_handle(runtime, current_handle, current_owned);
            return -22;
        }
        after_slashes = skip_slashes(path);
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
                close_walk_handle(runtime, current_handle, current_owned);
                return lpr_exec_status_to_errno(status);
            }
            path = after_slashes;
            continue;
        }

        if (component_len == 2 && component[0] == '.' && component[1] == '.') {
            filed_vfs_open_result_t parent_open;
            const uint32_t next_rights = final_component ? rights : LPR_EXEC_WALK_RIGHTS;
            const uint32_t next_flags =
                final_component ? (open_flags | (require_directory ? FILED_OPEN_DIRECTORY : 0)) : FILED_OPEN_DIRECTORY;
            memset(&parent_open, 0, sizeof(parent_open));
            const filed_status_t status = filed_vfs_open_parent(
                &runtime->vfs,
                current_handle,
                next_rights,
                next_flags,
                &parent_open);
            close_walk_handle(runtime, current_handle, current_owned);
            if (status != FILED_OK) {
                return lpr_exec_status_to_errno(status);
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
            const int result = lookup_and_open_component(
                runtime,
                current_handle,
                component,
                rights,
                open_flags | (require_directory ? FILED_OPEN_DIRECTORY : 0),
                out_open);
            close_walk_handle(runtime, current_handle, current_owned);
            return result;
        }

        filed_vfs_open_result_t next_open;
        const int result = lookup_and_open_component(
            runtime,
            current_handle,
            component,
            LPR_EXEC_WALK_RIGHTS,
            FILED_OPEN_DIRECTORY,
            &next_open);
        close_walk_handle(runtime, current_handle, current_owned);
        if (result != 0) {
            return result;
        }
        current_handle = next_open.handle_id;
        current_owned = 1;
        path = after_slashes;
    }
}

static int read_range(
    filed_runtime_t *runtime,
    filed_handle_id_t handle_id,
    uint64_t backend_object,
    uint64_t file_size,
    uint64_t offset,
    unsigned char *buffer,
    uint64_t length)
{
    filed_vfs_io_decision_t read_decision;
    uint64_t got = 0;

    if (runtime == NULL || buffer == NULL || offset > file_size || length > file_size - offset) {
        return -22;
    }
    if (length == 0) {
        return 0;
    }
    const filed_status_t status = filed_vfs_pread_prepare(&runtime->vfs, handle_id, offset, length, &read_decision);
    if (status != FILED_OK) {
        return -13;
    }
    if (read_decision.backend_object != backend_object) {
        return -13;
    }
    const int result = filed_cached_pread(runtime, backend_object, read_decision.offset, buffer, read_decision.length, &got);
    if (result != 0 || got != length) {
        return result != 0 ? result : -5;
    }
    return 0;
}

int lpr_exec_read_full_image(filed_runtime_t *runtime, filed_handle_id_t handle_id, lpr_exec_image_t *out_image)
{
    filed_vfs_io_decision_t stat_decision;
    filed_vfs_stat_snapshot_t snapshot;
    koboxd_wire_fs_statx_t stat;

    if (runtime == NULL || out_image == NULL) {
        return -22;
    }
    memset(out_image, 0, sizeof(*out_image));
    filed_status_t vfs_status = filed_vfs_stat_prepare(&runtime->vfs, handle_id, &stat_decision);
    if (vfs_status != FILED_OK) {
        return -13;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    vfs_status = filed_vfs_get_stat_snapshot(&runtime->vfs, handle_id, &snapshot);
    if (vfs_status != FILED_OK) {
        return -13;
    }
    memset(&stat, 0, sizeof(stat));
    if (snapshot.valid) {
        stat.mode = snapshot.mode;
        stat.size = snapshot.size;
        stat.blocks = snapshot.blocks;
        stat.nlink = snapshot.nlink;
        stat.kind = snapshot.kind;
    } else {
        const int status = filed_kobox_backend_statx(&runtime->backend, stat_decision.backend_object, &stat);
        if (status != 0) {
            return status;
        }
        snapshot.valid = true;
        snapshot.mode = stat.mode;
        snapshot.size = stat.size;
        snapshot.blocks = stat.blocks;
        snapshot.nlink = stat.nlink;
        snapshot.kind = stat.kind;
        (void)filed_vfs_update_stat_snapshot(&runtime->vfs, stat_decision.backend_object, &snapshot);
    }
    if ((stat.kind & 0170000u) != 0100000u ||
        stat.size < LPR_EXEC_EHDR_BYTES ||
        stat.size > LPR_EXEC_MAX_IMAGE_BYTES)
    {
        return -8;
    }
    unsigned char *image = malloc((size_t)stat.size);
    if (image == NULL) {
        return -12;
    }
    const int status = read_range(runtime, handle_id, stat_decision.backend_object, stat.size, 0, image, stat.size);
    if (status != 0) {
        free(image);
        return status;
    }
    out_image->bytes = image;
    out_image->size = stat.size;
    return 0;
}

int lpr_exec_read_absolute_image(filed_runtime_t *runtime, const char *path, lpr_exec_image_t *out_image)
{
    filed_vfs_open_result_t open_result;
    if (runtime == NULL || path == NULL || out_image == NULL) {
        return -22;
    }
    memset(&open_result, 0, sizeof(open_result));
    int status = open_absolute_path(
        runtime,
        path,
        FILED_RIGHT_READ | FILED_RIGHT_EXEC | FILED_RIGHT_STAT,
        0,
        &open_result);
    if (status != 0) {
        return status;
    }
    status = lpr_exec_read_full_image(runtime, open_result.handle_id, out_image);
    (void)filed_vfs_close_handle(&runtime->vfs, open_result.handle_id);
    return status;
}

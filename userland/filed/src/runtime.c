#include "filed/runtime.h"

#include <string.h>

#include "filed/dispatch.h"
#include "filed/fd_ipc.h"
#include "pacha/abi.h"
#include "pacha/ipc.h"

static int filed_clear_inherit_flag(int fd)
{
    if (fd < 16) {
        return -1;
    }
    const long status = pacha_fd_fcntl(
        fd,
        PACHA_FD_FCNTL_SET_FLAGS,
        0,
        PACHA_FD_FLAG_INHERIT);
    return status == 0 ? 0 : -2;
}

static int filed_find_bootstrap_fd(char **argv, int *out_fd)
{
    if (argv == NULL || out_fd == NULL) {
        return -1;
    }

    *out_fd = -1;
    char **p = argv;
    while (*p != NULL) {
        ++p;
    }
    ++p;
    while (*p != NULL) {
        ++p;
    }
    ++p;

    const uint64_t *auxv = (const uint64_t *)(const void *)p;
    for (unsigned int i = 0; i < 64; ++i) {
        const uint64_t type = auxv[i * 2u];
        const uint64_t value = auxv[i * 2u + 1u];
        if (type == 0) {
            break;
        }
        if (type == PACHA_AT_BOOTSTRAP_FD) {
            if (value < 16) {
                return -2;
            }
            *out_fd = (int)value;
            return 0;
        }
    }

    return -2;
}

static int filed_read_bootstrap_fd(int fd, filed_bootstrap_t *out_bootstrap)
{
    if (fd < 16 || out_bootstrap == NULL) {
        return -1;
    }

    const long got = pacha_fd_read(fd, out_bootstrap, sizeof(*out_bootstrap));
    return got == (long)sizeof(*out_bootstrap) ? 0 : -2;
}

static int filed_validate_bootstrap(const filed_bootstrap_t *bootstrap)
{
    if (bootstrap == NULL) {
        return -1;
    }
    if (bootstrap->magic != FILED_BOOTSTRAP_MAGIC ||
        bootstrap->fs_backend_fd < 16 ||
        bootstrap->public_endpoint_fd < 16)
    {
        return -2;
    }
    return 0;
}

void filed_runtime_init(filed_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->bootstrap_fd = -1;
    runtime->client_endpoint_fd = -1;
    for (uint64_t i = 0; i < FILED_RUNTIME_MAX_SESSIONS; ++i) {
        runtime->sessions[i].channel_fd = -1;
        runtime->sessions[i].page_fd = -1;
    }
    filed_vfs_init(&runtime->vfs);
    filed_kobox_backend_init(&runtime->backend, -1);
    filed_tmpfs_backend_init(&runtime->tmpfs);
}

int filed_runtime_bootstrap(filed_runtime_t *runtime, char **argv)
{
    int status;

    if (runtime == NULL) {
        return -1;
    }

    status = filed_find_bootstrap_fd(argv, &runtime->bootstrap_fd);
    if (status != 0) {
        return status;
    }

    status = filed_read_bootstrap_fd(runtime->bootstrap_fd, &runtime->bootstrap);
    if (status != 0) {
        return status;
    }

    status = filed_validate_bootstrap(&runtime->bootstrap);
    if (status != 0) {
        return status;
    }

    filed_kobox_backend_init(
        &runtime->backend,
        (int)(uint32_t)runtime->bootstrap.fs_backend_fd);
    runtime->client_endpoint_fd = (int)(uint32_t)runtime->bootstrap.public_endpoint_fd;
    status = filed_clear_inherit_flag(runtime->bootstrap_fd);
    if (status != 0) {
        return status;
    }
    status = filed_clear_inherit_flag(runtime->backend.fs_fd);
    if (status != 0) {
        return status;
    }
    status = filed_clear_inherit_flag(runtime->client_endpoint_fd);
    if (status != 0) {
        return status;
    }

    return 0;
}

int filed_runtime_mount_root(filed_runtime_t *runtime)
{
    koboxd_wire_fs_statx_t root_stat;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root_open;
    filed_status_t vfs_status;

    if (runtime == NULL) {
        return -1;
    }

    int status = filed_kobox_backend_mount_root(&runtime->backend);
    if (status != 0) {
        return status;
    }

    memset(&root_stat, 0, sizeof(root_stat));
    status = filed_kobox_backend_statx(
        &runtime->backend,
        runtime->backend.root_object_id,
        &root_stat);
    if (status != 0) {
        return status;
    }
    runtime->root_size = root_stat.size;

    vfs_status = filed_vfs_mount_root(
        &runtime->vfs,
        FILED_FS_EXT4,
        (filed_backend_id_t)(uint32_t)runtime->backend.fs_fd,
        runtime->backend.root_object_id,
        &root_mount);
    if (vfs_status != FILED_OK) {
        return -20 - (int)vfs_status;
    }
    {
        filed_vfs_stat_snapshot_t root_snapshot;
        memset(&root_snapshot, 0, sizeof(root_snapshot));
        root_snapshot.valid = true;
        root_snapshot.mode = root_stat.mode;
        root_snapshot.size = root_stat.size;
        root_snapshot.blocks = root_stat.blocks;
        root_snapshot.nlink = root_stat.nlink;
        root_snapshot.kind = root_stat.kind;
        root_snapshot.times_valid = true;
        root_snapshot.atime_sec = root_stat.atime_sec;
        root_snapshot.atime_nsec = root_stat.atime_nsec;
        root_snapshot.mtime_sec = root_stat.mtime_sec;
        root_snapshot.mtime_nsec = root_stat.mtime_nsec;
        root_snapshot.ctime_sec = root_stat.ctime_sec;
        root_snapshot.ctime_nsec = root_stat.ctime_nsec;
        (void)filed_vfs_update_stat_snapshot(
            &runtime->vfs,
            runtime->backend.root_object_id,
            &root_snapshot);
    }
    runtime->root_mount_id = root_mount;

    memset(&root_open, 0, sizeof(root_open));
    vfs_status = filed_vfs_open_root(
        &runtime->vfs,
        root_mount,
            FILED_RIGHT_LOOKUP |
            FILED_RIGHT_READ |
            FILED_RIGHT_EXEC |
            FILED_RIGHT_STAT |
            FILED_RIGHT_GETDENTS |
            FILED_RIGHT_CREATE |
            FILED_RIGHT_REMOVE |
            FILED_RIGHT_RENAME,
        FILED_OPEN_DIRECTORY,
        &root_open);
    if (vfs_status != FILED_OK) {
        return -30 - (int)vfs_status;
    }
    runtime->root_handle_id = root_open.handle_id;

    {
        uint64_t tmpfs_root = 0;
        uint64_t root_tmp_object = 0;
        filed_vfs_open_result_t tmp_open;
        memset(&tmp_open, 0, sizeof(tmp_open));
        status = filed_tmpfs_backend_mount_root(&runtime->tmpfs, &tmpfs_root);
        if (status != 0) {
            return status;
        }
        runtime->root_tmpfs_synthetic_dirent =
            filed_kobox_backend_lookup(
                &runtime->backend,
                runtime->backend.root_object_id,
                "tmp",
                &root_tmp_object) == 0 ? 0u : 1u;
        vfs_status = filed_vfs_open_backend_child(
            &runtime->vfs,
            runtime->root_handle_id,
            tmpfs_root,
            FILED_VNODE_DIRECTORY,
            "tmp",
            FILED_RIGHT_LOOKUP |
                FILED_RIGHT_READ |
                FILED_RIGHT_EXEC |
                FILED_RIGHT_STAT |
                FILED_RIGHT_GETDENTS |
                FILED_RIGHT_CREATE |
                FILED_RIGHT_REMOVE |
                FILED_RIGHT_RENAME,
            FILED_OPEN_DIRECTORY,
            &tmp_open);
        if (vfs_status != FILED_OK) {
            return -50 - (int)vfs_status;
        }
        runtime->tmpfs_root_handle_id = tmp_open.handle_id;
        runtime->tmpfs_root_handle_valid = 1u;
        {
            koboxd_wire_fs_statx_t tmp_stat;
            filed_vfs_stat_snapshot_t tmp_snapshot;
            memset(&tmp_stat, 0, sizeof(tmp_stat));
            status = filed_tmpfs_backend_statx(&runtime->tmpfs, tmpfs_root, &tmp_stat);
            if (status != 0) {
                return status;
            }
            memset(&tmp_snapshot, 0, sizeof(tmp_snapshot));
            tmp_snapshot.valid = true;
            tmp_snapshot.handle_id = tmp_open.handle_id;
            tmp_snapshot.mode = tmp_stat.mode;
            tmp_snapshot.size = tmp_stat.size;
            tmp_snapshot.blocks = tmp_stat.blocks;
            tmp_snapshot.nlink = tmp_stat.nlink;
            tmp_snapshot.kind = tmp_stat.kind;
            tmp_snapshot.times_valid = true;
            tmp_snapshot.object_generation = tmp_open.object_generation;
            tmp_snapshot.dir_generation = tmp_open.dir_generation;
            (void)filed_vfs_update_stat_snapshot(
                &runtime->vfs,
                tmpfs_root,
                &tmp_snapshot);
        }
    }

    vfs_status = filed_vfs_check_basic(&runtime->vfs);
    if (vfs_status != FILED_OK) {
        return -40 - (int)vfs_status;
    }

    return 0;
}

int filed_runtime_serve(filed_runtime_t *runtime)
{
    if (runtime == NULL || runtime->client_endpoint_fd < 16) {
        return -1;
    }

    for (;;) {
        struct pacha_pollfd fds[1 + FILED_RUNTIME_MAX_SESSIONS];
        uint64_t session_indices[FILED_RUNTIME_MAX_SESSIONS];
        uint64_t count = 0;
        fds[count++] = (struct pacha_pollfd){
            .fd = runtime->client_endpoint_fd,
            .events = PACHA_FD_EVENT_READABLE,
            .revents = 0,
        };
        for (uint64_t i = 0; i < FILED_RUNTIME_MAX_SESSIONS; ++i) {
            if (!runtime->sessions[i].active) {
                continue;
            }
            fds[count++] = (struct pacha_pollfd){
                .fd = runtime->sessions[i].channel_fd,
                .events = PACHA_FD_EVENT_READABLE,
                .revents = 0,
            };
            session_indices[count - 2u] = i;
        }

        const long wait_status = pacha_fd_wait_many(fds, count, PACHA_FD_WAIT_FOREVER);
        if (wait_status < 0) {
            return (int)wait_status;
        }

        if ((fds[0].revents & (PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_ERROR | PACHA_FD_EVENT_HANGUP)) != 0) {
            const int status = filed_dispatch_client_once(runtime, runtime->client_endpoint_fd);
            if (status != 0 &&
                status != PACHA_ERR_EMPTY &&
                status != PACHA_ERR_NOT_READY &&
                status != -2)
            {
                return status;
            }
        }

        for (uint64_t pos = 1; pos < count; ++pos) {
            if ((fds[pos].revents & (PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_ERROR | PACHA_FD_EVENT_HANGUP)) == 0) {
                continue;
            }
            const uint64_t session_index = session_indices[pos - 1u];
            const int status = filed_dispatch_session_once(runtime, session_index);
            if (status != 0 &&
                status != PACHA_ERR_EMPTY &&
                status != PACHA_ERR_NOT_READY &&
                status != -2)
            {
                return status;
            }
        }
    }
}

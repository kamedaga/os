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
    filed_vfs_init(&runtime->vfs);
    filed_kobox_backend_init(&runtime->backend, -1);
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
        const int status = filed_dispatch_client_once(runtime, runtime->client_endpoint_fd);
        if (status == 0 ||
            status == PACHA_ERR_EMPTY ||
            status == PACHA_ERR_NOT_READY ||
            status == -2)
        {
            continue;
        }
        return status;
    }
}

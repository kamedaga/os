#include "vfs_core.h"

#include "koboxd/ipc_protocol.h"
#include "pacha/ipc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int recv_ipc_wait(int fd, struct pacha_ipc_msg *msg)
{
    if (fd < 16 || msg == NULL) {
        return -1;
    }
    for (unsigned i = 0; i < 262144; i++) {
        const int status = pacha_ipc_recv(fd, msg);
        if (status == 0) {
            return 0;
        }
        if (status != PACHA_ERR_EMPTY && status != PACHA_ERR_NOT_READY && status != -2) {
            return status;
        }
        struct pacha_pollfd pollfd = {
            .fd = fd,
            .events = PACHA_FD_EVENT_READABLE,
            .revents = 0,
        };
        (void)pacha_fd_wait_many(&pollfd, 1, 1);
    }
    return -2;
}

static int send_ipc_wait(int fd, const struct pacha_ipc_msg *msg)
{
    if (fd < 16 || msg == NULL) {
        return -1;
    }
    for (unsigned i = 0; i < 262144; i++) {
        const int status = pacha_ipc_send(fd, msg);
        if (status == 0) {
            return 0;
        }
        if (status != PACHA_ERR_EMPTY && status != PACHA_ERR_NOT_READY && status != -2) {
            return status;
        }
        struct pacha_pollfd pollfd = {
            .fd = fd,
            .events = PACHA_FD_EVENT_WRITABLE,
            .revents = 0,
        };
        (void)pacha_fd_wait_many(&pollfd, 1, 1);
    }
    return -2;
}

static int fs_backend_call(int fs_fd, uint64_t op, uint64_t object_id, uint64_t *out_result)
{
    if (fs_fd < 16 || out_result == NULL) {
        return -1;
    }
    const struct pacha_ipc_msg request = {
        .word0 = KOBOXD_WIRE_ENDPOINT_MAGIC,
        .word1 = op,
        .word2 = object_id,
        .word3 = KOBOXD_WIRE_VERSION,
    };
    int status = send_ipc_wait(fs_fd, &request);
    if (status != 0) {
        return status;
    }

    struct pacha_ipc_msg reply;
    for (unsigned attempt = 0; attempt < 128; attempt++) {
        memset(&reply, 0, sizeof(reply));
        status = recv_ipc_wait(fs_fd, &reply);
        if (status != 0) {
            return status;
        }
        if (reply.word0 == KOBOXD_WIRE_REPLY_MAGIC && reply.word3 == KOBOXD_WIRE_VERSION) {
            break;
        }
        if (attempt == 127) {
            return -2;
        }
    }
    if ((int64_t)reply.word1 < 0) {
        return (int)(int64_t)reply.word1;
    }
    *out_result = reply.word2;
    return 0;
}

static filed_vnode_kind_t vnode_kind_from_mode(uint64_t mode)
{
    switch (mode & 0170000u) {
    case 0040000u: return FILED_VNODE_KIND_DIR;
    case 0100000u: return FILED_VNODE_KIND_REG;
    case 0120000u: return FILED_VNODE_KIND_SYMLINK;
    case 0020000u:
    case 0060000u: return FILED_VNODE_KIND_DEVICE;
    default: return FILED_VNODE_KIND_UNKNOWN;
    }
}

void filed_vfs_init(filed_vfs_t *vfs)
{
    if (vfs == NULL) {
        return;
    }
    memset(vfs, 0, sizeof(*vfs));
    vfs->next_mount_id = 1;
    vfs->next_vnode_id = 1;
    vfs->next_file_id = 1;
    vfs->next_handle_id = 1;
    vfs->generation = 1;
    vfs->fs_backend_fd = -1;
}

int filed_vfs_attach_root_backend(filed_vfs_t *vfs, int fs_backend_fd)
{
    if (vfs == NULL || fs_backend_fd < 16) {
        return -1;
    }

    uint64_t magic = 0;
    int status = fs_backend_call(fs_backend_fd, KOBOXD_WIRE_FS_MOUNT_ROOT, 0, &magic);
    if (status != 0 || magic != 0xef53u) {
        fprintf(stderr,
            "[filed] fs-backend mount-root failed status=%d magic=0x%llx\n",
            status,
            (unsigned long long)magic);
        return status != 0 ? status : -2;
    }

    vfs->fs_backend_fd = fs_backend_fd;
    filed_vmount_t *mount = &vfs->mounts[0];
    mount->mount_id = vfs->next_mount_id++;
    mount->fs_backend_fd = fs_backend_fd;
    mount->root_object_id = KOBOXD_WIRE_FS_ROOT_OBJECT_ID;
    mount->generation = vfs->generation;
    mount->active = 1;

    filed_vnode_t *root = &vfs->vnodes[0];
    root->vnode_id = vfs->next_vnode_id++;
    root->mount_id = mount->mount_id;
    root->backend_object_id = mount->root_object_id;
    root->parent_vnode_id = root->vnode_id;
    root->size = 0;
    root->mode = 0040755u;
    root->generation = vfs->generation;
    root->kind = FILED_VNODE_KIND_DIR;
    snprintf(root->name, sizeof(root->name), "/");
    root->active = 1;

    filed_vfs_handle_t *handle = &vfs->handles[0];
    handle->handle_id = vfs->next_handle_id++;
    handle->object_id = root->vnode_id;
    handle->generation = vfs->generation;
    handle->rights = 0;
    handle->active = 1;

    return 0;
}

int filed_vfs_self_check(filed_vfs_t *vfs)
{
    if (vfs == NULL || vfs->fs_backend_fd < 16) {
        return -1;
    }
    if (!vfs->mounts[0].active || !vfs->vnodes[0].active || !vfs->handles[0].active) {
        return -2;
    }
    if (vfs->vnodes[0].kind != FILED_VNODE_KIND_DIR ||
        vfs->vnodes[0].backend_object_id != KOBOXD_WIRE_FS_ROOT_OBJECT_ID)
    {
        return -3;
    }
    return 0;
}

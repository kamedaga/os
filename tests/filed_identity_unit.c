#include <assert.h>
#include <stdio.h>
#include "../userland/filed/src/dispatch/identity.c"
#include "../userland/filed/src/exec/router.c"

const filed_handle_t *filed_find_handle_const(const filed_vfs_t *vfs, filed_handle_id_t id)
{
    for (unsigned i = 0; i < FILED_MAX_HANDLES; i++)
        if (vfs->handles[i].active && vfs->handles[i].id == id) return &vfs->handles[i];
    return NULL;
}

static filed_runtime_t runtime;
int main(void)
{
    struct filed_client root = { .id = 11, .identity = { .pid = 1, .generation = 1 } };
    struct filed_client user = { .id = 12, .identity = { .pid = 2, .generation = 2,
        .uid = 1001, .euid = 1001, .rights = 1023 } };
    runtime.vfs.handles[0] = (filed_handle_t){ .active = true, .id = 70, .owner_client = 11 };
    runtime.vfs.handles[1] = (filed_handle_t){ .active = true, .id = 71, .owner_client = 12 };
    runtime.actor = &root;
    filed_openat_t open = { .rights = FILED_RIGHT_READ, .name = "/etc/passwd" };
    assert(filed_client_authorize(&runtime, FILED_OP_VFS_OPENAT, &open, sizeof(open), 0) == -13);
    assert(filed_client_authorize(&runtime, FILED_OP_CLIENT_REGISTER, &user.identity, sizeof(user.identity), 0) == -1);
    assert(filed_client_authorize(&runtime, FILED_OP_SERVICE_SET_NETD_SOCKET, NULL, 0, 0) == -1);
    filed_io_t io = { .handle = 71, .length = 1 };
    assert(filed_client_authorize(&runtime, FILED_OP_VFS_READ, &io, sizeof(io), 0) == -9);
    assert(filed_client_authorize(&runtime, FILED_OP_VFS_CLOSE, NULL, 0, 71) == -9);
    /* A delegated descriptor remains authority even without namespace access. */
    io.handle = 70;
    assert(filed_client_authorize(&runtime, FILED_OP_VFS_READ, &io, sizeof(io), 0) == 0);
    runtime.actor = &user;
    assert(filed_client_authorize(&runtime, FILED_OP_VFS_OPENAT, &open, sizeof(open), 0) == 0);
    assert(filed_client_authorize(&runtime, FILED_OP_VFS_READ, &io, sizeof(io), 0) == -9);
    io.handle = UINT64_C(0x100000047); /* cannot truncate into somebody's handle */
    assert(filed_client_authorize(&runtime, FILED_OP_VFS_READ, &io, sizeof(io), 0) == -9);
    filed_exec_path_t exec = { .inherit_handle_count = 1, .inherit_handles = {70} };
    assert(filed_client_authorize(&runtime, FILED_OP_EXEC_SELF, &exec, sizeof(exec), 0) == -9);
    exec.inherit_handles[0] = 71;
    assert(filed_client_authorize(&runtime, FILED_OP_EXEC_SELF, &exec, sizeof(exec), 0) == 0);
    user.identity.rights = FILED_RIGHT_LOOKUP | FILED_RIGHT_READ;
    open.open_flags = FILED_OPEN_CREATE;
    assert(filed_client_authorize(&runtime, FILED_OP_VFS_OPENAT, &open, sizeof(open), 0) == -13);
    filed_unlink_t remove = {0};
    assert(filed_client_authorize(&runtime, FILED_OP_VFS_UNLINK, &remove, sizeof(remove), 0) == -13);
    runtime.actor = NULL;
    assert(filed_client_authorize(&runtime, FILED_OP_CLIENT_REGISTER, &user.identity, sizeof(user.identity), 0) == 0);
    runtime.clients = &root;
    filed_identity_update_t update = { .client = root.id, .identity = root.identity };
    update.identity.uid = update.identity.euid = 81;
    assert(!filed_client_credentials(&runtime, &update));
    assert(root.identity.uid == 81 && root.identity.rights == 0);
    update.identity.rights = FILED_RIGHT_READ;
    assert(filed_client_credentials(&runtime, &update) == -1);
    update.identity = root.identity; update.identity.generation++;
    assert(filed_client_credentials(&runtime, &update) == -1);
    update.identity = root.identity;
    runtime.actor = &root;
    assert(filed_client_credentials(&runtime, &update) == -1);
    assert(filed_client_authorize(&runtime, FILED_OP_CLIENT_CREDENTIALS, &update, sizeof(update), 0) == -1);
    runtime.actor = NULL;
    struct pacha_process_fd_grant grants[32];
    uint64_t count;
    exec = (filed_exec_path_t){ .flags = FILED_EXEC_LINUX_LPR };
    assert(!filed_exec_build_grants(&runtime, &exec, NULL, 0, -1, grants, &count) && count == 2);
    exec.flags |= FILED_EXEC_SERVICE_NETD;
    runtime.netd_socket_endpoint_fd = 55;
    assert(!filed_exec_build_grants(&runtime, &exec, NULL, 0, -1, grants, &count) && count == 3);
    assert(grants[2].source_fd == 55 && grants[2].target_fd == FILED_EXEC_NETD_SOCKET_ENDPOINT_FD);
    runtime.actor = &root;
    assert(filed_exec_build_grants(&runtime, &exec, NULL, 0, -1, grants, &count) == -1);
    puts("filed identity: manager-only issuance, root without grant denied, handle isolation and namespace rights passed");
}

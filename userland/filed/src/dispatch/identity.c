#include "common.h"
#include "../vfs/private.h"

int filed_client_create(filed_runtime_t *runtime, const filed_identity_t *identity,
    int *client_fd, uint64_t *id)
{
    if (!identity || identity->pid <= 0 || !identity->generation ||
        (identity->rights & ~1023u)) return -22;
    if (runtime->client_sequence == UINT64_MAX) return -75;
    unsigned clients = 0;
    for (struct filed_client *it = runtime->clients; it; it = it->next) clients++;
    /* Reserve wait slots for every fast session and every live transfer lease. */
    if (clients + FILED_RUNTIME_MAX_SESSIONS + FILED_MAX_HANDLES + 4 >= PACHA_SERVICE_WAIT_MAX_FDS)
        return -24;
    struct filed_client *client = calloc(1, sizeof(*client));
    if (!client) return -12;
    struct pacha_fd_table_info info;
    const uint64_t reserve = PACHA_IPC_MAX_TRANSFER_FDS + 2;
    if (pacha_fd_table(0, &info) != 0 ||
        (info.free_slots < reserve && pacha_fd_table(info.capacity + reserve - info.free_slots, &info) != 0)) {
        free(client); return -24;
    }
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER;
    struct pacha_ipc_channel_pair pair;
    int status = pacha_ipc_channel_create(&pair, rights, 0);
    if (status != 0) { free(client); return -12; }
    *client = (struct filed_client){ .next = runtime->clients,
        .id = ++runtime->client_sequence, .fd = pair.a, .identity = *identity };
    runtime->clients = client;
    *client_fd = pair.b;
    *id = client->id;
    return 0;
}

int filed_client_credentials(filed_runtime_t *runtime, const filed_identity_update_t *update)
{
    if (runtime->actor) return -1;
    struct filed_client *client = runtime->clients;
    while (client && client->id != update->client) client = client->next;
    if (!client) return -3;
    /* Identity changes cannot replace the subject or enlarge its grants. */
    if (update->identity.pid != client->identity.pid ||
        update->identity.generation != client->identity.generation ||
        update->identity.rights != client->identity.rights) return -1;
    client->identity = update->identity;
    return 0;
}

void filed_client_release(filed_runtime_t *runtime, struct filed_client *client)
{
    /* Fast pages cannot outlive their authenticated control connection. */
    for (unsigned i = 0; i < FILED_RUNTIME_MAX_SESSIONS; i++) {
        filed_session_t *session = &runtime->sessions[i];
        if (!session->active || session->client != client) continue;
        (void)pacha_munmap(session->page, session->page_size);
        (void)pacha_fd_close(session->page_fd);
        (void)pacha_fd_close(session->channel_fd);
        *session = (filed_session_t){ .page_fd = -1, .channel_fd = -1 };
    }
    for (unsigned i = 0; i < FILED_MAX_HANDLES; i++) {
        filed_handle_t *handle = &runtime->vfs.handles[i];
        if (handle->active && handle->owner_client == client->id && handle->lease_fd < 16)
            (void)filed_close_handle_runtime(runtime, handle->id);
    }
    struct filed_client **cursor = &runtime->clients;
    while (*cursor && *cursor != client) cursor = &(*cursor)->next;
    if (*cursor) *cursor = client->next;
    (void)pacha_fd_close(client->fd);
    free(client);
}

static int handle_allowed(filed_runtime_t *runtime, uint64_t id, int directory)
{
    if (!id) return directory && (runtime->actor->identity.rights & FILED_RIGHT_LOOKUP) ? 0 : -9;
    if (id > UINT32_MAX) return -9;
    const filed_handle_t *handle = filed_find_handle_const(&runtime->vfs, (uint32_t)id);
    return handle && handle->owner_client == runtime->actor->id ? 0 : -9;
}

int filed_client_authorize(filed_runtime_t *runtime, uint32_t op,
    const void *payload, uint64_t size, uint64_t scalar)
{
    if (!runtime->actor) return 0; /* The explicitly delegated launch endpoint. */
    if (op == FILED_OP_HELLO || op == FILED_OP_SESSION_OPEN || op == FILED_OP_DIAG_PING)
        return 0;
    if (op == FILED_OP_VFS_MEMFD_CREATE)
        return (runtime->actor->identity.rights & (FILED_RIGHT_CREATE | FILED_RIGHT_WRITE)) ==
            (FILED_RIGHT_CREATE | FILED_RIGHT_WRITE) ? 0 : -13;
    if (op == FILED_OP_VFS_SYNC_ALL || op == FILED_OP_DIAG_DUMP_METRICS) return 0;
    if (op < FILED_OP_VFS_OPENAT || op > FILED_OP_EXEC_SELF) return -1;
    uint32_t required = 0;
    switch (op) {
    case FILED_OP_VFS_UNLINK: case FILED_OP_VFS_RMDIR: required = FILED_RIGHT_REMOVE; break;
    case FILED_OP_VFS_RENAME: required = FILED_RIGHT_REMOVE | FILED_RIGHT_RENAME; break;
    case FILED_OP_VFS_MKDIR: case FILED_OP_VFS_MKNOD:
    case FILED_OP_VFS_SYMLINK: case FILED_OP_VFS_LINK: required = FILED_RIGHT_CREATE; break;
    case FILED_OP_VFS_OPENAT:
        /* The wire and fast-page forms share these first three words. */
        if (!payload || size < 24) return -22;
        if (((const filed_openat_t *)payload)->rights & ~runtime->actor->identity.rights) return -13;
        if (((const filed_openat_t *)payload)->open_flags & FILED_OPEN_CREATE) required |= FILED_RIGHT_CREATE;
        if (((const filed_openat_t *)payload)->open_flags & FILED_OPEN_TRUNCATE) required |= FILED_RIGHT_WRITE;
        break;
    default: break;
    }
    if (required & ~runtime->actor->identity.rights) return -13;
    if (op == FILED_OP_VFS_SHARED_FILE_VMO) {
        if (size < sizeof(filed_file_vmo_request_t)) return -22;
        if ((((const filed_file_vmo_request_t *)payload)->flags & FILED_FILE_VMO_EXEC) &&
            !(runtime->actor->identity.rights & FILED_RIGHT_EXEC)) return -13;
    }
    uint64_t first = scalar, second = 0;
    int directory = op == FILED_OP_VFS_OPENAT || op == FILED_OP_VFS_STATAT ||
        (op >= FILED_OP_VFS_UNLINK && op <= FILED_OP_VFS_LINK) ||
        op == FILED_OP_EXEC_PATH || op == FILED_OP_EXEC_SELF;
    if (op != FILED_OP_VFS_CLOSE && op != FILED_OP_VFS_FSYNC &&
        op != FILED_OP_VFS_WRITE_BATCH && op != FILED_OP_VFS_PWRITE_BATCH) {
        if (!payload || size < sizeof(first)) return -22;
        memcpy(&first, payload, sizeof(first));
    }
    int status = handle_allowed(runtime, first, directory);
    if (status) return status;
    if (op == FILED_OP_VFS_RENAME || op == FILED_OP_VFS_LINK || op == FILED_OP_VFS_VALIDATE_OPEN_CACHE) {
        if (size < 16) return -22;
        memcpy(&second, (const uint8_t *)payload + 8, 8);
        status = handle_allowed(runtime, second, 1);
    }
    if (!status && (op == FILED_OP_EXEC_PATH || op == FILED_OP_EXEC_SELF)) {
        if (!(runtime->actor->identity.rights & FILED_RIGHT_EXEC)) return -13;
        if (size < sizeof(filed_exec_path_t)) return -22;
        const filed_exec_path_t *exec = payload;
        if (exec->inherit_handle_count > FILED_EXEC_MAX_INHERIT_HANDLES) return -22;
        for (uint64_t i = 0; i < exec->inherit_handle_count; i++) {
            status = handle_allowed(runtime, exec->inherit_handles[i], 0);
            if (status) return status;
        }
    }
    return status;
}

int filed_lease_receive(filed_runtime_t *runtime, uint32_t id)
{
    filed_handle_t *handle = filed_find_handle(&runtime->vfs, id);
    if (!handle || handle->lease_fd < 16) return -9;
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS] = {{0}};
    struct pacha_ipc_msg message = { .fds = fds, .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS };
    int status = pacha_ipc_recv(handle->lease_fd, &message);
    if (status) return status;
    int reply = -1;
    if (message.fd_count) {
        struct pacha_fd_info info;
        int last = (int)fds[message.fd_count - 1].fd;
        if (pacha_fd_get_info(last, &info) == 0 && info.kind == PACHA_FD_KIND_REPLY) reply = last;
    }
    status = -22;
    if (reply >= 16 && message.fd_count == 1 && message.word0 == FILED_LEASE_MAGIC && message.word1 == id) {
        struct filed_client *target = runtime->clients;
        while (target && target->id != message.word2) target = target->next;
        /* One prepared duplicate has one recipient. Repeated adoption by the
         * same recipient is harmless; forwarding creates a new duplicate. */
        status = !target ? -3 : handle->owner_client && handle->owner_client != target->id ? -1 : 0;
        if (!status) handle->owner_client = target->id;
    }
    if (reply >= 16) {
        struct pacha_ipc_msg response = { .word0 = FILED_LEASE_MAGIC, .word1 = (uint64_t)(int64_t)status,
            .word2 = id, .word3 = message.word3 };
        (void)pacha_ipc_reply(reply, &response);
    }
    for (unsigned i = 0; i < message.fd_count; i++) (void)pacha_fd_close((int)fds[i].fd);
    return 0;
}

#include "rights.h"
#include "../lpr_filed_internal.h"
#include <errno.h>

struct linux_cmsg { uint64_t length; int32_t level, type; };

void lpr_unix_credentials_output(struct lpr_unix_ancillary *state,
    const struct unix_credentials *credentials, uint32_t *flags)
{
    if (!state || !state->passcred) return;
    struct { struct linux_cmsg header; int32_t pid; uint32_t uid, gid; } value = {
        .header = { .length = sizeof(struct linux_cmsg) + 12, .level = 1, .type = 2 },
        .pid = credentials && credentials->generation ? credentials->pid : 0,
        .uid = credentials && credentials->generation ? credentials->uid : 65534u,
        .gid = credentials && credentials->generation ? credentials->gid : 65534u };
    uint64_t available = state->capacity - state->used;
    if (available < value.header.length && flags) *flags |= 8; /* MSG_CTRUNC */
    if (available < sizeof(struct linux_cmsg)) return;
    uint64_t copied = available < value.header.length ? available : value.header.length;
    value.header.length = copied;
    lpr_memcpy((void *)(uintptr_t)(state->control + state->used), &value, copied);
    uint64_t used = (copied + 7) & ~UINT64_C(7);
    state->used += used < available ? used : available;
}

int lpr_unix_rights_parse(struct lpr_unix_ancillary *state)
{
    uint64_t offset = 0;
    while (state->capacity - offset >= sizeof(struct linux_cmsg)) {
        struct linux_cmsg header;
        lpr_memcpy(&header, (void *)(uintptr_t)(state->control + offset), sizeof(header));
        if (header.length < sizeof(header) || header.length > state->capacity - offset) return -EINVAL;
        if (header.level != 1 || (header.type != 1 && header.type != 2)) return -EOPNOTSUPP;
        const uint64_t bytes = header.length - sizeof(header);
        if (header.type == 2) {
            struct { int32_t pid; uint32_t uid, gid; } credentials;
            if (bytes != sizeof(credentials)) return -EINVAL;
            lpr_memcpy(&credentials,
                (void *)(uintptr_t)(state->control + offset + sizeof(header)), sizeof(credentials));
            state->credentials = (struct unix_credentials){ .generation = 1,
                .pid = credentials.pid, .uid = credentials.uid, .gid = credentials.gid };
        } else {
            if (bytes % sizeof(int) || bytes / sizeof(int) > UNIX_RIGHTS_MAX - state->count) return -EINVAL;
            lpr_memcpy(state->fds + state->count,
                (void *)(uintptr_t)(state->control + offset + sizeof(header)), bytes);
            state->count += (unsigned)(bytes / sizeof(int));
        }
        if (header.length > UINT64_MAX - 7) return -EINVAL;
        const uint64_t aligned = (header.length + 7) & ~UINT64_C(7);
        if (aligned > state->capacity - offset) break;
        offset += aligned;
    }
    return 0;
}

static int call(struct lpr_unix_context *context, struct unix_control *request)
{
    unsigned received;
    return lpr_unix_context_call(context, request, NULL, 0, NULL, 0, &received);
}

static void close_caps(const int *fds, unsigned count)
{
    for (unsigned i = 0; i < count; i++)
        if (fds[i] >= 16) (void)lpr_close_native_fd_if_open((uint32_t)fds[i]);
}

static void ack(struct lpr_unix_context *context, uint64_t ticket, uint64_t operation, int receiving)
{
    struct unix_control request = { .operation = UNIX_OP_RIGHTS_ACK,
        .io.owner = context->waiter.owner,
        .rights = { .ticket = ticket, .operation = operation,
            .flags = receiving ? UNIX_RIGHTS_RECEIVING : 0 } };
    (void)call(context, &request);
}

void lpr_unix_rights_cancel(struct lpr_unix_context *context,
    const struct lpr_unix_ancillary *state)
{
    if (!state || !state->ticket) return;
    struct unix_control request = { .operation = UNIX_OP_RIGHTS_CANCEL,
        .io.owner = context->waiter.owner, .rights.ticket = state->ticket };
    if (call(context, &request) == 0) ack(context, state->ticket, state->operation, 0);
}

int lpr_unix_rights_prepare(struct lpr_unix_context *context, uint64_t socket,
    struct lpr_unix_ancillary *state)
{
    if (!state || (!state->count && !state->credentials.generation && !state->automatic_credentials)) return 0;
    if (state->credentials.generation) {
        /* Obtain the current authenticated generation, not a process-local
         * fork/exec snapshot. PREPARE validates the claimed Linux ucred. */
        struct unix_control identity = { .operation = UNIX_OP_HELLO };
        int status = call(context, &identity);
        if (status) return status;
        if (!identity.credentials.generation) return -EPROTO;
        state->credentials.generation = identity.credentials.generation;
    }
    int status = lpr_unix_context_next_request(context, &state->operation);
    if (status) return status;
    struct unix_control request = { .operation = UNIX_OP_RIGHTS_PREPARE, .socket = socket,
        .credentials = state->credentials,
        .io.owner = context->waiter.owner,
        .rights = { .operation = state->operation, .total = state->count, .route = state->route } };
    status = call(context, &request);
    if (status) return status;
    state->ticket = request.rights.ticket;
    if (!state->ticket) return -EPROTO;
    for (unsigned offset = 0; offset < state->count;) {
        request = (struct unix_control){ .operation = UNIX_OP_RIGHTS_APPEND,
            .io.owner = context->waiter.owner,
            .rights = { .ticket = state->ticket, .offset = offset } };
        int fds[PACHA_IPC_MAX_TRANSFER_FDS];
        struct pacha_ipc_fd caps[PACHA_IPC_MAX_TRANSFER_FDS];
        unsigned cap_count = 0;
        lpr_fd_pin_t pins[4];
        unsigned pinned = 0;
        /* Four descriptors fit even when each provider needs four caps,
         * leaving room for the control page and native reply capability. */
        while (request.rights.count < 4 && offset + request.rights.count < state->count) {
            const int fd = state->fds[offset + request.rights.count];
            lpr_fd_pin_t *pin = &pins[pinned];
            if (fd < 0 || lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, pin) != 0) {
                status = -EBADF; break;
            }
            pinned++;
            unsigned exported = 0;
            struct unix_transfer_item *item = &request.rights.items[request.rights.count];
            status = !(pin->effective_rights & LPR_FD_RIGHT_DUP) ? -EBADF :
                lpr_fd_transfer_prepare(pin, item, fds + cap_count,
                    PACHA_IPC_MAX_TRANSFER_FDS - 2 - cap_count, &exported);
            if (status) break;
            item->capability_first = cap_count;
            cap_count += exported;
            request.rights.count++;
        }
        for (unsigned i = 0; !status && i < cap_count; i++) {
            struct pacha_fd_info info;
            if (!lpr_native_fd_info((uint32_t)fds[i], &info)) { status = -EBADF; break; }
            caps[i] = (struct pacha_ipc_fd){ .fd = (uint32_t)fds[i], .rights = info.rights };
        }
        unsigned received;
        if (!status) status = lpr_unix_context_call(context, &request, caps, cap_count, NULL, 0, &received);
        close_caps(fds, cap_count);
        for (unsigned i = 0; i < pinned; i++) lpr_fd_unpin(&pins[i]);
        if (status) { lpr_unix_rights_cancel(context, state); state->ticket = 0; return status; }
        offset += request.rights.count;
    }
    return 0;
}

int lpr_unix_rights_commit(struct lpr_unix_context *context, uint64_t socket,
    const struct lpr_unix_ancillary *state, const struct unix_write *write)
{
    struct unix_control request = { .operation = UNIX_OP_RIGHTS_COMMIT, .socket = socket,
        .io = { .owner = write->owner, .before = write->before, .after = write->after, .length = write->length },
        .rights = { .ticket = state->ticket, .operation = state->operation } };
    int status = call(context, &request);
    if (!status) ack(context, state->ticket, state->operation, 0);
    return status;
}

static void abort_imports(const lpr_linux_fd_t *fds, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        lpr_fd_drop_t drop = {0};
        if (lpr_fd_table_abort_staged(&lpr_control_fd_table, fds[i], &drop) == 0 && drop.ready)
            (void)lpr_backend_finish_drop(&drop);
    }
}

/* CLAIM's caps are owned here on every return. Broker ownership is not yet
 * ours: process_token stays zero until FINISH, so rollback only unmaps. */
static int stage_socket(const struct unix_control *claim,
    const struct pacha_ipc_fd *caps, unsigned count, uint64_t flags, lpr_linux_fd_t *fd)
{
    const struct unix_attachment *attachment = &claim->attachment;
    int native[PACHA_IPC_MAX_TRANSFER_FDS];
    for (unsigned i = 0; i < count; i++) native[i] = (int)caps[i].fd;
    if (claim->rights.count != 1 || attachment->socket != claim->rights.items[0].object ||
        !attachment->socket || attachment->reserved || attachment->listening > 1 ||
        (attachment->flags & ~UINT32_C(0x800)) ||
        (attachment->type != UNIX_TRANSPORT_STREAM && attachment->type != UNIX_TRANSPORT_SEQPACKET &&
         attachment->type != UNIX_TRANSPORT_DGRAM) ||
        (attachment->generation && (attachment->listening || attachment->type == UNIX_TRANSPORT_DGRAM)) ||
        claim->rights.items[0].capability_count != count ||
        claim->rights.items[0].capability_first || claim->rights.items[0].rights ||
        claim->rights.items[0].flags || claim->rights.items[0].provider_data ||
        count != (attachment->generation ? 2u : 0u)) {
        close_caps(native, count); return -EPROTO;
    }
    struct lpr_unix_socket *socket = lpr_backend_state_alloc(sizeof(*socket));
    if (!socket) { close_caps(native, count); return -ENOMEM; }
    *socket = (struct lpr_unix_socket){ .socket = attachment->socket,
        .type = attachment->type, .flags = LPR_LINUX_O_RDWR | attachment->flags,
        .listening = attachment->listening, .mapping = { .fds = {-1, -1} } };
    int status = 0;
    if (count) {
        status = lpr_unix_mapping_import(attachment, caps, count, &socket->mapping);
        if (!status) socket->mapped = 1;
    }
    if (!status) {
        const lpr_fd_install_t install = { .ops_id = LPR_FD_OPS_UNIX,
            .fd_flags = flags & UINT64_C(0x40000000) ? LPR_FD_ENTRY_CLOEXEC : 0,
            .access_mode = LPR_LINUX_O_RDWR,
            .status_flags = attachment->flags & 0x800 ? LPR_OFD_NONBLOCK : 0,
            .rights = LPR_FD_RIGHT_READ | LPR_FD_RIGHT_WRITE | LPR_FD_RIGHT_DUP |
                LPR_FD_RIGHT_STAT | LPR_FD_RIGHT_IOCTL,
            .backend_state = socket, .backend_state_bytes = sizeof(*socket) };
        while (lpr_fd_table_stage_batch(&lpr_control_fd_table, &install, 1, fd) != 0) {
            const uint64_t capacity = lpr_fd_table_capacity;
            if (capacity >= LPR_FD_TABLE_MAX_SIZE || lpr_fd_table_ensure_capacity(capacity + 1) != 0) {
                status = -EMFILE; break;
            }
        }
    }
    if (status) {
        lpr_unix_mapping_destroy(&socket->mapping);
        (void)lpr_backend_state_free(socket, sizeof(*socket));
    }
    return status;
}

static void activate_sockets(const lpr_linux_fd_t *fds, unsigned count)
{
    lpr_fd_table_lock(&lpr_control_fd_table);
    for (unsigned i = 0; i < count; i++) {
        const lpr_fd_entry_t *entry = &lpr_control_fd_table.entries[fds[i]];
        const lpr_ofd_t *ofd = &lpr_control_fd_table.ofds[entry->ofd_index];
        if (entry->active == 2 && lpr_ofd_ops_id(ofd) == LPR_FD_OPS_UNIX)
            ((struct lpr_unix_socket *)lpr_backend_state_from_ofd(ofd))->process_token = lpr_supervisor_token;
    }
    lpr_fd_table_unlock(&lpr_control_fd_table);
}

int lpr_unix_rights_receive(struct lpr_unix_context *context, uint64_t socket,
    const struct unix_read *read, struct lpr_unix_ancillary *state,
    uint64_t flags, uint32_t *message_flags)
{
    const uint64_t credential_space = state && state->passcred ?
        (state->capacity < 32 ? state->capacity : 32) : 0;
    uint64_t available = state && state->capacity - credential_space >= sizeof(struct linux_cmsg) ?
        (state->capacity - credential_space - sizeof(struct linux_cmsg)) / sizeof(int) : 0;
    unsigned capacity = available > UNIX_RIGHTS_MAX ? UNIX_RIGHTS_MAX : (unsigned)available;
    lpr_linux_fd_t imported[UNIX_RIGHTS_MAX];
    unsigned taken = 0, total = 0;
    struct unix_credentials credentials = {0};
    struct unix_control request = { .operation = UNIX_OP_RIGHTS_CLAIM, .socket = socket,
        .io = { .owner = read->owner, .operation = read->operation,
            .before = read->before, .after = read->after, .length = read->length,
            .message_length = read->message_length }, .rights.ticket = read->ticket };
    int status = 0;
    /* Claim once even with no output buffer: authenticate the ticket and
     * obtain its total before the broker discards the unimported suffix. */
    do {
        request.operation = UNIX_OP_RIGHTS_CLAIM;
        request.rights.flags = 0;
        request.rights.offset = taken;
        request.rights.count = capacity > taken ? capacity - taken : 1;
        if (request.rights.count > UNIX_TRANSFER_BATCH) request.rights.count = UNIX_TRANSFER_BATCH;
        struct pacha_ipc_fd caps[PACHA_IPC_MAX_TRANSFER_FDS] = {{0}};
        unsigned received = 0;
        status = lpr_unix_context_call(context, &request, NULL, 0, caps, PACHA_IPC_MAX_TRANSFER_FDS, &received);
        if (status) break;
        if (request.rights.flags & UNIX_RIGHTS_CREDENTIALS) credentials = request.credentials;
        int native[PACHA_IPC_MAX_TRANSFER_FDS];
        for (unsigned i = 0; i < received; i++) native[i] = (int)caps[i].fd;
        total = request.rights.total;
        if (total > UNIX_RIGHTS_MAX || total < taken || request.rights.count > UNIX_TRANSFER_BATCH ||
            (capacity > taken && request.rights.count > capacity - taken) ||
            request.rights.count > total - taken || request.result == 0) {
            close_caps(native, received); status = -EPROTO; break;
        }
        if (taken == capacity || request.rights.count == 0) { close_caps(native, received); break; }
        if (request.rights.items[0].provider == UNIX_TRANSFER_SOCKET) {
            status = stage_socket(&request, caps, received, flags, &imported[taken]);
            if (!status) { taken++; continue; }
            if (status == -EMFILE || status == -ENOMEM) status = 0;
            break;
        }
        int descriptors[UNIX_TRANSFER_BATCH];
        status = lpr_fd_transfer_stage_batch(request.rights.items, request.rights.count,
            native, received, flags & UINT64_C(0x40000000) ? LPR_LINUX_O_CLOEXEC : 0, descriptors);
        if (status) {
            close_caps(native, received);
            if (status == -EMFILE || status == -ENOMEM || status == -EOPNOTSUPP) status = 0;
            break;
        }
        for (unsigned i = 0; i < request.rights.count; i++) imported[taken++] = (uint32_t)descriptors[i];
    } while (taken < capacity && taken < total);
    if (status) { abort_imports(imported, taken); return status; }
    uint64_t operation;
    status = lpr_unix_context_next_request(context, &operation);
    if (status) { abort_imports(imported, taken); return status; }
    request.operation = UNIX_OP_RIGHTS_FINISH;
    request.rights.operation = operation;
    request.rights.count = taken;
    request.rights.flags = flags & 2 ? UNIX_RIGHTS_PEEK : 0;
    status = call(context, &request);
    if (status) { abort_imports(imported, taken); return status; }
    activate_sockets(imported, taken);
    if (lpr_fd_table_publish_batch(&lpr_control_fd_table, imported, taken) != 0) {
        abort_imports(imported, taken);
        ack(context, read->ticket, operation, 1);
        return -EIO;
    }
    lpr_unix_credentials_output(state, &credentials, message_flags);
    if (taken) {
        const struct linux_cmsg header = { .length = sizeof(header) + taken * sizeof(int), .level = 1, .type = 1 };
        lpr_memcpy((void *)(uintptr_t)(state->control + state->used), &header, sizeof(header));
        lpr_memcpy((void *)(uintptr_t)(state->control + state->used + sizeof(header)), imported, taken * sizeof(int));
        state->used += (header.length + 7) & ~UINT64_C(7);
        if (state->used > state->capacity) state->used = state->capacity;
    }
    if (taken < total && message_flags) *message_flags |= 8; /* MSG_CTRUNC */
    ack(context, read->ticket, operation, 1);
    return 0;
}

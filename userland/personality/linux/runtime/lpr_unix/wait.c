#include "wait.h"
#include "../lpr_filed_internal.h"
#include <errno.h>

static int current(const struct lpr_unix_waiter *waiter)
{
    return waiter && waiter->client && waiter->process_token &&
        waiter->process_token == lpr_supervisor_token &&
        waiter->client->process_token == waiter->process_token &&
        waiter->client->fd >= 16 && waiter->client->fd < PACHAOS_FD_TABLE_LIMIT;
}

static void close_receive(int fd)
{
    if (fd >= 16 && fd < PACHAOS_FD_TABLE_LIMIT)
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
}

void lpr_unix_waiter_init(struct lpr_unix_waiter *waiter,
    const struct lpr_unix_client *client, uint64_t owner)
{
    if (waiter) *waiter = (struct lpr_unix_waiter){ .client = client,
        .process_token = client ? client->process_token : 0, .owner = owner, .fd = -1 };
}

void lpr_unix_waiter_destroy(struct lpr_unix_waiter *waiter)
{
    if (!waiter) return;
    /* Still close after client_close(), but never in a fork child. */
    if (waiter->process_token && waiter->process_token == lpr_supervisor_token)
        close_receive(waiter->fd);
    *waiter = (struct lpr_unix_waiter){ .fd = -1 };
}

static int control(struct lpr_unix_waiter *waiter, struct unix_control *request,
    struct pacha_ipc_fd *cap, unsigned capacity, unsigned *count)
{
    if (!current(waiter)) return -ENOTCONN;
    if (waiter->request == UINT64_MAX) return -EOVERFLOW;
    request->request = ++waiter->request;
    return lpr_unix_client_call(waiter->client, request, NULL, 0, cap, capacity, count);
}

static uint64_t next_identity;

static int socket_identity(uint64_t *identity, uint64_t *out)
{
    if (!identity) return -EINVAL;
    *out = __atomic_load_n(identity, __ATOMIC_ACQUIRE);
    if (*out) return 0;
    uint64_t previous = __atomic_load_n(&next_identity, __ATOMIC_RELAXED);
    do {
        if (previous == UINT64_MAX) return -EOVERFLOW;
    } while (!__atomic_compare_exchange_n(&next_identity, &previous, previous + 1,
        0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    uint64_t zero = 0;
    const uint64_t candidate = previous + 1;
    *out = __atomic_compare_exchange_n(identity, &zero, candidate, 0,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? candidate : zero;
    return 0;
}

static int remove_watch(struct lpr_unix_waiter *waiter, uint64_t socket)
{
    struct unix_control request = { .operation = UNIX_OP_WAIT_REMOVE,
        .socket = socket, .argument = waiter->id };
    unsigned count = 0;
    return control(waiter, &request, NULL, 0, &count);
}

int lpr_unix_waiter_watch(struct lpr_unix_waiter *waiter, uint64_t socket, uint64_t *identity)
{
    if (!current(waiter)) return -ENOTCONN;
    if (!socket || !waiter->owner || waiter->owner == UNIX_TRANSPORT_RECOVERING) return -EINVAL;
    uint64_t local_identity;
    int status = socket_identity(identity, &local_identity);
    if (status) return status;
    unsigned slot;
    for (slot = 0; slot < LPR_WAIT_GRAPH_MAX_LEAVES; slot++) {
        if (waiter->watches[slot].socket != socket) continue;
        if (waiter->watches[slot].references == UINT32_MAX) return -EOVERFLOW;
        if (waiter->watches[slot].identity == local_identity) {
            waiter->watches[slot].references++;
            return 0;
        }
        /* The same broker socket can be received again after this process
         * dropped its last owner and the broker pruned its registrations. */
        break;
    }
    if (slot == LPR_WAIT_GRAPH_MAX_LEAVES) {
        for (unsigned i = 0; i < LPR_WAIT_GRAPH_MAX_LEAVES; i++) {
            const unsigned candidate = (waiter->next + i) % LPR_WAIT_GRAPH_MAX_LEAVES;
            if (waiter->watches[candidate].references) continue;
            slot = candidate;
            break;
        }
        if (slot == LPR_WAIT_GRAPH_MAX_LEAVES) return -EAGAIN;
        if (waiter->watches[slot].socket) {
            /* An error can mean a lost reply after successful removal.
             * Never reuse the old cache hit without a fresh REGISTER. */
            waiter->watches[slot].identity = 0;
            status = remove_watch(waiter, waiter->watches[slot].socket);
            if (status) return status;
            waiter->watches[slot].socket = 0;
        }
        waiter->next = (slot + 1) % LPR_WAIT_GRAPH_MAX_LEAVES;
    }
    struct unix_control request = { .operation = UNIX_OP_WAIT_REGISTER, .socket = socket,
        .transaction = waiter->owner, .argument = waiter->id };
    struct pacha_ipc_fd cap = {0};
    unsigned count = 0;
    status = control(waiter, &request, &cap, waiter->id ? 0 : 1, &count);
    if (status != 0) return status;
    if (waiter->id) {
        if (count || request.result != waiter->id) return -EPROTO;
        goto registered;
    }
    struct pacha_fd_info info;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL;
    if (count != 1 || !request.result || request.result > UINT32_MAX ||
        cap.fd < 16 || cap.fd >= PACHAOS_FD_TABLE_LIMIT ||
        lpr_pacha_syscall2(PACHAOS_SYSCALL_FD_GET_INFO, cap.fd, (uint64_t)(uintptr_t)&info) != 0 ||
        info.kind != PACHA_FD_KIND_CHANNEL || info.rights != rights ||
        info.flags != (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC)) {
        if (count) close_receive((int)cap.fd);
        return -EPROTO;
    }
    waiter->fd = (int)cap.fd;
    waiter->id = (uint32_t)request.result;
registered:
    waiter->watches[slot].socket = socket;
    waiter->watches[slot].identity = local_identity;
    waiter->watches[slot].references++;
    return 0;
}

int lpr_unix_waiter_unwatch(struct lpr_unix_waiter *waiter, uint64_t socket)
{
    if (!current(waiter)) return -ENOTCONN;
    if (!socket) return -EINVAL;
    for (unsigned i = 0; i < LPR_WAIT_GRAPH_MAX_LEAVES; i++) {
        if (waiter->watches[i].socket != socket) continue;
        if (!waiter->watches[i].references) return -EINVAL;
        waiter->watches[i].references--;
        return 0;
    }
    return -EINVAL;
}

static int healthy(struct lpr_unix_waiter *waiter)
{
    if (!current(waiter)) return -ENOTCONN;
    if (!waiter->id || waiter->fd < 16 || waiter->fd >= PACHAOS_FD_TABLE_LIMIT) return -EINVAL;
    struct pacha_pollfd leaves[2] = {
        { .fd = waiter->client->fd, .events = PACHA_FD_EVENT_HANGUP, .revents = UINT64_MAX },
        { .fd = waiter->fd, .events = PACHA_FD_EVENT_HANGUP, .revents = UINT64_MAX },
    };
    const int64_t status = lpr_pacha_syscall2(PACHAOS_SYSCALL_FD_POLL,
        (uint64_t)(uintptr_t)leaves, 2);
    /* Native POLL returns a count, overlapping positive error numbers.
     * Require both output slots to have been written, not just status >= 0. */
    if (leaves[0].revents == UINT64_MAX || leaves[1].revents == UINT64_MAX ||
        status < 0 || status > 2) return -EIO;
    return (leaves[0].revents | leaves[1].revents) & PACHA_FD_EVENT_HANGUP ? -EPIPE : 0;
}

int lpr_unix_waiter_drain(struct lpr_unix_waiter *waiter)
{
    if (!current(waiter)) return -ENOTCONN;
    if (!waiter->id || waiter->fd < 16 || waiter->fd >= PACHAOS_FD_TABLE_LIMIT) return -EINVAL;
    int status;
    for (unsigned i = 0; i < 256; i++) {
        struct pacha_ipc_msg message = {0};
        const int64_t raw = lpr_pacha_syscall2(PACHAOS_SYSCALL_IPC_RECV,
            (uint64_t)(uint32_t)waiter->fd, (uint64_t)(uintptr_t)&message);
        /* Nonblocking IPC_RECV reports EMPTY, not the NOT_READY used by
         * interrupted waits. Treating EMPTY as a retry bypasses arm/sleep. */
        if (raw == PACHA_SYSCALL_ERR_EMPTY || raw == -PACHA_SYSCALL_ERR_EMPTY)
            return healthy(waiter);
        status = (int)pacha_kernel_status_to_errno(raw);
        if (status != 0) return status;
        /* Untrusted senders possess SEND only. Payload is a hint, not an
         * authority to mutate state; old generations and garbage are drained. */
    }
    status = healthy(waiter);
    return status ? status : -EAGAIN;
}

int lpr_unix_waiter_add_graph(struct lpr_unix_waiter *waiter, lpr_wait_graph_t *graph)
{
    if (!current(waiter)) return -ENOTCONN;
    if (!graph || !waiter->id || waiter->fd < 16) return -EINVAL;
    int status = (int)lpr_wait_graph_add_native(graph, waiter->fd,
        PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP);
    if (status == 0) status = (int)lpr_wait_graph_add_native(graph,
        waiter->client->fd, PACHA_FD_EVENT_HANGUP);
    return status;
}

int64_t lpr_unix_waiter_wait(struct lpr_unix_waiter *waiter,
    struct unix_wait_bank *bank, const uint64_t *first_changes,
    const uint64_t *second_changes, int (*ready)(void *), void *context,
    const lpr_wait_deadline_t *deadline)
{
    if (!bank || !first_changes || !second_changes || !ready || !deadline) return -EINVAL;
    if (!current(waiter)) return -ENOTCONN;
    /* A peer may already have made progress since the failed I/O. Avoid
     * draining/polling native channels when no sleep is needed. Retain the
     * post-arm readiness + change check, which closes the lost-wake race. */
    int64_t status = ready(context);
    if (status != 0) return status > 0 ? 0 : status;
    status = lpr_unix_waiter_drain(waiter);
    /* A continuously replenished queue cannot justify sleeping. Recheck I/O
     * after bounded work, rather than exposing EAGAIN as a blocking result. */
    if (status != 0) return status == -EAGAIN ? 0 : status;
    if (waiter->attempt == UINT32_MAX) return -EOVERFLOW;
    struct unix_wait_registration registration;
    status = unix_wait_arm(bank, first_changes, second_changes,
        waiter->id, ++waiter->attempt, &registration);
    if (status != 0) return status;
    status = ready(context);
    if (status == 0 && !unix_wait_changed(first_changes, second_changes, &registration)) {
        lpr_wait_graph_t graph;
        lpr_wait_graph_init(&graph);
        status = lpr_unix_waiter_add_graph(waiter, &graph);
        if (status == 0) status = lpr_wait_graph_block(&graph, deadline);
        if (status == 0) status = healthy(waiter);
    } else if (status > 0) status = 0;
    unix_wait_disarm(bank, &registration);
    return status;
}

/* Exercise the actual service registration/authorization code. Native IPC
 * is represented by bounded independent queues; guest wake delivery remains
 * a separate integration test, not a claim made by this fixture. */
#include "../userland/unixd/src/service.c"
#include <assert.h>

static struct {
    uint64_t kind;
    int peer;
    unsigned pending;
} native_fds[256];

uint64_t pacha_service_wait_revents(const struct pacha_service_wait_set *set, int fd)
{
    for (uint64_t i = 0; i < set->count; i++)
        if (set->fds[i].fd == fd) return set->fds[i].revents;
    return 0;
}

int pacha_fd_table(uint64_t minimum, struct pacha_fd_table_info *out)
{
    if (minimum > 256) return PACHA_ERR_ALLOC;
    *out = (struct pacha_fd_table_info){ .capacity = 256, .maximum = 256 };
    for (int fd = 16; fd < 256; fd++) if (!native_fds[fd].kind) out->free_slots++;
    return 0;
}

int pacha_fd_get_info(int fd, struct pacha_fd_info *out)
{
    if (fd < 16 || fd >= 256 || !native_fds[fd].kind) return -1;
    *out = (struct pacha_fd_info){ .kind = native_fds[fd].kind };
    return 0;
}

int pacha_fd_close(int fd)
{
    assert(fd >= 16 && fd < 256 && native_fds[fd].kind);
    native_fds[fd].kind = 0;
    return 0;
}

int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *out, uint64_t rights, uint32_t flags)
{
    (void)rights; (void)flags;
    int a = -1, b = -1;
    for (int fd = 16; fd < 256; fd++) {
        if (native_fds[fd].kind) continue;
        if (a < 0) a = fd;
        else { b = fd; break; }
    }
    if (b < 0) return -1;
    native_fds[a].kind = native_fds[b].kind = PACHA_FD_KIND_CHANNEL;
    native_fds[a].peer = b; native_fds[b].peer = a;
    native_fds[a].pending = native_fds[b].pending = 0;
    *out = (struct pacha_ipc_channel_pair){ a, b };
    return 0;
}

int pacha_ipc_send(int fd, const struct pacha_ipc_msg *message)
{
    assert(fd >= 16 && fd < 256 && native_fds[fd].kind);
    assert(message->word0 == UNIX_NOTIFY_MAGIC && !message->fd_count);
    int peer = native_fds[fd].peer;
    if (!native_fds[peer].kind) return -1;
    if (native_fds[peer].pending == 4) return PACHA_ERR_ALLOC;
    native_fds[peer].pending++;
    return 0;
}

static int direction_new(void *context, uint64_t generation, uint32_t type,
    struct unix_direction *direction)
{
    (void)context;
    direction->tx = calloc(1, sizeof(*direction->tx));
    direction->rx = calloc(1, sizeof(*direction->rx));
    assert(direction->tx && direction->rx);
    return unix_transport_init(direction->tx, direction->rx, generation, type);
}

static void direction_free(void *context, struct unix_direction *direction)
{
    (void)context;
    free(direction->tx); free(direction->rx);
}

int main(void)
{
    struct unix_service service = { .next_waiter = 1 };
    struct unix_broker_platform platform = { .context = &service,
        .direction_create = direction_new, .direction_destroy = direction_free,
        .notify = notify_socket };
    service.broker = unix_broker_create(&platform);
    assert(service.broker);
    struct service_session parent = {0}, child = {0}, outsider = {0};
    struct unix_credentials credentials = { .pid = 1, .generation = 1 };
    assert(unix_broker_session_create(service.broker, &credentials, &parent.session) == 0);
    credentials.pid++;
    assert(unix_broker_session_create(service.broker, &credentials, &child.session) == 0);
    credentials.pid++;
    assert(unix_broker_session_create(service.broker, &credentials, &outsider.session) == 0);
    struct service_thread thread = { .session = &child, .owner = 77 };
    service.threads = &thread;
    uint64_t pair[2], unrelated;
    assert(unix_broker_socketpair(service.broker, parent.session,
        UNIX_TRANSPORT_STREAM, 0, pair) == 0);
    assert(unix_broker_retain(service.broker, parent.session, pair[1], child.session) == 0);
    assert(unix_broker_close(service.broker, parent.session, pair[1]) == 0);
    assert(unix_broker_socket(service.broker, outsider.session,
        UNIX_TRANSPORT_STREAM, 0, &unrelated) == 0);
    struct unix_control request = { .socket = pair[1], .transaction = 77 };
    struct response response = {0};
    assert(register_waiter(&service, &parent, &request, &response) == -EPERM);
    assert(register_waiter(&service, &child, &request, &response) == 0);
    assert(response.count == 1 && request.result == 1);
    assert(response.capabilities[0].transfer_flags ==
        (PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_PRIVATE | PACHA_IPC_TRANSFER_CLOEXEC));
    assert(response.capabilities[0].rights == (PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL));
    const int receiver = (int)response.capabilities[0].fd;
    assert(native_fds[receiver].pending == 1);
    request.argument = request.result;
    response = (struct response){0};
    assert(register_waiter(&service, &child, &request, &response) == 0);
    assert(response.count == 0 && service.waiters->watches->next == NULL);

    struct unix_control sync = { .operation = UNIX_OP_WAIT_SYNC,
        .socket = pair[0], .argument = request.result };
    assert(sync_waiter(&service, &parent, &sync, &response) == 0);
    assert(response.count == 1 && response.capabilities[0].rights ==
        (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_SEND));
    assert(response.capabilities[0].transfer_flags ==
        (PACHA_IPC_TRANSFER_PRIVATE | PACHA_IPC_TRANSFER_CLOEXEC));
    response = (struct response){0};
    sync.socket = unrelated;
    assert(sync_waiter(&service, &outsider, &sync, &response) == -EPERM && !response.count);
    sync.socket = pair[1];
    assert(sync_waiter(&service, &parent, &sync, &response) == -EPERM && !response.count);
    sync.socket = pair[0];
    sync.operation = UNIX_OP_WAIT_NOTIFY;
    for (unsigned i = 0; i < 8; i++)
        assert(sync_waiter(&service, &parent, &sync, &response) == 0);
    assert(native_fds[receiver].pending == 4 && !response.count); /* bounded/full coalescing */

    struct unix_attachment attachment;
    struct unix_direction tx, rx;
    assert(unix_broker_attach(service.broker, child.session, pair[1], &attachment, &tx, &rx) == 0);
    struct unix_wait_registration registration;
    assert(unix_wait_arm(&rx.rx->waiters, &rx.tx->changes, &rx.rx->changes,
        (uint32_t)request.result, 1, &registration) == 0);
    assert(remove_waiter(&service, &parent, &request) == -EPERM);
    drop_thread_waiters(&service, 77);
    assert(service.waiters == NULL && rx.rx->waiters.armed[registration.slot] == 0);
    assert(sync_waiter(&service, &parent, &sync, &response) == -ENOENT);
    (void)pacha_fd_close(receiver);

    /* exec can close the receive cap while the native thread remains alive. */
    request.argument = 0;
    response = (struct response){0};
    assert(register_waiter(&service, &child, &request, &response) == 0);
    const int exec_receiver = (int)response.capabilities[0].fd;
    assert(unix_wait_arm(&rx.rx->waiters, &rx.tx->changes, &rx.rx->changes,
        (uint32_t)request.result, 2, &registration) == 0);
    const struct pacha_service_wait_set hangup = { .count = 1,
        .fds = {{ .fd = service.waiters->notify_fd, .revents = PACHA_FD_EVENT_HANGUP }} };
    (void)pacha_fd_close(exec_receiver);
    reap(&service, &hangup);
    assert(!service.waiters && service.threads == &thread);
    assert(rx.rx->waiters.armed[registration.slot] == 0);

    request.argument = 0;
    response = (struct response){0};
    for (int fd = 16; fd < 240; fd++) native_fds[fd].kind = PACHA_FD_KIND_EVENT;
    assert(register_waiter(&service, &child, &request, &response) == -EMFILE);
    assert(!service.waiters && !response.count);
    memset(native_fds, 0, sizeof(native_fds));
    assert(register_waiter(&service, &child, &request, &response) == 0);
    const int second_receiver = (int)response.capabilities[0].fd;
    assert(unix_broker_retain(service.broker, child.session, pair[1], child.session) == 0);
    assert(unix_broker_close(service.broker, child.session, pair[1]) == 0);
    prune_socket_watches(&service, &child, pair[1]);
    assert(service.waiters->watches != NULL); /* one dup still owns the OFD */
    assert(unix_broker_close(service.broker, child.session, pair[1]) == 0);
    prune_socket_watches(&service, &child, pair[1]);
    assert(service.waiters->watches == NULL);
    request.argument = request.result;
    request.socket = 0;
    assert(remove_waiter(&service, &child, &request) == 0 && !service.waiters);
    assert(remove_waiter(&service, &child, &request) == 0);
    (void)pacha_fd_close(second_receiver);
    /* An authenticated client can bypass LPR's cache. Enforce the budget in
     * the service, while allowing duplicates and reuse after removal. */
    uint64_t budget_sockets[UNIX_WAIT_REGISTRATION_LIMIT + 1];
    request = (struct unix_control){ .transaction = 77 };
    response = (struct response){0};
    int budget_receiver = -1;
    for (unsigned i = 0; i <= UNIX_WAIT_REGISTRATION_LIMIT; i++) {
        assert(unix_broker_socket(service.broker, child.session, UNIX_TRANSPORT_DGRAM, 0,
            &budget_sockets[i]) == 0);
        request.socket = budget_sockets[i];
        int status = register_waiter(&service, &child, &request, &response);
        assert(status == (i == UNIX_WAIT_REGISTRATION_LIMIT ? -EAGAIN : 0));
        if (i == 0) {
            budget_receiver = response.capabilities[0].fd;
            request.argument = request.result;
            response = (struct response){0};
        }
    }
    request.socket = budget_sockets[0];
    assert(register_waiter(&service, &child, &request, &response) == 0 && !response.count);
    assert(remove_waiter(&service, &child, &request) == 0);
    request.socket = budget_sockets[UNIX_WAIT_REGISTRATION_LIMIT];
    assert(register_waiter(&service, &child, &request, &response) == 0);
    request.socket = 0;
    assert(remove_waiter(&service, &child, &request) == 0);
    (void)pacha_fd_close(budget_receiver);
    unix_broker_destroy(service.broker);
    puts("unix service waits: private receive, restricted send, authorization, reuse, full queue, admission, death/close cleanup passed");
    return 0;
}

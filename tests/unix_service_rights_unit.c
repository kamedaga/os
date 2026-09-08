/* Actual service SCM dispatch with a bounded native FD table mock. This
 * covers wire segmentation/escrow, not native IPC or LPR hidden imports. */
#include "../userland/unixd/src/service.c"
#include <assert.h>

static struct pacha_fd_info native_fds[256];
static unsigned closes;

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
    *out = native_fds[fd];
    return 0;
}

int pacha_fd_close(int fd)
{
    assert(fd >= 16 && fd < 256 && native_fds[fd].kind);
    native_fds[fd] = (struct pacha_fd_info){0};
    closes++;
    return 0;
}

static struct pacha_ipc_fd new_cap(void)
{
    for (int fd = 16; fd < 256; fd++) {
        if (native_fds[fd].kind) continue;
        native_fds[fd] = (struct pacha_fd_info){ .kind = PACHA_FD_KIND_CHANNEL,
            .rights = PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE };
        return (struct pacha_ipc_fd){ .fd = (uint64_t)fd };
    }
    assert(0);
    return (struct pacha_ipc_fd){0};
}

static void cleanup(struct pacha_ipc_fd *fds, unsigned count)
{
    for (unsigned i = 0; i < count; i++) if (fds[i].fd) {
        pacha_fd_close((int)fds[i].fd);
        fds[i].fd = 0;
    }
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

static void wire_read(struct unix_control *request, const struct unix_read *read)
{
    request->io.owner = read->owner;
    request->io.operation = read->operation;
    request->io.before = read->before;
    request->io.after = read->after;
    request->io.length = read->length;
    request->io.message_length = read->message_length;
}

int main(void)
{
    struct unix_service service = {0};
    unix_escrow_init(&service.escrow);
    struct unix_broker_platform platform = { .context = &service,
        .direction_create = direction_new, .direction_destroy = direction_free,
        .external_retain = retain_external, .external_release = release_external };
    service.broker = unix_broker_create(&platform);
    assert(service.broker);
    struct service_session sender = {0}, receiver = {0};
    struct unix_credentials credentials = { .pid = 10, .generation = 1 };
    assert(unix_broker_session_create(service.broker, &credentials, &sender.session) == 0);
    credentials.pid++;
    assert(unix_broker_session_create(service.broker, &credentials, &receiver.session) == 0);
    struct service_thread sender_thread = { .session = &sender, .owner = 1 };
    struct service_thread receiver_thread = { .session = &receiver, .owner = 2, .next = &sender_thread };
    service.threads = &receiver_thread;
    uint64_t pair[2], transferred;
    assert(unix_broker_socketpair(service.broker, sender.session, UNIX_TRANSPORT_SEQPACKET, 0, pair) == 0);
    assert(unix_broker_retain(service.broker, sender.session, pair[1], receiver.session) == 0);
    assert(unix_broker_close(service.broker, sender.session, pair[1]) == 0);
    assert(unix_broker_socket(service.broker, sender.session, UNIX_TRANSPORT_STREAM, 0, &transferred) == 0);
    struct unix_attachment attachment;
    struct unix_direction tx, rx;
    assert(unix_broker_attach(service.broker, sender.session, pair[0], &attachment, &tx, &rx) == 0);
    struct unix_control request = { .operation = UNIX_OP_RIGHTS_PREPARE, .socket = pair[0],
        .io = { .owner = 1 }, .rights = { .operation = 1, .total = 9 } };
    struct response response = {0};
    assert(dispatch_rights(&service, &receiver, &request, NULL, 0, &response) == -EPERM);
    assert(dispatch_rights(&service, &sender, &request, NULL, 0, &response) == 0);
    const uint64_t ticket = request.rights.ticket;
    assert(ticket);
    assert(dispatch_rights(&service, &sender, &request, NULL, 0, &response) == 0);
    assert(request.rights.ticket == ticket);

    request.operation = UNIX_OP_RIGHTS_APPEND;
    request.rights.count = 4;
    for (unsigned offset = 0; offset < 8; offset += 4) {
        request.rights.offset = offset;
        struct pacha_ipc_fd caps[12];
        for (unsigned i = 0; i < 4; i++) request.rights.items[i] = (struct unix_transfer_item){
            .provider = 1, .object = 100 + offset + i, .capability_first = (uint16_t)(3 * i),
            .capability_count = 3, .rights = 5 };
        for (unsigned i = 0; i < 12; i++) caps[i] = new_cap();
        if (!offset) {
            /* A late invalid capability must unwind earlier captured items. */
            native_fds[caps[4].fd].rights = 0;
            assert(dispatch_rights(&service, &sender, &request, caps, 12, &response) == -EBADF);
            assert(!service.escrow.entries && !caps[0].fd && caps[3].fd);
            cleanup(caps, 12);
            for (unsigned i = 0; i < 12; i++) caps[i] = new_cap();
        }
        assert(dispatch_rights(&service, &sender, &request, caps, 12, &response) == 0);
        for (unsigned i = 0; i < 12; i++) assert(!caps[i].fd);
        for (unsigned i = 0; i < 12; i++) caps[i] = new_cap();
        request.rights.items[0].object++;
        assert(dispatch_rights(&service, &sender, &request, caps, 12, &response) == -EINVAL);
        request.rights.items[0].object--;
        assert(dispatch_rights(&service, &sender, &request, caps, 12, &response) == 0);
        for (unsigned i = 0; i < 12; i++) assert(caps[i].fd); /* retry did not adopt replacements */
        cleanup(caps, 12);
    }
    request.rights.offset = 8;
    request.rights.count = 1;
    request.rights.items[0] = (struct unix_transfer_item){ .object = transferred };
    assert(dispatch_rights(&service, &sender, &request, NULL, 0, &response) == 0);
    assert(unix_broker_close(service.broker, sender.session, transferred) == 0);
    struct unix_write write;
    assert(unix_transport_write_begin(tx.tx, tx.rx, attachment.generation, 1, 0, ticket, 1, &write) == 0);
    request.operation = UNIX_OP_RIGHTS_COMMIT;
    request.io.before = write.before; request.io.after = write.after; request.io.length = write.length;
    assert(dispatch_rights(&service, &sender, &request, NULL, 0, &response) == 0);
    assert(dispatch_rights(&service, &sender, &request, NULL, 0, &response) == 0);
    request.operation = UNIX_OP_RIGHTS_ACK;
    assert(dispatch_rights(&service, &sender, &request, NULL, 0, &response) == 0);

    struct unix_read read;
    assert(unix_transport_read_begin(tx.tx, tx.rx, attachment.generation, 2, 0, &read) == 0);
    request.operation = UNIX_OP_RIGHTS_CLAIM;
    request.socket = pair[1]; request.rights.offset = 0; request.rights.count = 16;
    wire_read(&request, &read);
    assert(dispatch_rights(&service, &sender, &request, NULL, 0, &response) == -EPERM);
    assert(dispatch_rights(&service, &receiver, &request, NULL, 0, &response) == 0);
    assert(request.rights.count == 6 && request.rights.total == 9 && response.count == 18);
    assert(request.credentials.pid == 10 && request.result == attachment.generation);
    for (unsigned i = 0; i < 6; i++) {
        assert(request.rights.items[i].object == 100 + i);
        assert(request.rights.items[i].capability_first == 3 * i);
    }
    for (unsigned i = 0; i < response.count; i++)
        assert(!response.capabilities[i].transfer_flags && native_fds[response.capabilities[i].fd].kind);
    request.rights.offset = 6; request.rights.count = 16;
    response = (struct response){0};
    assert(dispatch_rights(&service, &receiver, &request, NULL, 0, &response) == 0);
    assert(request.rights.count == 2 && response.count == 6);
    request.rights.offset = 8; request.rights.count = 16;
    response = (struct response){0};
    assert(dispatch_rights(&service, &receiver, &request, NULL, 0, &response) == 0);
    assert(request.rights.count == 1 && response.count == 0);
    assert(request.rights.items[0].provider == UNIX_TRANSFER_SOCKET && request.rights.items[0].object == transferred);
    assert(request.attachment.socket == transferred && request.attachment.type == UNIX_TRANSPORT_STREAM &&
        !request.attachment.generation);
    assert(unix_broker_retain(service.broker, receiver.session, transferred, receiver.session) == -EBADF);

    /* Caller would native-import the claimed prefix before FINISH. Here
     * take=0 explicitly exercises ordinary-read/CTRUNC discard instead. */
    const unsigned before_finish = closes;
    request.operation = UNIX_OP_RIGHTS_FINISH;
    request.rights.operation = 1; request.rights.count = 0;
    response = (struct response){0};
    assert(dispatch_rights(&service, &receiver, &request, NULL, 0, &response) == 0);
    assert(closes == before_finish + 24 && !service.escrow.entries);
    unix_broker_thread_died(service.broker, 2);
    service.threads = &sender_thread;
    assert(dispatch_rights(&service, &receiver, &request, NULL, 0, &response) == 0);
    request.rights.operation++;
    assert(dispatch_rights(&service, &receiver, &request, NULL, 0, &response) == -ESTALE);
    request.rights.operation--;
    request.operation = UNIX_OP_RIGHTS_ACK; request.rights.flags = UNIX_RIGHTS_RECEIVING;
    assert(dispatch_rights(&service, &receiver, &request, NULL, 0, &response) == 0);
    assert(!unix_broker_ticket_count(service.broker));
    request.operation = UNIX_OP_RIGHTS_FINISH; request.rights.flags = 0;
    assert(dispatch_rights(&service, &receiver, &request, NULL, 0, &response) == -ESTALE);
    unix_broker_destroy(service.broker);
    unix_escrow_destroy(&service.escrow);
    for (int fd = 16; fd < 256; fd++) assert(!native_fds[fd].kind);
    puts("unix service rights: segmented escrow, retries, rollback, authorization, discard, dead-thread receipt passed");
}

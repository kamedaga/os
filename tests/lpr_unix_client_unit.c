/* LPR adapter + shared wire implementation + real broker, with a loopback
 * syscall fixture. This is not native IPC or Linux socket FD integration. */
#include "../userland/personality/linux/runtime/lpr_unix/client.c"
#include "../userland/personality/linux/runtime/lpr_process/client.c"
#include "../userland/unixd/src/broker.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lpr_state_t lpr_state;
void lpr_state_lock(volatile uint32_t *word) { assert(!*word); *word = 1; }
void lpr_state_unlock(volatile uint32_t *word) { assert(*word == 1); *word = 0; }
static struct unix_broker *broker;
static struct unix_session *session;
static _Alignas(struct unix_control) unsigned char unix_page[UNIX_CONTROL_BYTES];
static _Alignas(pacha_service_envelope_t) unsigned char process_page[PACHA_SERVICE_PAGE_BYTES];
static unsigned live[256], maps, calls, interruptions, thread_registrations;
enum { NORMAL, ALLOC_ERROR, MAP_ERROR, CALL_ERROR, RECV_ERROR, BAD_REPLY };
static unsigned mode, pending_caps;
static uint64_t process_request;

void *lpr_memset(void *p, int value, size_t count) { return memset(p, value, count); }
void lpr_linux_process_state_init(void) {}
int64_t lpr_pacha_status_to_errno(int64_t status) { return pacha_kernel_status_to_errno(status); }
void lpr_trace_error_record(uint64_t domain, uint64_t op, uint64_t stage,
    int64_t status, int64_t raw, uint64_t request, uint64_t count,
    uint64_t subject, uint64_t child, const char *text)
{
    (void)domain; (void)op; (void)stage; (void)status; (void)raw;
    (void)request; (void)count; (void)subject; (void)child; (void)text;
}

int lpr_create_standalone_wire_page(void **out)
{
    assert(!live[70]); live[70] = 1; *out = process_page; return 70;
}
void lpr_destroy_standalone_wire_page(int fd, void *page)
{
    assert(fd == 70 && live[70] && page == process_page); live[70] = 0;
}

int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE && fd >= 16 && fd < 256 && live[fd]);
    live[fd] = 0;
    return 0;
}

int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    if (nr == PACHAOS_SYSCALL_FD_GET_INFO) {
        assert(a0 == 80 && live[80]);
        struct pacha_fd_info *info = (struct pacha_fd_info *)(uintptr_t)a1;
        *info = (struct pacha_fd_info){ .kind = PACHA_FD_KIND_CHANNEL,
            .rights = PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL,
            .flags = PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC };
        return 0;
    }
    if (nr == PACHAOS_SYSCALL_MUNMAP) {
        assert(a0 == (uint64_t)(uintptr_t)unix_page && a1 == sizeof(unix_page) && maps == 1);
        maps--; return 0;
    }
    assert(nr == PACHAOS_SYSCALL_IPC_CALL && a0 < 256 && live[a0]);
    const struct pacha_ipc_msg *message = (const struct pacha_ipc_msg *)(uintptr_t)a1;
    if (a0 == 81) {
        assert(message->word0 == PACHA_SERVICE_REQUEST_MAGIC && message->fd_count == 1);
        pacha_service_envelope_t *header = (pacha_service_envelope_t *)process_page;
        assert(header->op == LPRS_OP_PROCESS_UNIX_SESSION && message->fds[0].fd == 70);
        assert(((lprs_token_request_t *)(process_page + PACHA_SERVICE_HEADER_BYTES))->token == 101);
        process_request = header->request_id;
        header->magic = PACHA_SERVICE_REPLY_MAGIC;
        assert(!live[71]); live[71] = 1; return 71;
    }
    assert(a0 == 80 && message->word0 == UNIX_SERVICE_MAGIC);
    assert(message->fd_count == (message->word1 == UNIX_OP_THREAD_REGISTER ? 2u : 1u));
    assert(message->fds[0].fd == 40 && live[40] && maps == 1);
    assert(!(message->fds[0].rights & PACHA_FD_RIGHT_TRANSFER));
    calls++;
    if (mode == CALL_ERROR) return PACHA_SYSCALL_ERR_INVALID;
    struct unix_control *request = (struct unix_control *)unix_page;
    assert(request->request == message->word3 && request->operation == message->word1);
    pending_caps = 0;
    switch (request->operation) {
    case UNIX_OP_THREAD_REGISTER:
        assert(message->fds[1].fd == 50 && live[50]);
        assert(message->fds[1].rights == (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_WAIT |
            PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE));
        assert(message->fds[1].transfer_flags == PACHA_IPC_TRANSFER_PRIVATE);
        request->result = ++thread_registrations;
        break;
    case UNIX_OP_HELLO: request->result = UNIX_SERVICE_VERSION; break;
    case UNIX_OP_SOCKETPAIR: {
        uint64_t pair[2];
        request->status = unix_broker_socketpair(broker, session,
            (uint32_t)request->argument, (uint32_t)request->transaction, pair);
        request->result = pair[0]; request->argument = pair[1];
        break;
    }
    case UNIX_OP_ATTACH: {
        struct unix_attachment attachment;
        struct unix_direction tx, rx;
        request->status = unix_broker_attach(broker, session, request->socket, &attachment, &tx, &rx);
        if (!request->status) {
            request->result = attachment.generation;
            request->credentials = attachment.peer_credentials;
            pending_caps = 4;
        }
        break;
    }
    case UNIX_OP_CLOSE: request->status = unix_broker_close(broker, session, request->socket); break;
    default: assert(!"unexpected broker operation");
    }
    request->magic = UNIX_REPLY_MAGIC;
    assert(!live[41]); live[41] = 1; return 41;
}

int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t bytes, uint64_t rights, uint64_t flags)
{
    assert(nr == PACHAOS_SYSCALL_VMO_CREATE && bytes == sizeof(unix_page) && !live[40]);
    assert((rights & PACHA_FD_RIGHT_TRANSFER) && flags == (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC));
    if (mode == ALLOC_ERROR) return PACHA_SYSCALL_ERR_ALLOC;
    live[40] = 1; return 40;
}

int64_t lpr_pacha_syscall6(uint64_t nr, uint64_t fd, uint64_t address,
    uint64_t bytes, uint64_t prot, uint64_t flags, uint64_t offset)
{
    assert(nr == PACHAOS_SYSCALL_MMAP && fd == 40 && live[40] && !address && !offset);
    assert(bytes == sizeof(unix_page) && prot == (PACHA_PROT_READ | PACHA_PROT_WRITE) && flags == PACHA_MMAP_SHARED);
    if (mode == MAP_ERROR) return PACHA_SYSCALL_ERR_MAP;
    assert(!maps); maps++; return (int64_t)(uintptr_t)unix_page;
}

int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t fd, uint64_t address, uint64_t timeout, uint64_t flags)
{
    if (nr == PACHAOS_SYSCALL_FD_DUP) {
        assert(fd == PACHAOS_THREAD_SELF_FD && address == 16 && !live[50]);
        assert(timeout == (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL |
            PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER));
        assert(flags == (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC));
        live[50] = 1; return 50;
    }
    assert(nr == PACHAOS_SYSCALL_IPC_RECV_WAIT && timeout == UINT64_MAX && !flags && live[fd]);
    struct pacha_ipc_msg *reply = (struct pacha_ipc_msg *)(uintptr_t)address;
    if (fd == 71) {
        assert(reply->fd_capacity == 1 && !live[80]);
        reply->word0 = PACHA_SERVICE_REPLY_MAGIC; reply->word3 = process_request;
        reply->word2 = unix_broker_session_id(session);
        reply->fd_count = 1; reply->fds[0] = (struct pacha_ipc_fd){ .fd = 80 };
        live[80] = 1; return 0;
    }
    assert(fd == 41);
    if (interruptions) { interruptions--; return PACHA_SYSCALL_ERR_NOT_READY; }
    if (mode == RECV_ERROR) return PACHA_SYSCALL_ERR_INVALID;
    const struct unix_control *request = (const struct unix_control *)unix_page;
    reply->word0 = mode == BAD_REPLY ? 0 : UNIX_REPLY_MAGIC;
    reply->word1 = (uint64_t)request->status;
    reply->word2 = request->result; reply->word3 = request->request;
    assert(reply->fd_capacity >= pending_caps);
    reply->fd_count = pending_caps;
    for (unsigned i = 0; i < pending_caps; i++) {
        assert(!live[90 + i]); live[90 + i] = 1;
        reply->fds[i] = (struct pacha_ipc_fd){ .fd = 90 + i, .rights = PACHA_FD_RIGHT_CLOSE };
    }
    return 0;
}

static int direction_create(void *context, uint64_t generation, uint32_t type, struct unix_direction *out)
{
    (void)context;
    out->tx = calloc(1, sizeof(*out->tx)); out->rx = calloc(1, sizeof(*out->rx));
    assert(out->tx && out->rx);
    return unix_transport_init(out->tx, out->rx, generation, type);
}
static void direction_destroy(void *context, struct unix_direction *direction)
{
    (void)context; free(direction->tx); free(direction->rx);
}

int main(void)
{
    struct unix_broker_platform platform = { .direction_create = direction_create, .direction_destroy = direction_destroy };
    broker = unix_broker_create(&platform); assert(broker);
    struct unix_credentials credentials = { .generation = 10, .pid = 123 };
    assert(unix_broker_session_create(broker, &credentials, &session) == 0);
    struct lpr_unix_client client;
    assert(lpr_unix_client_open(&client) == -LPR_LINUX_ENOTCONN);
    lpr_supervisor_enabled = 1; lpr_supervisor_token = 101;
    lpr_process_control_fd = 81; live[81] = 1;
    assert(lpr_unix_client_open(&client) == 0 && client.fd == 80 && client.process_token == 101);
    unsigned received = 0;
    struct unix_control request = { .operation = UNIX_OP_SOCKETPAIR, .request = 1, .argument = UNIX_TRANSPORT_STREAM };
    interruptions = 1;
    assert(lpr_unix_client_call(&client, &request, NULL, 0, NULL, 0, &received) == 0);
    assert(calls == 1 && !interruptions && unix_broker_socket_count(broker) == 2);
    uint64_t owner = 0;
    assert(lpr_unix_client_register_thread(&client, 20, &owner) == 0);
    assert(owner == 1 && thread_registrations == 1 && !live[50]);
    uint64_t pair[2] = { request.result, request.argument };
    struct pacha_ipc_fd caps[4];
    request = (struct unix_control){ .operation = UNIX_OP_ATTACH, .request = 2, .socket = pair[0] };
    assert(lpr_unix_client_call(&client, &request, NULL, 0, caps, 4, &received) == 0);
    assert(received == 4 && request.result && request.credentials.pid == 123);
    for (unsigned i = 0; i < 4; i++) native_close((int)caps[i].fd);
    request = (struct unix_control){ .operation = UNIX_OP_ATTACH, .request = 3, .socket = pair[0] };
    assert(lpr_unix_client_call(&client, &request, NULL, 0, caps, 3, &received) == -ENOBUFS);
    assert(!received);
    for (unsigned i = 90; i < 94; i++) assert(!live[i]);
    mode = BAD_REPLY;
    assert(lpr_unix_client_call(&client, &request, NULL, 0, caps, 4, &received) < 0);
    for (unsigned i = 90; i < 94; i++) assert(!live[i]);
    for (mode = ALLOC_ERROR; mode <= BAD_REPLY; mode++) {
        request = (struct unix_control){ .operation = UNIX_OP_HELLO, .request = 4 };
        assert(lpr_unix_client_call(&client, &request, NULL, 0, NULL, 0, &received) < 0);
        assert(!received && !maps && !live[40] && !live[41] && live[80]);
        assert(lpr_unix_client_register_thread(&client, 21, &owner) < 0);
        assert(!owner && !live[50] && !live[40] && !live[41]);
    }
    mode = NORMAL;
    for (unsigned i = 0; i < 2; i++) {
        request = (struct unix_control){ .operation = UNIX_OP_CLOSE, .request = 5 + i, .socket = pair[i] };
        assert(lpr_unix_client_call(&client, &request, NULL, 0, NULL, 0, &received) == 0);
    }
    assert(!unix_broker_socket_count(broker));
    const uint32_t packet_types[] = { UNIX_TRANSPORT_SEQPACKET, UNIX_TRANSPORT_DGRAM };
    for (unsigned type = 0; type < 2; type++) {
        request = (struct unix_control){ .operation = UNIX_OP_SOCKETPAIR,
            .request = 10 + type * 3, .argument = packet_types[type] };
        assert(lpr_unix_client_call(&client, &request, NULL, 0, NULL, 0, &received) == 0);
        pair[0] = request.result; pair[1] = request.argument;
        assert(pair[0] && pair[1] && pair[0] != pair[1] && unix_broker_socket_count(broker) == 2);
        for (unsigned i = 0; i < 2; i++) {
            request = (struct unix_control){ .operation = UNIX_OP_CLOSE,
                .request = 11 + type * 3 + i, .socket = pair[i] };
            assert(lpr_unix_client_call(&client, &request, NULL, 0, NULL, 0, &received) == 0);
        }
        assert(!unix_broker_socket_count(broker));
    }
    struct lpr_unix_client copied = client;
    lpr_supervisor_token = 102;
    assert(lpr_unix_client_call(&copied, &request, NULL, 0, NULL, 0, &received) == -LPR_LINUX_ENOTCONN);
    lpr_unix_client_close(&copied);
    assert(live[80]); /* child does not close a copied/reused private number */
    lpr_supervisor_token = 101;
    lpr_unix_client_close(&client); native_close(81);
    unix_broker_destroy(broker);
    for (unsigned i = 16; i < 256; i++) assert(!live[i]);
    puts("lpr unix client: session-to-socketpair loopback, attach caps, interrupted reply, errors, fork identity passed");
}

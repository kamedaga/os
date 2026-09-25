#include "socket.h"
#include "../lpr_filed_internal.h"
#include "../support/browser_diag.h"
#include <errno.h>

static void close_caps(struct pacha_ipc_fd *caps, unsigned count)
{
    for (unsigned i = 0; i < count; i++)
        if (caps[i].fd >= 16) (void)lpr_close_native_fd_if_open(caps[i].fd);
}

int lpr_unix_socket_adopt(struct lpr_unix_socket *socket)
{
    if (__atomic_load_n(&socket->process_token, __ATOMIC_ACQUIRE) == lpr_supervisor_token && lpr_supervisor_token)
        return 0;
    lpr_state_lock(&socket->adopt_lock);
    int status = 0, acquired = 0;
    if (socket->process_token == lpr_supervisor_token && lpr_supervisor_token) goto done;
    if (socket->handoff_fd < 16 || !socket->socket) { status = -ENOTCONN; goto done; }
    struct lpr_unix_context *context;
    status = lpr_unix_context_current(&context);
    if (status) goto done;
    struct lpr_unix_client grant = context->client;
    grant.fd = socket->handoff_fd;
    struct unix_control request = { .operation = UNIX_OP_HANDOFF, .socket = socket->socket,
        .argument = context->client.session };
    unsigned count = 0;
    status = lpr_unix_context_next_request(context, &request.request);
    if (!status) status = lpr_unix_client_call(&grant, &request, NULL, 0, NULL, 0, &count);
    if (status) goto done;
    if (!request.result) { status = -EPROTO; goto done; }
    request = (struct unix_control){ .operation = UNIX_OP_HANDOFF, .socket = socket->socket,
        .argument = request.result };
    struct pacha_ipc_fd caps[2] = {{0}};
    status = lpr_unix_context_call(context, &request, NULL, 0, caps, 2, &count);
    if (status) goto done;
    /* Confirmation acquired the reference even if local mapping fails.
     * Keep that ownership; map() can retry without re-importing it. */
    acquired = 1;
    (void)lpr_close_native_fd_if_open((uint32_t)socket->handoff_fd);
    socket->handoff_fd = -1;
    if (request.attachment.socket != socket->socket || request.attachment.type != socket->type ||
        request.attachment.listening > 1 || request.attachment.reserved ||
        (request.attachment.flags & ~UINT32_C(0x800))) {
        close_caps(caps, count); status = -EPROTO; goto done;
    }
    socket->flags = LPR_LINUX_O_RDWR | request.attachment.flags;
    socket->listening = request.attachment.listening;
    if (request.attachment.generation) {
        status = lpr_unix_mapping_import(&request.attachment, caps, count, &socket->mapping);
        if (!status) __atomic_store_n(&socket->mapped, 1, __ATOMIC_RELEASE);
    } else if (count) { close_caps(caps, count); status = -EPROTO; }
done:
    if (acquired) __atomic_store_n(&socket->process_token, lpr_supervisor_token, __ATOMIC_RELEASE);
    lpr_state_unlock(&socket->adopt_lock);
    return status;
}

int lpr_unix_socket_map(struct lpr_unix_socket *socket)
{
    int status = lpr_unix_socket_adopt(socket);
    if (status || __atomic_load_n(&socket->mapped, __ATOMIC_ACQUIRE)) return status;
    if (socket->type == UNIX_TRANSPORT_DGRAM) return -EOPNOTSUPP;
    lpr_state_lock(&socket->adopt_lock);
    if (!socket->mapped) {
        struct lpr_unix_context *context;
        uint64_t request;
        status = lpr_unix_context_current(&context);
        if (!status) status = lpr_unix_context_next_request(context, &request);
        if (!status) status = lpr_unix_mapping_attach(&context->client, socket->socket,
            socket->type, request, &socket->mapping);
        if (!status) __atomic_store_n(&socket->mapped, 1, __ATOMIC_RELEASE);
    }
    lpr_state_unlock(&socket->adopt_lock);
    return status;
}

int lpr_unix_socket_handoff(struct lpr_unix_socket *socket, struct lpr_unix_socket *record)
{
    int status = lpr_unix_socket_adopt(socket);
    if (status) { lpr_browser_diag("handoff-adopt", socket->socket, status, 0); return status; }
    struct lpr_unix_context *context;
    status = lpr_unix_context_current(&context);
    if (status) { lpr_browser_diag("handoff-context", socket->socket, status, 0); return status; }
    struct unix_control request = { .operation = UNIX_OP_HANDOFF, .socket = socket->socket };
    struct pacha_ipc_fd cap = {0};
    unsigned count;
    status = lpr_unix_context_call(context, &request, NULL, 0, &cap, 1, &count);
    if (status) { lpr_browser_diag("handoff-call", socket->socket, status, context->client.fd); return status; }
    struct pacha_fd_info info;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CALL;
    if (count != 1 || !request.result || !lpr_native_fd_info(cap.fd, &info) ||
        info.kind != PACHA_FD_KIND_CHANNEL || info.rights != rights || info.flags) {
        close_caps(&cap, count); return -EPROTO;
    }
    /* No process token, virtual address, native direction FD or lock state
     * crosses the image boundary. The grant is the only native capability. */
    *record = (struct lpr_unix_socket){ .socket = socket->socket, .type = socket->type,
        .flags = socket->flags, .listening = socket->listening,
        .send_timeout_ns = socket->send_timeout_ns, .receive_timeout_ns = socket->receive_timeout_ns,
        .handoff_fd = (int32_t)cap.fd,
        .mapping = { .fds = {-1, -1} } };
    return 0;
}

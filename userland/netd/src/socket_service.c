#include "socket_service.h"

#include "libuinet_backend.h"
#include "netlink_socket.h"
#include "unix_socket.h"
#include "netd/ipc_protocol.h"
#include "pacha/ipc.h"
#include "pacha/trace.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_netd_socket_endpoint_fd = -1;
static uint64_t g_netd_socket_requests;
static uint64_t g_netd_socket_errors;
static int g_netd_socket_trace;
static uint64_t g_netd_socket_recv_bytes;

static void netd_socket_trace_data_op(uint64_t op, int status, uint64_t result)
{
    if (op == NETD_OP_CONNECT || op == NETD_OP_SEND ||
        op == NETD_OP_RECV || op == NETD_OP_POLL) {
        if (op == NETD_OP_RECV && status == 0) {
            g_netd_socket_recv_bytes += result;
            if (result == 0 || g_netd_socket_recv_bytes <= 4096 ||
                (g_netd_socket_recv_bytes % (128u * 1024u)) < result) {
                pacha_trace4(PACHA_TRACE_COMPONENT_NETD, PACHA_TRACE_EVENT_NETD_SOCKET, PACHA_TRACE_CLASS_IO, op, (uint64_t)status, result, g_netd_socket_recv_bytes);
            }
            return;
        }
        if (op == NETD_OP_CONNECT || (status != 0 && status != -11)) {
            pacha_trace3(PACHA_TRACE_COMPONENT_NETD, PACHA_TRACE_EVENT_NETD_SOCKET, status != 0 ? PACHA_TRACE_CLASS_ERROR : PACHA_TRACE_CLASS_IO, op, (uint64_t)status, result);
        }
    }
}

static int netd_socket_send_reply(
    uint64_t op,
    int reply_fd,
    uint64_t request_id,
    int64_t status,
    uint64_t result,
    int transfer_fd)
{
    struct pacha_ipc_fd transferred = {
        .fd = (uint64_t)(uint32_t)transfer_fd,
        .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_SET_FLAGS | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_RECV |
            PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL,
        .transfer_flags = PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_INHERIT,
    };
    const struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = result,
        .word3 = request_id,
        .fds = transfer_fd >= 16 ? &transferred : NULL,
        .fd_count = transfer_fd >= 16 ? 1u : 0u,
    };
    if (g_netd_socket_trace && (op == NETD_OP_CONNECT || op == NETD_OP_POLL)) {
        pacha_trace4(PACHA_TRACE_COMPONENT_NETD, PACHA_TRACE_EVENT_NETD_SOCKET, PACHA_TRACE_CLASS_DEBUG, op, (uint64_t)status, result, (uint64_t)(uint32_t)reply_fd);
    }
    const int reply_status = pacha_ipc_reply(reply_fd, &reply);
    if (g_netd_socket_trace && (op == NETD_OP_CONNECT || op == NETD_OP_POLL)) {
        pacha_trace4(PACHA_TRACE_COMPONENT_NETD, PACHA_TRACE_EVENT_NETD_SOCKET, reply_status != 0 ? PACHA_TRACE_CLASS_ERROR : PACHA_TRACE_CLASS_DEBUG, op, (uint64_t)reply_status, result, (uint64_t)(uint32_t)reply_fd);
    }
    (void)pacha_fd_close(reply_fd);
    if (transfer_fd >= 16) (void)pacha_fd_close(transfer_fd);
    return reply_status;
}

static void *netd_socket_map_page(int page_fd)
{
    if (page_fd < 16) {
        return NULL;
    }
    return pacha_mmap(
        page_fd,
        NETD_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
}

static int netd_socket_dispatch(
    uint64_t op,
    void *page,
    int transferred_fd,
    uint64_t *out_result,
    int *out_reply_fd)
{
    if (out_result == NULL) {
        return -22;
    }

    switch (op) {
    case NETD_OP_HELLO:
        *out_result = 0;
        return 0;
    case NETD_OP_SOCKET: {
        if (page == NULL) {
            return -22;
        }
        const netd_socket_t *req = (const netd_socket_t *)page;
        if (req->domain == NETD_AF_UNIX) {
            return netd_unix_socket_open(req->type, req->protocol, transferred_fd, out_result);
        }
        if (req->domain == NETD_AF_NETLINK) {
            return netd_netlink_socket_open(req->type, req->protocol, transferred_fd, out_result);
        }
        return netd_libuinet_socket_open(req->domain, req->type, req->protocol, out_result);
    }
    case NETD_OP_CONNECT: {
        if (page == NULL) {
            return -22;
        }
        const netd_connect_t *req = (const netd_connect_t *)page;
        *out_result = 0;
        if (netd_unix_socket_is_handle(req->handle)) {
            return netd_unix_socket_connect((const netd_unix_path_t *)page);
        }
        return netd_libuinet_socket_connect(req->handle, req->addr.addr_be, req->addr.port_be, req->flags);
    }
    case NETD_OP_SEND: {
        if (page == NULL) {
            return -22;
        }
        const netd_io_t *req = (const netd_io_t *)page;
        if (req->length > NETD_IO_BYTES) {
            return -22;
        }
        size_t sent = 0;
        int status = netd_netlink_socket_is_handle(req->handle) ? -95 :
            netd_unix_socket_is_handle(req->handle) ? netd_unix_socket_send(req, transferred_fd, &sent) : netd_libuinet_socket_send(
            req->handle,
            req->data,
            (size_t)req->length,
            req->flags,
            req->addr.addr_be,
            req->addr.port_be,
            &sent);
        *out_result = sent;
        return status;
    }
    case NETD_OP_RECV: {
        if (page == NULL) {
            return -22;
        }
        netd_io_t *req = (netd_io_t *)page;
        size_t capacity = (size_t)req->length;
        if (capacity > NETD_IO_BYTES) {
            capacity = NETD_IO_BYTES;
        }
        size_t received = 0;
        int status = netd_netlink_socket_is_handle(req->handle) ?
            netd_netlink_socket_recv(req, capacity, &received) :
            netd_unix_socket_is_handle(req->handle) ? netd_unix_socket_recv(req, capacity, out_reply_fd, &received) :
            netd_libuinet_socket_recv(req->handle, req->data, capacity, req->flags, &received);
        req->length = received;
        *out_result = received;
        return status;
    }
    case NETD_OP_POLL: {
        if (page == NULL) {
            return -22;
        }
        netd_poll_t *req = (netd_poll_t *)page;
        uint32_t revents = 0;
        int32_t error = 0;
        int status = netd_netlink_socket_is_handle(req->handle) ?
            netd_netlink_socket_poll(req->handle, req->events, &revents, &error) :
            netd_unix_socket_is_handle(req->handle) ? netd_unix_socket_poll(req->handle, req->events, &revents, &error) :
            netd_libuinet_socket_poll(req->handle, req->events, &revents, &error);
        req->revents = revents;
        req->error = error;
        *out_result = revents;
        return status;
    }
    case NETD_OP_CLOSE:
        {
            uint64_t handle = *out_result;
            *out_result = 0;
            return netd_netlink_socket_is_handle(handle) ? netd_netlink_socket_close(handle) :
                netd_unix_socket_is_handle(handle) ? netd_unix_socket_close(handle) : netd_libuinet_socket_close(handle);
        }
    case NETD_OP_DUP:
        {
            const uint64_t handle = *out_result;
            *out_result = 0;
            return netd_unix_socket_is_handle(handle) ?
                netd_unix_socket_dup(handle) : -95;
        }
    case NETD_OP_BIND:
        if (page == NULL) return -22;
        return netd_netlink_socket_is_handle(((const netd_netlink_bind_t *)page)->handle) ?
            netd_netlink_socket_bind((const netd_netlink_bind_t *)page) :
            netd_unix_socket_bind((const netd_unix_path_t *)page);
    case NETD_OP_LISTEN:
        return netd_unix_socket_listen(*out_result);
    case NETD_OP_ACCEPT:
        if (page == NULL) return -22;
        {
            netd_accept_t *req = (netd_accept_t *)page;
            const int status = netd_unix_socket_accept(req);
            *out_result = status == 0 ? req->accepted_handle : 0;
            return status;
        }
    case NETD_OP_ATTACH_WAIT:
        if (page == NULL) return -22;
        return netd_unix_socket_attach_wait(
            ((const netd_accept_t *)page)->handle,
            transferred_fd);
    case NETD_OP_UEVENT_PUBLISH:
        {
            const uint64_t device = *out_result;
            *out_result = 0;
            return netd_netlink_publish_device(device);
        }
    default:
        return -38;
    }
}

static int netd_socket_dispatch_request(const struct pacha_ipc_msg *request, const struct pacha_ipc_fd *fds)
{
    if (request == NULL) {
        return -22;
    }
    if (request->fd_count < 1 || fds == NULL || fds[request->fd_count - 1].fd < 16) {
        return -22;
    }

    const int reply_fd = (int)fds[request->fd_count - 1].fd;
    if (request->word0 != PACHA_SERVICE_REQUEST_MAGIC || request->word3 == 0) {
        return netd_socket_send_reply(request->word1, reply_fd, request->word3, -22, 0, -1);
    }
    int page_fd = -1;
    if (request->fd_count >= 2 && fds[0].fd >= 16) {
        page_fd = (int)fds[0].fd;
    }
    const int transferred_fd =
        (request->word1 == NETD_OP_SOCKET || request->word1 == NETD_OP_SEND ||
            request->word1 == NETD_OP_ATTACH_WAIT) && request->fd_count >= 3 ?
        (int)(uint32_t)fds[1].fd : -1;

    void *page = NULL;
    if (page_fd >= 16) {
        page = netd_socket_map_page(page_fd);
        if (page == NULL) {
            (void)pacha_fd_close(page_fd);
            if (transferred_fd >= 16) (void)pacha_fd_close(transferred_fd);
            return netd_socket_send_reply(request->word1, reply_fd, request->word3, -5, 0, -1);
        }
    }

    const int send_has_fd = request->word1 == NETD_OP_SEND && page != NULL &&
        netd_unix_socket_is_handle(((const netd_io_t *)page)->handle) &&
        ((const netd_io_t *)page)->fd_kind != 0;
    uint64_t result = request->word2;
    int reply_transfer_fd = -1;
    const int status = netd_socket_dispatch(
        request->word1, page, transferred_fd, &result, &reply_transfer_fd);
    if (g_netd_socket_trace) {
        netd_socket_trace_data_op(request->word1, status, result);
    }
    if (g_netd_socket_trace || (status != 0 && status != -11 && status != -115)) {
        pacha_trace4(
            PACHA_TRACE_COMPONENT_NETD,
            PACHA_TRACE_EVENT_NETD_SOCKET,
            status != 0 ? PACHA_TRACE_CLASS_ERROR : PACHA_TRACE_CLASS_DEBUG,
            request->word1,
            (uint64_t)status,
            result,
            request->fd_count);
    }
    const int transfer_retained = status == 0 &&
        ((request->word1 == NETD_OP_SOCKET &&
            (netd_netlink_socket_is_handle(result) || netd_unix_socket_is_handle(result))) ||
         request->word1 == NETD_OP_ATTACH_WAIT ||
         send_has_fd);
    if (page != NULL) {
        (void)pacha_munmap(page, NETD_PAGE_BYTES);
    }
    if (page_fd >= 16 && page_fd != reply_fd) {
        (void)pacha_fd_close(page_fd);
    }
    if (transferred_fd >= 16 && !transfer_retained)
    {
        (void)pacha_fd_close(transferred_fd);
    }
    if (status != 0) {
        g_netd_socket_errors++;
    }
    return netd_socket_send_reply(
        request->word1, reply_fd, request->word3, status, result, reply_transfer_fd);
}

int netd_socket_service_start(struct netd_runtime *runtime)
{
    if (runtime == NULL || runtime->cfg == NULL) {
        return 8;
    }
    g_netd_socket_endpoint_fd = (int)runtime->cfg->socket_endpoint_fd;
    g_netd_socket_requests = 0;
    g_netd_socket_errors = 0;
    g_netd_socket_trace = (runtime->cfg->flags & NETD_BOOT_FLAG_TRACE) != 0;
    if (g_netd_socket_endpoint_fd < 16) {
        pacha_trace1(PACHA_TRACE_COMPONENT_NETD, PACHA_TRACE_EVENT_NETD_SOCKET, PACHA_TRACE_CLASS_ERROR, (uint64_t)(uint32_t)g_netd_socket_endpoint_fd);
        return 8;
    }
    pacha_trace1(PACHA_TRACE_COMPONENT_NETD, PACHA_TRACE_EVENT_NETD_SOCKET, PACHA_TRACE_CLASS_STATE, (uint64_t)(uint32_t)g_netd_socket_endpoint_fd);
    return 0;
}

void netd_socket_service_poll(void)
{
    if (g_netd_socket_endpoint_fd < 16) {
        return;
    }

    for (unsigned i = 0; i < 32; i++) {
        struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
        struct pacha_ipc_msg request;
        memset(fds, 0, sizeof(fds));
        memset(&request, 0, sizeof(request));
        request.fds = fds;
        request.fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS;

        int status = pacha_ipc_recv(g_netd_socket_endpoint_fd, &request);
        if (status != 0) {
            break;
        }
        g_netd_socket_requests++;
        (void)netd_socket_dispatch_request(&request, fds);
    }
}

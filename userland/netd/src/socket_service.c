#include "socket_service.h"

#include "libuinet_backend.h"
#include "netd/ipc_protocol.h"
#include "pacha/ipc.h"

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
    if (op == NETD_WIRE_OP_CONNECT || op == NETD_WIRE_OP_SEND ||
        op == NETD_WIRE_OP_RECV || op == NETD_WIRE_OP_POLL) {
        if (op == NETD_WIRE_OP_RECV && status == 0) {
            g_netd_socket_recv_bytes += result;
            if (result == 0 || g_netd_socket_recv_bytes <= 4096 ||
                (g_netd_socket_recv_bytes % (128u * 1024u)) < result) {
                printf("[netd] socket recv status=%d bytes=%llu total=%llu\n",
                    status,
                    (unsigned long long)result,
                    (unsigned long long)g_netd_socket_recv_bytes);
            }
            return;
        }
        if (op == NETD_WIRE_OP_CONNECT || (status != 0 && status != -11)) {
            printf("[netd] socket op=%llu status=%d result=%llu\n",
                (unsigned long long)op,
                status,
                (unsigned long long)result);
        }
    }
}

static int netd_socket_send_reply(uint64_t op, int reply_fd, uint64_t request_id, int64_t status, uint64_t result)
{
    const struct pacha_ipc_msg reply = {
        .word0 = NETD_WIRE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = result,
        .word3 = request_id,
    };
    if (g_netd_socket_trace && (op == NETD_WIRE_OP_CONNECT || op == NETD_WIRE_OP_POLL)) {
        printf("[netd] socket reply begin op=%llu status=%lld result=%llu reply_fd=%d\n",
            (unsigned long long)op,
            (long long)status,
            (unsigned long long)result,
            reply_fd);
        fflush(stdout);
    }
    const int reply_status = pacha_ipc_reply(reply_fd, &reply);
    if (g_netd_socket_trace && (op == NETD_WIRE_OP_CONNECT || op == NETD_WIRE_OP_POLL)) {
        printf("[netd] socket reply end op=%llu status=%d result=%llu reply_fd=%d\n",
            (unsigned long long)op,
            reply_status,
            (unsigned long long)result,
            reply_fd);
        fflush(stdout);
    }
    (void)pacha_fd_close(reply_fd);
    return reply_status;
}

static void *netd_socket_map_page(int page_fd)
{
    if (page_fd < 16) {
        return NULL;
    }
    return pacha_mmap(
        page_fd,
        NETD_WIRE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
}

static int netd_socket_dispatch(uint64_t op, void *page, uint64_t *out_result)
{
    if (out_result == NULL) {
        return -22;
    }

    switch (op) {
    case NETD_WIRE_OP_HELLO:
        *out_result = 0;
        return 0;
    case NETD_WIRE_OP_SOCKET: {
        if (page == NULL) {
            return -22;
        }
        const netd_wire_socket_t *req = (const netd_wire_socket_t *)page;
        return netd_libuinet_socket_open(req->domain, req->type, req->protocol, out_result);
    }
    case NETD_WIRE_OP_CONNECT: {
        if (page == NULL) {
            return -22;
        }
        const netd_wire_connect_t *req = (const netd_wire_connect_t *)page;
        *out_result = 0;
        return netd_libuinet_socket_connect(req->handle, req->addr.addr_be, req->addr.port_be, req->flags);
    }
    case NETD_WIRE_OP_SEND: {
        if (page == NULL) {
            return -22;
        }
        const netd_wire_io_t *req = (const netd_wire_io_t *)page;
        if (req->length > NETD_WIRE_IO_BYTES) {
            return -22;
        }
        size_t sent = 0;
        int status = netd_libuinet_socket_send(
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
    case NETD_WIRE_OP_RECV: {
        if (page == NULL) {
            return -22;
        }
        netd_wire_io_t *req = (netd_wire_io_t *)page;
        size_t capacity = (size_t)req->length;
        if (capacity > NETD_WIRE_IO_BYTES) {
            capacity = NETD_WIRE_IO_BYTES;
        }
        size_t received = 0;
        int status = netd_libuinet_socket_recv(req->handle, req->data, capacity, req->flags, &received);
        req->length = received;
        *out_result = received;
        return status;
    }
    case NETD_WIRE_OP_POLL: {
        if (page == NULL) {
            return -22;
        }
        netd_wire_poll_t *req = (netd_wire_poll_t *)page;
        uint32_t revents = 0;
        int32_t error = 0;
        int status = netd_libuinet_socket_poll(req->handle, req->events, &revents, &error);
        req->revents = revents;
        req->error = error;
        *out_result = revents;
        return status;
    }
    case NETD_WIRE_OP_CLOSE:
        {
            uint64_t handle = *out_result;
            *out_result = 0;
            return netd_libuinet_socket_close(handle);
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
    if (request->word0 != NETD_WIRE_REQUEST_MAGIC || request->word3 == 0) {
        return netd_socket_send_reply(request->word1, reply_fd, request->word3, -22, 0);
    }
    int page_fd = -1;
    if (request->fd_count >= 2 && fds[0].fd >= 16) {
        page_fd = (int)fds[0].fd;
    }

    void *page = NULL;
    if (page_fd >= 16) {
        page = netd_socket_map_page(page_fd);
        if (page == NULL) {
            (void)pacha_fd_close(page_fd);
            return netd_socket_send_reply(request->word1, reply_fd, request->word3, -5, 0);
        }
    }

    uint64_t result = request->word2;
    const int status = netd_socket_dispatch(request->word1, page, &result);
    if (g_netd_socket_trace) {
        netd_socket_trace_data_op(request->word1, status, result);
    }
    if (g_netd_socket_trace || (status != 0 && status != -11 && status != -115)) {
        printf("[netd] socket request op=%llu status=%d result=%llu fds=%llu\n",
            (unsigned long long)request->word1,
            status,
            (unsigned long long)result,
            (unsigned long long)request->fd_count);
    }
    if (page != NULL) {
        (void)pacha_munmap(page, NETD_WIRE_PAGE_BYTES);
    }
    if (page_fd >= 16 && page_fd != reply_fd) {
        (void)pacha_fd_close(page_fd);
    }
    if (status != 0) {
        g_netd_socket_errors++;
    }
    return netd_socket_send_reply(request->word1, reply_fd, request->word3, status, result);
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
        fprintf(stderr, "[netd] socket service endpoint missing fd=%d\n", g_netd_socket_endpoint_fd);
        return 8;
    }
    printf("[netd] socket service ready endpoint_fd=%d\n", g_netd_socket_endpoint_fd);
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

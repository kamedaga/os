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
#include <stdlib.h>
#include <string.h>

#ifndef NETD_DBUS_DIAG
#define NETD_DBUS_DIAG 0
#endif

static int g_netd_socket_endpoint_fd = -1;
static uint64_t g_netd_socket_requests;
static uint64_t g_netd_socket_errors;
static int g_netd_socket_trace;
static uint64_t g_netd_socket_recv_bytes;

struct netd_socket_transfer_lease {
    struct netd_socket_transfer_lease *next;
    uint64_t handle;
    int lease_fd;
};

static struct netd_socket_transfer_lease *g_netd_socket_transfer_leases;

enum {
    NETD_PAGE_ATTACHMENT_MAX = 64,
};

#define NETD_PAGE_ATTACHMENT_TAG UINT64_C(0x4e50000000000000)
#define NETD_PAGE_ATTACHMENT_COUNTER_MASK UINT64_C(0x0000ffffffffffff)

struct netd_page_attachment {
    struct netd_page_attachment *next;
    uint64_t id;
    uint64_t wait_index;
    int page_fd;
    int lease_fd;
    void *page;
};

static struct netd_page_attachment *g_netd_page_attachments;
static uint64_t g_netd_page_attachment_next = 1;
static uint64_t g_netd_page_attachment_count;

static int netd_socket_handle_dup(uint64_t handle)
{
    return netd_netlink_socket_is_handle(handle) ?
        netd_netlink_socket_dup(handle) :
        netd_unix_socket_is_handle(handle) ?
            netd_unix_socket_dup(handle) :
            netd_libuinet_socket_dup(handle);
}

static int netd_socket_handle_close(uint64_t handle)
{
    return netd_netlink_socket_is_handle(handle) ?
        netd_netlink_socket_close(handle) :
        netd_unix_socket_is_handle(handle) ?
            netd_unix_socket_close(handle) :
            netd_libuinet_socket_close(handle);
}

static void netd_socket_transfer_lease_destroy(
    struct netd_socket_transfer_lease *lease)
{
    if (lease == NULL) return;
    if (lease->lease_fd >= 16)
        (void)pacha_fd_close(lease->lease_fd);
    if (lease->handle != 0)
        (void)netd_socket_handle_close(lease->handle);
    free(lease);
}

static int netd_socket_transfer_lease_add(uint64_t handle, int lease_fd)
{
    if (handle == 0 || lease_fd < 16) return -22;
    struct netd_socket_transfer_lease *lease =
        calloc(1, sizeof(*lease));
    if (lease == NULL) return -12;
    const int status = netd_socket_handle_dup(handle);
    if (status != 0) {
        free(lease);
        return status;
    }
    lease->handle = handle;
    lease->lease_fd = lease_fd;
    lease->next = g_netd_socket_transfer_leases;
    g_netd_socket_transfer_leases = lease;
    return 0;
}

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
    const int *transfer_fds,
    uint32_t transfer_count)
{
    struct pacha_ipc_fd transferred[NETD_TRANSFER_MAX_CAPABILITIES];
    if (transfer_count > NETD_TRANSFER_MAX_CAPABILITIES) {
        (void)pacha_fd_close(reply_fd);
        return -22;
    }
    memset(transferred, 0, sizeof(transferred));
    for (uint32_t i = 0; i < transfer_count; ++i) {
        if (transfer_fds == NULL || transfer_fds[i] < 16) {
            (void)pacha_fd_close(reply_fd);
            for (uint32_t j = 0; transfer_fds != NULL && j < transfer_count; ++j)
                if (transfer_fds[j] >= 16) (void)pacha_fd_close(transfer_fds[j]);
            return -22;
        }
        struct pacha_fd_info info;
        memset(&info, 0, sizeof(info));
        if (pacha_fd_get_info(transfer_fds[i], &info) != 0 ||
            (info.rights & PACHA_FD_RIGHT_TRANSFER) == 0)
        {
            (void)pacha_fd_close(reply_fd);
            for (uint32_t j = 0; j < transfer_count; ++j)
                if (transfer_fds[j] >= 16) (void)pacha_fd_close(transfer_fds[j]);
            return -9;
        }
        transferred[i].fd = (uint64_t)(uint32_t)transfer_fds[i];
        transferred[i].rights = info.rights;
        transferred[i].transfer_flags = PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_INHERIT;
    }
    const struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = result,
        .word3 = request_id,
        .fds = transfer_count != 0 ? transferred : NULL,
        .fd_count = transfer_count,
    };
    if (g_netd_socket_trace && (op == NETD_OP_CONNECT || op == NETD_OP_POLL)) {
        pacha_trace4(PACHA_TRACE_COMPONENT_NETD, PACHA_TRACE_EVENT_NETD_SOCKET, PACHA_TRACE_CLASS_DEBUG, op, (uint64_t)status, result, (uint64_t)(uint32_t)reply_fd);
    }
    const int reply_status = pacha_ipc_reply(reply_fd, &reply);
    if (g_netd_socket_trace && (op == NETD_OP_CONNECT || op == NETD_OP_POLL)) {
        pacha_trace4(PACHA_TRACE_COMPONENT_NETD, PACHA_TRACE_EVENT_NETD_SOCKET, reply_status != 0 ? PACHA_TRACE_CLASS_ERROR : PACHA_TRACE_CLASS_DEBUG, op, (uint64_t)reply_status, result, (uint64_t)(uint32_t)reply_fd);
    }
    (void)pacha_fd_close(reply_fd);
    for (uint32_t i = 0; i < transfer_count; ++i)
        (void)pacha_fd_close(transfer_fds[i]);
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

static struct netd_page_attachment *netd_page_attachment_find(uint64_t id)
{
    if ((id & ~NETD_PAGE_ATTACHMENT_COUNTER_MASK) !=
        NETD_PAGE_ATTACHMENT_TAG)
        return NULL;
    for (struct netd_page_attachment *attachment =
             g_netd_page_attachments;
         attachment != NULL;
         attachment = attachment->next)
        if (attachment->id == id) return attachment;
    return NULL;
}

static void netd_page_attachment_destroy(
    struct netd_page_attachment *attachment)
{
    if (attachment == NULL) return;
    if (attachment->page != NULL)
        (void)pacha_munmap(attachment->page, NETD_PAGE_BYTES);
    if (attachment->page_fd >= 16)
        (void)pacha_fd_close(attachment->page_fd);
    if (attachment->lease_fd >= 16)
        (void)pacha_fd_close(attachment->lease_fd);
    free(attachment);
}

static int netd_page_attachment_add(
    int page_fd,
    int lease_fd,
    uint64_t *out_id)
{
    if (page_fd < 16 || lease_fd < 16 || out_id == NULL) return -22;
    if (g_netd_page_attachment_count >= NETD_PAGE_ATTACHMENT_MAX) return -24;

    struct pacha_fd_info page_info;
    struct pacha_fd_info lease_info;
    memset(&page_info, 0, sizeof(page_info));
    memset(&lease_info, 0, sizeof(lease_info));
    if (pacha_fd_get_info(page_fd, &page_info) != 0 ||
        page_info.kind != PACHA_FD_KIND_VMO ||
        (page_info.rights & (PACHA_FD_RIGHT_MAP_READ |
             PACHA_FD_RIGHT_MAP_WRITE)) !=
            (PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE) ||
        pacha_fd_get_info(lease_fd, &lease_info) != 0 ||
        lease_info.kind != PACHA_FD_KIND_CHANNEL ||
        (lease_info.rights & PACHA_FD_RIGHT_WAIT) == 0)
        return -9;

    struct netd_page_attachment *attachment =
        calloc(1, sizeof(*attachment));
    if (attachment == NULL) return -12;
    void *page = netd_socket_map_page(page_fd);
    if (page == NULL) {
        free(attachment);
        return -5;
    }

    uint64_t counter =
        g_netd_page_attachment_next++ & NETD_PAGE_ATTACHMENT_COUNTER_MASK;
    if (counter == 0)
        counter =
            g_netd_page_attachment_next++ & NETD_PAGE_ATTACHMENT_COUNTER_MASK;
    const uint64_t id = NETD_PAGE_ATTACHMENT_TAG | counter;
    if (netd_page_attachment_find(id) != NULL) {
        (void)pacha_munmap(page, NETD_PAGE_BYTES);
        free(attachment);
        return -11;
    }

    attachment->id = id;
    attachment->page_fd = page_fd;
    attachment->lease_fd = lease_fd;
    attachment->page = page;
    attachment->next = g_netd_page_attachments;
    g_netd_page_attachments = attachment;
    g_netd_page_attachment_count++;
    *out_id = id;
    return 0;
}

static int netd_socket_dispatch(
    uint64_t op,
    void *page,
    const int *transferred_fds,
    uint32_t transferred_count,
    uint64_t *out_result,
    int *out_reply_fds,
    uint32_t *out_reply_count)
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
            return netd_unix_socket_open(
                req->type, req->protocol,
                transferred_count == 1 ? transferred_fds[0] : -1,
                out_result);
        }
        if (req->domain == NETD_AF_NETLINK) {
            return netd_netlink_socket_open(
                req->type, req->protocol,
                transferred_count == 1 ? transferred_fds[0] : -1,
                out_result);
        }
        return netd_libuinet_socket_open(
            req->domain, req->type, req->protocol,
            transferred_count == 1 ? transferred_fds[0] : -1,
            out_result);
    }
    case NETD_OP_SOCKETPAIR: {
        if (page == NULL || transferred_count != 2) {
            return -22;
        }
        netd_socket_pair_t *req = (netd_socket_pair_t *)page;
        if (req->domain != NETD_AF_UNIX) {
            return -97;
        }
        return netd_unix_socket_pair(
            req->type, req->protocol,
            transferred_fds[0], transferred_fds[1],
            req->handles);
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
            netd_unix_socket_is_handle(req->handle) ? netd_unix_socket_send(
                req, transferred_fds, transferred_count, &sent) : netd_libuinet_socket_send(
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
            netd_unix_socket_is_handle(req->handle) ? netd_unix_socket_recv(
                req, capacity, out_reply_fds,
                NETD_TRANSFER_MAX_CAPABILITIES, out_reply_count, &received) :
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
            return netd_socket_handle_close(handle);
        }
    case NETD_OP_DUP:
        {
            const uint64_t handle = *out_result;
            *out_result = 0;
            if (transferred_count > 1) return -22;
            return transferred_count == 1 ?
                netd_socket_transfer_lease_add(handle, transferred_fds[0]) :
                netd_socket_handle_dup(handle);
        }
    case NETD_OP_BIND:
        if (page == NULL) return -22;
        return netd_netlink_socket_is_handle(((const netd_netlink_bind_t *)page)->handle) ?
            netd_netlink_socket_bind((const netd_netlink_bind_t *)page) :
            netd_unix_socket_bind((const netd_unix_path_t *)page);
    case NETD_OP_LISTEN:
        if (page == NULL) return -22;
        return netd_unix_socket_listen((const netd_listen_t *)page);
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
            transferred_count == 1 ? transferred_fds[0] : -1);
    case NETD_OP_UNIX_NAME:
        if (page == NULL) return -22;
        return netd_unix_socket_name((netd_unix_name_t *)page);
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

static int netd_socket_dispatch_request(
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds)
{
    if (request == NULL) {
        return -22;
    }
    if (request->fd_count < 1 || fds == NULL || fds[request->fd_count - 1].fd < 16) {
        return -22;
    }

    const int reply_fd = (int)fds[request->fd_count - 1].fd;
    if (request->word0 != PACHA_SERVICE_REQUEST_MAGIC || request->word3 == 0) {
        return netd_socket_send_reply(request->word1, reply_fd, request->word3, -22, 0, NULL, 0);
    }

    if (request->word1 == NETD_OP_PAGE_ATTACH) {
        if (request->fd_count != 3 || fds[0].fd < 16 || fds[1].fd < 16) {
            for (uint64_t i = 0; i + 1 < request->fd_count; ++i)
                if (fds[i].fd >= 16) (void)pacha_fd_close((int)fds[i].fd);
            return netd_socket_send_reply(
                request->word1, reply_fd, request->word3, -22, 0, NULL, 0);
        }
        const int page_fd = (int)fds[0].fd;
        const int lease_fd = (int)fds[1].fd;
        uint64_t attachment_id = 0;
        const int status = netd_page_attachment_add(
            page_fd, lease_fd, &attachment_id);
        if (status != 0) {
            (void)pacha_fd_close(page_fd);
            (void)pacha_fd_close(lease_fd);
            g_netd_socket_errors++;
        }
        return netd_socket_send_reply(
            request->word1,
            reply_fd,
            request->word3,
            status,
            attachment_id,
            NULL,
            0);
    }

    const int op_uses_page =
        request->word1 == NETD_OP_SOCKET ||
        request->word1 == NETD_OP_SOCKETPAIR ||
        request->word1 == NETD_OP_CONNECT ||
        request->word1 == NETD_OP_SEND ||
        request->word1 == NETD_OP_RECV ||
        request->word1 == NETD_OP_POLL ||
        request->word1 == NETD_OP_BIND ||
        request->word1 == NETD_OP_LISTEN ||
        request->word1 == NETD_OP_ACCEPT ||
        request->word1 == NETD_OP_ATTACH_WAIT ||
        request->word1 == NETD_OP_UNIX_NAME;
    struct netd_page_attachment *attachment = op_uses_page ?
        netd_page_attachment_find(request->word2) : NULL;
    if (op_uses_page && attachment == NULL) {
        for (uint64_t i = 0; i + 1 < request->fd_count; ++i)
            if (fds[i].fd >= 16) (void)pacha_fd_close((int)fds[i].fd);
        g_netd_socket_errors++;
        return netd_socket_send_reply(
            request->word1,
            reply_fd,
            request->word3,
            NETD_STATUS_STALE_ATTACHMENT,
            0,
            NULL,
            0);
    }

    int transferred_fds[NETD_TRANSFER_MAX_CAPABILITIES];
    memset(transferred_fds, 0xff, sizeof(transferred_fds));
    const uint64_t transferred_count64 = request->fd_count - 1u;
    if (transferred_count64 > NETD_TRANSFER_MAX_CAPABILITIES) {
        return netd_socket_send_reply(
            request->word1, reply_fd, request->word3, -22, 0, NULL, 0);
    }
    const uint32_t transferred_count = (uint32_t)transferred_count64;
    for (uint32_t i = 0; i < transferred_count; ++i)
        transferred_fds[i] = (int)(uint32_t)fds[i].fd;

    void *const page = attachment != NULL ? attachment->page : NULL;
    if (page != NULL) __atomic_thread_fence(__ATOMIC_ACQUIRE);

#if NETD_DBUS_DIAG
    const uint64_t diag_handle = page != NULL &&
        (request->word1 == NETD_OP_SEND ||
         request->word1 == NETD_OP_RECV ||
         request->word1 == NETD_OP_POLL) ?
        ((const netd_io_t *)page)->handle : 0;
    const int diag_dbus = netd_unix_socket_is_handle(diag_handle) &&
        netd_unix_socket_diag_dbus(diag_handle);
    if (diag_dbus) {
        printf("[netd-dbus-rpc] phase=dispatch-enter id=%llu op=%llu handle=%llu reply_fd=%d\n",
            (unsigned long long)request->word3,
            (unsigned long long)request->word1,
            (unsigned long long)diag_handle,
            reply_fd);
    }
#endif

    const int send_has_transfer = request->word1 == NETD_OP_SEND && page != NULL &&
        netd_unix_socket_is_handle(((const netd_io_t *)page)->handle) &&
        ((const netd_io_t *)page)->transfer_count != 0;
    uint64_t result = op_uses_page ? 0 : request->word2;
    int reply_transfer_fds[NETD_TRANSFER_MAX_CAPABILITIES];
    uint32_t reply_transfer_count = 0;
    memset(reply_transfer_fds, 0xff, sizeof(reply_transfer_fds));
    netd_unix_socket_set_notifications_deferred(1);
    const int status = netd_socket_dispatch(
        request->word1, page, transferred_fds, transferred_count, &result,
        reply_transfer_fds, &reply_transfer_count);
#if NETD_DBUS_DIAG
    if (diag_dbus) {
        printf("[netd-dbus-rpc] phase=dispatch-exit id=%llu op=%llu handle=%llu status=%d result=%llu\n",
            (unsigned long long)request->word3,
            (unsigned long long)request->word1,
            (unsigned long long)diag_handle,
            status,
            (unsigned long long)result);
    }
#endif
    if (page != NULL) __atomic_thread_fence(__ATOMIC_RELEASE);
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
            result != 0) ||
         request->word1 == NETD_OP_SOCKETPAIR ||
         request->word1 == NETD_OP_ATTACH_WAIT ||
         (request->word1 == NETD_OP_DUP && transferred_count == 1) ||
         send_has_transfer);
    if (!transfer_retained)
        for (uint32_t i = 0; i < transferred_count; ++i)
            if (transferred_fds[i] >= 16) (void)pacha_fd_close(transferred_fds[i]);
    if (status != 0) {
        g_netd_socket_errors++;
    }
    const int reply_status = netd_socket_send_reply(
        request->word1, reply_fd, request->word3, status, result,
        reply_transfer_fds, reply_transfer_count);
    netd_unix_socket_set_notifications_deferred(0);
#if NETD_DBUS_DIAG
    if (diag_dbus) {
        printf("[netd-dbus-rpc] phase=reply-exit id=%llu op=%llu handle=%llu status=%d\n",
            (unsigned long long)request->word3,
            (unsigned long long)request->word1,
            (unsigned long long)diag_handle,
            reply_status);
    }
#endif
    return reply_status;
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
    while (g_netd_socket_transfer_leases != NULL) {
        struct netd_socket_transfer_lease *lease =
            g_netd_socket_transfer_leases;
        g_netd_socket_transfer_leases = lease->next;
        netd_socket_transfer_lease_destroy(lease);
    }
    while (g_netd_page_attachments != NULL) {
        struct netd_page_attachment *attachment =
            g_netd_page_attachments;
        g_netd_page_attachments = attachment->next;
        netd_page_attachment_destroy(attachment);
    }
    g_netd_page_attachment_count = 0;
    g_netd_page_attachment_next = 1;
    if (g_netd_socket_endpoint_fd < 16) {
        pacha_trace1(PACHA_TRACE_COMPONENT_NETD, PACHA_TRACE_EVENT_NETD_SOCKET, PACHA_TRACE_CLASS_ERROR, (uint64_t)(uint32_t)g_netd_socket_endpoint_fd);
        return 8;
    }
    pacha_trace1(PACHA_TRACE_COMPONENT_NETD, PACHA_TRACE_EVENT_NETD_SOCKET, PACHA_TRACE_CLASS_STATE, (uint64_t)(uint32_t)g_netd_socket_endpoint_fd);
    return 0;
}

int netd_socket_service_collect_wait_sources(struct pacha_service_wait_set *wait_set)
{
    if (wait_set == NULL) return -22;
    for (const struct netd_socket_transfer_lease *lease =
             g_netd_socket_transfer_leases;
         lease != NULL;
         lease = lease->next)
    {
        if (pacha_service_wait_add(
                wait_set, lease->lease_fd, PACHA_FD_EVENT_HANGUP) != 0)
            return -24;
    }
    for (struct netd_page_attachment *attachment =
             g_netd_page_attachments;
         attachment != NULL;
         attachment = attachment->next)
    {
        attachment->wait_index = wait_set->count;
        if (pacha_service_wait_add(
                wait_set,
                attachment->lease_fd,
                PACHA_FD_EVENT_HANGUP) != 0)
            return -24;
    }
    return 0;
}

void netd_socket_service_reap_hangups(
    const struct pacha_service_wait_set *wait_set)
{
    struct netd_socket_transfer_lease **lease_cursor =
        &g_netd_socket_transfer_leases;
    while (*lease_cursor != NULL) {
        struct netd_socket_transfer_lease *lease = *lease_cursor;
        if ((pacha_service_wait_revents(wait_set, lease->lease_fd) &
             PACHA_FD_EVENT_HANGUP) == 0)
        {
            lease_cursor = &lease->next;
            continue;
        }
        *lease_cursor = lease->next;
        netd_socket_transfer_lease_destroy(lease);
    }

    struct netd_page_attachment **cursor = &g_netd_page_attachments;
    while (*cursor != NULL) {
        struct netd_page_attachment *attachment = *cursor;
        const uint64_t index = attachment->wait_index;
        if (wait_set == NULL || index >= wait_set->count ||
            wait_set->fds[index].fd != attachment->lease_fd ||
            (wait_set->fds[index].revents & PACHA_FD_EVENT_HANGUP) == 0)
        {
            cursor = &attachment->next;
            continue;
        }
        *cursor = attachment->next;
        if (g_netd_page_attachment_count != 0)
            g_netd_page_attachment_count--;
        netd_page_attachment_destroy(attachment);
    }
}

void netd_socket_service_poll(void)
{
    if (g_netd_socket_endpoint_fd < 16) {
        return;
    }

    for (unsigned i = 0; i < 32; i++) {
        (void)netd_unix_socket_flush_notification();
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

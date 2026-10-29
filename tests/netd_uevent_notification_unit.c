#include <assert.h>
#include <stdio.h>
#include "../userland/netd/src/socket_service.c"

static unsigned published, closed, traces, replies;
static int publish_status;
static pacha_fd_t extracted_reply = PACHA_ERR_INVALID;

int netd_netlink_publish_device(uint64_t device)
{
    assert(device == 123);
    ++published;
    return publish_status;
}
int pacha_fd_close(pacha_fd_t fd)
{
    assert(fd == 40 || fd == 41 || fd == 42);
    ++closed;
    return 0;
}
pacha_fd_t pacha_ipc_reply_extract(void) { return extracted_reply; }
int pacha_ipc_reply(pacha_fd_t fd, const struct pacha_ipc_msg *msg)
{
    assert(fd == PACHA_IPC_REPLY_IMPLICIT && (int64_t)msg->word1 == -22);
    ++replies;
    return 0;
}
void pacha_trace_emit(uint32_t component, uint32_t event, uint32_t event_class,
    uint32_t count, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5)
{
    (void)component; (void)event; (void)event_class; (void)count;
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    ++traces;
}

int main(void)
{
    /* Genuine SEND paths must not reply, including malformed input. A CALL
     * impersonating SEND must not leave implicit authority blocking netd. */
    struct pacha_ipc_msg request = {.word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = NETD_OP_UEVENT_PUBLISH, .word2 = 123};
    int handled = 0;
    assert(netd_socket_dispatch_notification(&request, NULL, &handled) == 0);
    assert(handled && published == 1 && !closed);
    request.word3 = 1;
    assert(netd_socket_dispatch_notification(&request, NULL, &handled) == 0);
    assert(!handled && published == 1);
    request.word3 = 0;
    request.word1 = NETD_OP_SOCKET;
    assert(netd_socket_dispatch_notification(&request, NULL, &handled) == 0);
    assert(!handled && published == 1);
    request.word1 = NETD_OP_UEVENT_PUBLISH;
    request.word0 = 0;
    assert(netd_socket_dispatch_notification(&request, NULL, &handled) == -22);
    assert(handled && published == 1);
    request.word0 = PACHA_SERVICE_REQUEST_MAGIC;
    struct pacha_ipc_fd fds[2] = {{.fd = 40}, {.fd = 41}};
    request.fd_count = 2;
    assert(netd_socket_dispatch_notification(&request, fds, &handled) == -22);
    assert(handled && closed == 2 && published == 1);
    request.fd_count = 0;
    publish_status = -22;
    assert(netd_socket_dispatch_notification(&request, NULL, &handled) == -22);
    assert(handled && published == 2 && g_netd_socket_errors == 3);
    assert(replies == 0);
    extracted_reply = 42;
    assert(netd_socket_dispatch_notification(&request, NULL, &handled) == -22);
    assert(handled && published == 2 && closed == 3 && replies == 0);
    extracted_reply = PACHA_ERR_ALLOC;
    assert(netd_socket_dispatch_notification(&request, NULL, &handled) == -22);
    assert(handled && published == 2 && replies == 1);
    (void)traces;
    puts("netd notifications: one-way-only dispatch, correlated RPC exclusion and rejected FD cleanup PASS");
    return 0;
}

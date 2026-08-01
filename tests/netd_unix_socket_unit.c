#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pacha/ipc.h"

static unsigned notifications[512];

int pacha_ipc_send(int fd, const struct pacha_ipc_msg *msg)
{
    (void)msg;
    if (fd >= 0 && (unsigned)fd < sizeof(notifications) / sizeof(notifications[0]))
        notifications[fd]++;
    return 0;
}

int pacha_fd_close(int fd)
{
    (void)fd;
    return 0;
}

int pacha_service_wait_add(
    struct pacha_service_wait_set *set,
    int fd,
    uint32_t events)
{
    (void)set;
    (void)fd;
    (void)events;
    return 0;
}

uint64_t pacha_service_wait_revents(
    const struct pacha_service_wait_set *set,
    int fd)
{
    (void)set;
    (void)fd;
    return 0;
}

#include "../userland/netd/src/unix_socket.c"

static int failures;

static void expect(int condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

static uint64_t open_socket(int notify_fd)
{
    uint64_t handle = 0;
    expect(netd_unix_socket_open(
        NETD_SOCK_STREAM, 0, notify_fd, &handle) == 0,
        "open socket");
    return handle;
}

static int connect_socket(uint64_t handle, const char *path, int32_t pid)
{
    netd_unix_path_t req;
    memset(&req, 0, sizeof(req));
    req.handle = handle;
    req.pid = pid;
    req.uid = (uint32_t)pid + 1000u;
    req.gid = (uint32_t)pid + 2000u;
    snprintf(req.path, sizeof(req.path), "%s", path);
    return netd_unix_socket_connect(&req);
}

static uint64_t accept_socket(uint64_t listener, int32_t expected_pid)
{
    netd_accept_t req;
    memset(&req, 0, sizeof(req));
    req.handle = listener;
    expect(netd_unix_socket_accept(&req) == 0, "accept socket");
    expect(req.pid == expected_pid, "accept preserves FIFO credentials");
    return req.accepted_handle;
}

int main(void)
{
    memset(sockets, 0, sizeof(sockets));
    memset(notifications, 0, sizeof(notifications));
    next_handle = 0;

    const uint64_t listener = open_socket(20);
    netd_unix_path_t bind_req;
    memset(&bind_req, 0, sizeof(bind_req));
    bind_req.handle = listener;
    snprintf(bind_req.path, sizeof(bind_req.path), "/sway-ipc.sock");
    expect(netd_unix_socket_bind(&bind_req) == 0, "bind listener");
    const netd_listen_t listen_req = {
        .handle = listener,
        .backlog = 2,
    };
    expect(netd_unix_socket_listen(&listen_req) == 0, "listen backlog two");

    const uint64_t client1 = open_socket(21);
    const uint64_t client2 = open_socket(22);
    const uint64_t client3 = open_socket(23);
    expect(connect_socket(client1, "/sway-ipc.sock", 101) == 0,
        "first queued connect");
    expect(connect_socket(client2, "/sway-ipc.sock", 102) == 0,
        "second queued connect");
    expect(connect_socket(client3, "/sway-ipc.sock", 103) == -11,
        "backlog full reports EAGAIN");
    expect(notifications[20] == 1, "listener notification is coalesced");

    const uint64_t server1 = accept_socket(listener, 101);
    expect(notifications[20] == 2,
        "accept rearms notification while FIFO remains nonempty");
    expect(connect_socket(client3, "/sway-ipc.sock", 103) == 0,
        "connect succeeds after accept frees backlog slot");
    const uint64_t server2 = accept_socket(listener, 102);
    const uint64_t server3 = accept_socket(listener, 103);
    expect(server1 != 0 && server2 != 0 && server3 != 0,
        "FIFO accepts all queued peers");

    netd_unix_socket_state_t *listener_state = find_socket(listener);
    expect(listener_state != NULL, "find listener state");
    if (listener_state != NULL) listener_state->notify_pending = 1;
    netd_accept_t stale_accept;
    memset(&stale_accept, 0, sizeof(stale_accept));
    stale_accept.handle = listener;
    expect(netd_unix_socket_accept(&stale_accept) == -11,
        "stale listener edge reports EAGAIN");
    const unsigned listener_notifications = notifications[20];
    const uint64_t client_rearm = open_socket(26);
    expect(connect_socket(client_rearm, "/sway-ipc.sock", 106) == 0,
        "connect after stale accept");
    expect(notifications[20] == listener_notifications + 1,
        "empty accept acknowledges stale listener doorbell");
    const uint64_t server_rearm = accept_socket(listener, 106);
    expect(server_rearm != 0, "accept after stale listener edge");

    netd_io_t send_req;
    memset(&send_req, 0, sizeof(send_req));
    send_req.handle = client1;
    send_req.length = NETD_UNIX_RX + 257u;
    for (uint32_t i = 0; i < send_req.length; ++i)
        send_req.data[i] = (uint8_t)(i & 0xffu);
    size_t sent = 0;
    expect(netd_unix_socket_send(&send_req, NULL, 0, &sent) == 0,
        "oversized stream write succeeds partially");
    expect(sent == NETD_UNIX_RX, "partial write fills available receive space");

    uint32_t revents = 0;
    int32_t error = 0;
    expect(netd_unix_socket_poll(
        client1, NETD_POLLOUT, &revents, &error) == 0,
        "poll full writer");
    expect((revents & NETD_POLLOUT) == 0,
        "full peer receive buffer is not writable");

    const unsigned writer_notifications = notifications[21];
    netd_io_t recv_req;
    memset(&recv_req, 0, sizeof(recv_req));
    recv_req.handle = server1;
    size_t received = 0;
    uint32_t received_caps = 0;
    expect(netd_unix_socket_recv(
        &recv_req, 100, NULL, 0, &received_caps, &received) == 0,
        "drain full receive buffer");
    expect(received == 100, "drain count");
    for (size_t i = 0; i < received; ++i)
        expect(recv_req.data[i] == (uint8_t)(i & 0xffu),
            "partial stream byte ordering");
    expect(notifications[21] == writer_notifications + 1,
        "drain notifies blocked writer");
    revents = 0;
    expect(netd_unix_socket_poll(
        client1, NETD_POLLOUT, &revents, &error) == 0,
        "poll drained writer");
    expect((revents & NETD_POLLOUT) != 0,
        "drained peer receive buffer becomes writable");

    memset(&send_req, 0, sizeof(send_req));
    send_req.handle = client1;
    send_req.length = 100;
    expect(netd_unix_socket_send(&send_req, NULL, 0, &sent) == 0 &&
        sent == 100,
        "writer refills receive buffer after readiness acknowledgement");
    memset(&recv_req, 0, sizeof(recv_req));
    recv_req.handle = server1;
    expect(netd_unix_socket_recv(
        &recv_req, 100, NULL, 0, &received_caps, &received) == 0,
        "drain refilled receive buffer");
    expect(notifications[21] == writer_notifications + 2,
        "successive full-to-writable edges each notify writer");

    revents = 0;
    expect(netd_unix_socket_poll(
        client1, NETD_POLLOUT, &revents, &error) == 0 &&
        (revents & NETD_POLLOUT) != 0,
        "poll acknowledges second writable transition");

    const unsigned client_notifications = notifications[21];
    memset(&send_req, 0, sizeof(send_req));
    send_req.handle = server1;
    send_req.length = 1;
    expect(netd_unix_socket_send(&send_req, NULL, 0, &sent) == 0 &&
        sent == 1,
        "peer sends readable byte");
    expect(notifications[21] == client_notifications + 1,
        "readable transition notifies client");
    memset(&send_req, 0, sizeof(send_req));
    send_req.handle = client1;
    send_req.length = 1;
    expect(netd_unix_socket_send(&send_req, NULL, 0, &sent) == 0 &&
        sent == 1,
        "client sends while its readable doorbell is pending");
    expect(notifications[21] == client_notifications + 1,
        "bidirectional send does not duplicate pending doorbell");
    revents = 0;
    expect(netd_unix_socket_poll(
        client1, NETD_POLLIN, &revents, &error) == 0 &&
        (revents & NETD_POLLIN) != 0,
        "poll acknowledges doorbell and preserves level readiness");
    memset(&recv_req, 0, sizeof(recv_req));
    recv_req.handle = client1;
    expect(netd_unix_socket_recv(
        &recv_req, 1, NULL, 0, &received_caps, &received) == 0 &&
        received == 1,
        "receive consumes byte after poll acknowledgement");

    expect(netd_unix_socket_close(client2) == 0, "close empty peer");
    revents = 0;
    expect(netd_unix_socket_poll(server2, 0, &revents, &error) == 0,
        "poll closed peer");
    expect((revents & NETD_POLLHUP) != 0,
        "peer close reports HUP without requested event mask");

    const uint64_t client4 = open_socket(24);
    expect(connect_socket(client4, "/sway-ipc.sock", 104) == 0,
        "queue connection that closes before accept");
    expect(netd_unix_socket_close(client4) == 0,
        "close client before accept");
    const uint64_t server4 = accept_socket(listener, 104);
    revents = 0;
    expect(netd_unix_socket_poll(server4, 0, &revents, &error) == 0,
        "poll accepted dead peer");
    expect((revents & NETD_POLLHUP) != 0,
        "accepted dead peer reports HUP");
    const uint64_t client5 = open_socket(25);
    expect(connect_socket(client5, "/sway-ipc.sock", 105) == 0,
        "dead pending peer does not wedge listener");
    const uint64_t server5 = accept_socket(listener, 105);
    expect(server5 != 0, "accept after dead pending peer");

    uint64_t hup_pair[2] = {0, 0};
    expect(netd_unix_socket_pair(
        NETD_SOCK_STREAM, 0, 28, 29, hup_pair) == 0,
        "open data-before-HUP pair");
    memset(&send_req, 0, sizeof(send_req));
    send_req.handle = hup_pair[0];
    send_req.length = 4;
    expect(netd_unix_socket_send(&send_req, NULL, 0, &sent) == 0 &&
        sent == 4,
        "send data before peer close");
    const unsigned hup_notifications = notifications[29];
    expect(netd_unix_socket_close(hup_pair[0]) == 0,
        "close peer while data remains unread");
    expect(notifications[29] == hup_notifications,
        "close coalesces with existing data notification");
    memset(&recv_req, 0, sizeof(recv_req));
    recv_req.handle = hup_pair[1];
    expect(netd_unix_socket_recv(
        &recv_req, 4, NULL, 0, &received_caps, &received) == 0 &&
        received == 4,
        "receive final bytes before HUP");
    expect(notifications[29] == hup_notifications + 1,
        "final data receive rearms persistent HUP notification");
    revents = 0;
    expect(netd_unix_socket_poll(
        hup_pair[1], 0, &revents, &error) == 0 &&
        (revents & NETD_POLLHUP) != 0,
        "rearmed notification preserves HUP readiness");

    uint64_t pair[2] = {0, 0};
    expect(netd_unix_socket_pair(
        NETD_SOCK_STREAM, 0, 30, 31, pair) == 0,
        "open SCM partial-write pair");
    memset(&send_req, 0, sizeof(send_req));
    send_req.handle = pair[0];
    send_req.length = NETD_UNIX_RX - 1u;
    memset(send_req.data, 0x5a, (size_t)send_req.length);
    expect(netd_unix_socket_send(&send_req, NULL, 0, &sent) == 0 &&
        sent == NETD_UNIX_RX - 1u,
        "prefill SCM receive buffer");
    memset(&send_req, 0, sizeof(send_req));
    send_req.handle = pair[0];
    send_req.length = 8;
    memset(send_req.data, 0xa5, 8);
    send_req.transaction_id = 7;
    send_req.transfer_count = 1;
    send_req.capability_count = 1;
    send_req.transfers[0].provider_id = 1;
    send_req.transfers[0].capability_count = 1;
    send_req.transfers[0].transfer_token = 9;
    const int capability = 80;
    expect(netd_unix_socket_send(
        &send_req, &capability, 1, &sent) == 0 && sent == 1,
        "SCM rights attach once to a partial write");
    revents = 0;
    expect(netd_unix_socket_poll(
        pair[0], NETD_POLLOUT, &revents, &error) == 0,
        "poll writer with pending SCM transaction");
    expect((revents & NETD_POLLOUT) == 0,
        "pending SCM transaction applies write backpressure");
    memset(&recv_req, 0, sizeof(recv_req));
    recv_req.handle = pair[1];
    int received_fds[1] = {-1};
    expect(netd_unix_socket_recv(
        &recv_req, NETD_UNIX_RX, received_fds, 1,
        &received_caps, &received) == 0,
        "receive partial SCM transaction");
    expect(received_caps == 1 && received_fds[0] == capability,
        "SCM capability is delivered exactly once");
    expect(recv_req.transfer_count == 1 && recv_req.transaction_id == 7,
        "SCM metadata accompanies first accepted byte");

    uint64_t stale_pair[2] = {0, 0};
    expect(netd_unix_socket_pair(
        NETD_SOCK_STREAM, 0, 32, 33, stale_pair) == 0,
        "open stale-notification pair");
    netd_unix_socket_state_t *stale_reader = find_socket(stale_pair[1]);
    expect(stale_reader != NULL, "find stale reader");
    if (stale_reader != NULL) stale_reader->notify_pending = 1;
    memset(&recv_req, 0, sizeof(recv_req));
    recv_req.handle = stale_pair[1];
    expect(netd_unix_socket_recv(
        &recv_req, 1, NULL, 0, &received_caps, &received) == -11,
        "stale empty receive reports EAGAIN");
    memset(&send_req, 0, sizeof(send_req));
    send_req.handle = stale_pair[0];
    send_req.length = 1;
    const unsigned stale_notifications = notifications[33];
    expect(netd_unix_socket_send(
        &send_req, NULL, 0, &sent) == 0 && sent == 1,
        "send after stale empty receive");
    expect(notifications[33] == stale_notifications + 1,
        "empty receive acknowledges stale doorbell");

    if (failures != 0) return 1;
    puts("netd unix socket unit: PASS");
    return 0;
}

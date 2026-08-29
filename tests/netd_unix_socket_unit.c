#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pacha/ipc.h"

static unsigned notifications[512];
static uint32_t notification_events[512];

int pacha_ipc_send(int fd, const struct pacha_ipc_msg *msg)
{
    if (fd >= 0 && (unsigned)fd < sizeof(notifications) / sizeof(notifications[0])) {
        notifications[fd]++;
        notification_events[fd] |= (uint32_t)msg->word0;
    }
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

static uint32_t take_notification_events(int notify_fd)
{
    if (notify_fd < 0 ||
        (unsigned)notify_fd >=
            sizeof(notification_events) / sizeof(notification_events[0]))
        return 0;
    const uint32_t events = notification_events[notify_fd];
    notification_events[notify_fd] = 0;
    return events;
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
    netd_unix_socket_state_t *listener_state = find_socket(listener);
    if (listener_state != NULL)
        req.notify_ack = take_notification_events(listener_state->notify_fd);
    expect(netd_unix_socket_accept(&req) == 0, "accept socket");
    expect(req.pid == expected_pid, "accept preserves FIFO credentials");
    return req.accepted_handle;
}

int main(void)
{
    memset(sockets, 0, sizeof(sockets));
    memset(notifications, 0, sizeof(notifications));
    memset(notification_events, 0, sizeof(notification_events));
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
    expect(notification_events[20] == NETD_POLLIN,
        "listener notification identifies readable state");

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
    stale_accept.notify_ack = NETD_POLLIN;
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
    send_req.length = NETD_UNIX_RX;
    for (uint32_t i = 0; i < send_req.length; ++i)
        send_req.data[i] = (uint8_t)(i & 0xffu);
    size_t sent = 0;
    expect(netd_unix_socket_send(&send_req, NULL, 0, &sent) == 0,
        "maximum stream write succeeds");
    expect(sent == NETD_UNIX_RX, "maximum write fills receive space");

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
    expect((notification_events[21] & NETD_POLLOUT) != 0,
        "drain notification identifies writable state");
    revents = 0;
    expect(netd_unix_socket_poll(
        client1, NETD_POLLOUT, &revents, &error) == 0,
        "poll drained writer");
    expect((revents & NETD_POLLOUT) != 0,
        "drained peer receive buffer becomes writable");

    memset(&send_req, 0, sizeof(send_req));
    send_req.handle = client1;
    send_req.length = 100;
    for (uint32_t i = 0; i < send_req.length; ++i)
        send_req.data[i] = (uint8_t)(0xa5u ^ i);
    expect(netd_unix_socket_send(&send_req, NULL, 0, &sent) == 0 &&
        sent == 100,
        "writer refills receive buffer after readiness acknowledgement");
    memset(&recv_req, 0, sizeof(recv_req));
    recv_req.handle = server1;
    expect(netd_unix_socket_recv(
        &recv_req, NETD_UNIX_RX, NULL, 0, &received_caps, &received) == 0 &&
        received == NETD_UNIX_RX,
        "drain wrapped receive buffer");
    for (uint32_t i = 0; i < NETD_UNIX_RX - 100u; ++i)
        expect(recv_req.data[i] == (uint8_t)((i + 100u) & 0xffu),
            "wrapped stream preserves original tail");
    for (uint32_t i = 0; i < 100u; ++i)
        expect(recv_req.data[NETD_UNIX_RX - 100u + i] ==
            (uint8_t)(0xa5u ^ i),
            "wrapped stream preserves appended bytes");
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
    expect((notification_events[21] & NETD_POLLIN) != 0,
        "readable notification identifies input state");
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
    expect(notifications[29] == hup_notifications + 1,
        "close adds HUP to an existing data notification");
    expect(notification_events[29] == (NETD_POLLIN | NETD_POLLHUP),
        "data and HUP notifications retain both event kinds");
    memset(&recv_req, 0, sizeof(recv_req));
    recv_req.handle = hup_pair[1];
    recv_req.notify_ack = take_notification_events(29);
    expect(netd_unix_socket_recv(
        &recv_req, 4, NULL, 0, &received_caps, &received) == 0 &&
        received == 4,
        "receive final bytes before HUP");
    expect(notifications[29] == hup_notifications + 2,
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
    recv_req.notify_ack = NETD_POLLIN;
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

    uint64_t stress_pair[2] = {0, 0};
    expect(netd_unix_socket_pair(
        NETD_SOCK_STREAM, 0, 36, 37, stress_pair) == 0,
        "open wrapped stream stress pair");
    enum { STRESS_BYTES = NETD_UNIX_RX * 3u + 257u };
    static uint8_t stress_source[STRESS_BYTES];
    static uint8_t stress_received[STRESS_BYTES];
    for (uint32_t i = 0; i < STRESS_BYTES; ++i)
        stress_source[i] = (uint8_t)((i * 131u + i / 251u) & 0xffu);
    static const uint32_t receive_sizes[] = {
        32u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u,
        4096u, 8192u, 16384u, 32768u, NETD_UNIX_RX,
    };
    size_t source_offset = 0;
    size_t received_offset = 0;
    size_t receive_step = 0;
    while (received_offset < STRESS_BYTES) {
        const size_t previous_source = source_offset;
        const size_t previous_received = received_offset;
        if (source_offset < STRESS_BYTES) {
            memset(&send_req, 0, sizeof(send_req));
            send_req.handle = stress_pair[0];
            size_t request = STRESS_BYTES - source_offset;
            if (request > NETD_UNIX_RX) request = NETD_UNIX_RX;
            send_req.length = request;
            memcpy(send_req.data, stress_source + source_offset, request);
            const int send_status = netd_unix_socket_send(
                &send_req, NULL, 0, &sent);
            expect(send_status == 0 || send_status == -11,
                "wrapped stream stress send status");
            if (send_status == 0) source_offset += sent;
        }

        memset(&recv_req, 0, sizeof(recv_req));
        recv_req.handle = stress_pair[1];
        size_t capacity = receive_sizes[
            receive_step % (sizeof(receive_sizes) / sizeof(receive_sizes[0]))];
        if (capacity > STRESS_BYTES - received_offset)
            capacity = STRESS_BYTES - received_offset;
        const int recv_status = netd_unix_socket_recv(
            &recv_req, capacity, NULL, 0, &received_caps, &received);
        expect(recv_status == 0 || recv_status == -11,
            "wrapped stream stress receive status");
        if (recv_status == 0) {
            memcpy(stress_received + received_offset, recv_req.data, received);
            received_offset += received;
            receive_step++;
        }
        expect(source_offset != previous_source ||
            received_offset != previous_received,
            "wrapped stream stress makes progress");
    }
    expect(source_offset == STRESS_BYTES,
        "wrapped stream stress sends all bytes");
    expect(memcmp(stress_received, stress_source, STRESS_BYTES) == 0,
        "wrapped stream stress preserves every byte");

    uint64_t seq_pair[2] = {0, 0};
    expect(netd_unix_socket_pair(
        NETD_SOCK_SEQPACKET, 0, 34, 35, seq_pair) == 0,
        "open SOCK_SEQPACKET pair");
    memset(&send_req, 0, sizeof(send_req));
    send_req.handle = seq_pair[0];
    send_req.length = 3;
    memcpy(send_req.data, "one", 3);
    expect(netd_unix_socket_send(
        &send_req, NULL, 0, &sent) == 0 && sent == 3,
        "send first atomic seqpacket");
    send_req.length = 5;
    memcpy(send_req.data, "three", 5);
    expect(netd_unix_socket_send(
        &send_req, NULL, 0, &sent) == 0 && sent == 5,
        "send second atomic seqpacket");
    memset(&recv_req, 0, sizeof(recv_req));
    recv_req.handle = seq_pair[1];
    expect(netd_unix_socket_recv(
        &recv_req, 2, NULL, 0, &received_caps, &received) == 0 &&
        received == 2 && memcmp(recv_req.data, "on", 2) == 0,
        "short seqpacket receive returns packet prefix");
    expect((recv_req.flags & NETD_MSG_TRUNC) != 0,
        "short seqpacket receive reports truncation");
    memset(&recv_req, 0, sizeof(recv_req));
    recv_req.handle = seq_pair[1];
    expect(netd_unix_socket_recv(
        &recv_req, 8, NULL, 0, &received_caps, &received) == 0 &&
        received == 5 && memcmp(recv_req.data, "three", 5) == 0,
        "truncated packet remainder is discarded without crossing boundary");
    memset(&send_req, 0, sizeof(send_req));
    send_req.handle = seq_pair[0];
    send_req.length = NETD_UNIX_RX + 1u;
    expect(netd_unix_socket_send(
        &send_req, NULL, 0, &sent) == -90,
        "oversized seqpacket is rejected atomically");

    memset(&send_req, 0, sizeof(send_req));
    send_req.handle = seq_pair[0];
    expect(netd_unix_socket_send(
        &send_req, NULL, 0, &sent) == 0 && sent == 0,
        "zero-length seqpacket is queued");
    send_req.length = 1;
    send_req.data[0] = 0x7b;
    expect(netd_unix_socket_send(
        &send_req, NULL, 0, &sent) == 0 && sent == 1,
        "packet after zero-length seqpacket is queued separately");
    memset(&recv_req, 0, sizeof(recv_req));
    recv_req.handle = seq_pair[1];
    expect(netd_unix_socket_recv(
        &recv_req, 1, NULL, 0, &received_caps, &received) == 0 &&
        received == 0,
        "zero-length seqpacket is consumed as a record");
    memset(&recv_req, 0, sizeof(recv_req));
    recv_req.handle = seq_pair[1];
    expect(netd_unix_socket_recv(
        &recv_req, 1, NULL, 0, &received_caps, &received) == 0 &&
        received == 1 && recv_req.data[0] == 0x7b,
        "record after zero-length seqpacket remains intact");

    memset(&send_req, 0, sizeof(send_req));
    send_req.handle = seq_pair[0];
    expect(netd_unix_socket_send(
        &send_req, NULL, 0, &sent) == 0 && sent == 0,
        "queue zero-length packet before SCM packet");
    send_req.transaction_id = 17;
    send_req.transfer_count = 1;
    send_req.capability_count = 1;
    send_req.transfers[0].provider_id = 1;
    send_req.transfers[0].capability_count = 1;
    send_req.transfers[0].transfer_token = 19;
    const int seq_capability = 81;
    expect(netd_unix_socket_send(
        &send_req, &seq_capability, 1, &sent) == 0 && sent == 0,
        "zero-length seqpacket carries SCM rights");
    memset(&recv_req, 0, sizeof(recv_req));
    recv_req.handle = seq_pair[1];
    received_fds[0] = -1;
    expect(netd_unix_socket_recv(
        &recv_req, 0, received_fds, 1,
        &received_caps, &received) == 0 && received_caps == 0,
        "preceding zero packet does not steal later SCM rights");
    memset(&recv_req, 0, sizeof(recv_req));
    recv_req.handle = seq_pair[1];
    expect(netd_unix_socket_recv(
        &recv_req, 0, received_fds, 1,
        &received_caps, &received) == 0 &&
        received_caps == 1 && received_fds[0] == seq_capability,
        "SCM rights are anchored to their zero-length seqpacket");

    if (failures != 0) return 1;
    puts("netd unix socket unit: PASS");
    return 0;
}

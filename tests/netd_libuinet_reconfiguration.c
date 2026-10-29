/* Actual netd backend + unmodified libuinet/mbedTLS archives. Only native OS
 * boundaries are adapted: timers, poll, IPC notification FDs and entropy use
 * real Linux facilities. No replacement socket/ARP/route implementation. */
#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ucontext.h>
#include <mbedtls/sha256.h>

#include "../userland/netd/src/libuinet_backend.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
    __FILE__, __LINE__, #x); abort(); } } while (0)

static unsigned timer_fires, filed_calls;
static int high_fd(int fd)
{
    CHECK(fd >= 0);
    int result = fcntl(fd, F_DUPFD_CLOEXEC, 16);
    CHECK(result >= 16);
    CHECK(close(fd) == 0);
    return result;
}

int pacha_timerfd_settime(pacha_fd_t fd, uint64_t initial, uint64_t interval,
    uint64_t flags)
{
    CHECK(flags == 0);
    struct itimerspec spec = {
        .it_value = {(time_t)(initial / 1000000000), (long)(initial % 1000000000)},
        .it_interval = {(time_t)(interval / 1000000000), (long)(interval % 1000000000)},
    };
    return timerfd_settime((int)fd, 0, &spec, NULL) ? -errno : 0;
}

pacha_fd_t pacha_timerfd_create(uint64_t initial, uint64_t interval,
    uint64_t rights, uint32_t flags)
{
    (void)rights; (void)flags;
    int fd = high_fd(timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK));
    CHECK(pacha_timerfd_settime(fd, initial, interval, 0) == 0);
    return fd;
}

long pacha_fd_read(pacha_fd_t fd, void *buf, uint64_t len)
{
    ssize_t n = read((int)fd, buf, (size_t)len);
    if (n > 0 && fd == g_libuinet_sts_timer_fd) ++timer_fires;
    return n < 0 ? -errno : n;
}

int pacha_fd_close(pacha_fd_t fd) { return close((int)fd) ? -errno : 0; }

long pacha_fd_poll(struct pacha_pollfd *fds, uint64_t count)
{
    CHECK(count <= PACHA_SERVICE_WAIT_MAX_FDS);
    struct pollfd native[PACHA_SERVICE_WAIT_MAX_FDS];
    for (size_t i = 0; i < count; ++i) {
        native[i] = (struct pollfd){.fd = (int)fds[i].fd,
            .events = ((fds[i].events & PACHA_FD_EVENT_READABLE) ? POLLIN : 0) |
                ((fds[i].events & PACHA_FD_EVENT_WRITABLE) ? POLLOUT : 0)};
    }
    int result = poll(native, (nfds_t)count, 0);
    if (result < 0) return -errno;
    for (size_t i = 0; i < count; ++i) {
        fds[i].revents = ((native[i].revents & POLLIN) ? PACHA_FD_EVENT_READABLE : 0) |
            ((native[i].revents & POLLOUT) ? PACHA_FD_EVENT_WRITABLE : 0) |
            ((native[i].revents & POLLHUP) ? PACHA_FD_EVENT_HANGUP : 0) |
            ((native[i].revents & (POLLERR | POLLNVAL)) ? PACHA_FD_EVENT_ERROR : 0);
    }
    return result;
}

int pacha_service_wait_add(struct pacha_service_wait_set *set, pacha_fd_t fd,
    uint32_t events)
{
    CHECK(set->count < PACHA_SERVICE_WAIT_MAX_FDS);
    set->fds[set->count++] = (struct pacha_pollfd){.fd = fd, .events = events};
    return 0;
}

uint64_t pacha_service_wait_revents(const struct pacha_service_wait_set *set,
    pacha_fd_t fd)
{
    for (size_t i = 0; i < set->count; ++i)
        if (set->fds[i].fd == (uint64_t)fd) return set->fds[i].revents;
    return 0;
}

int pacha_ipc_send(pacha_fd_t fd, const struct pacha_ipc_msg *msg)
{
    ssize_t n = send((int)fd, msg, sizeof(*msg), MSG_DONTWAIT | MSG_NOSIGNAL);
    return n < 0 ? -errno : n == sizeof(*msg) ? 0 : -EIO;
}

long pacha_getrandom(void *buf, uint64_t length, uint64_t flags)
{
    ssize_t n = getrandom(buf, (size_t)length, (unsigned)flags);
    return n < 0 ? -errno : n;
}

void *pacha_mmap(pacha_fd_t fd, uint64_t size, uint64_t prot, uint64_t flags,
    uint64_t offset)
{
    (void)fd; (void)size; (void)prot; (void)flags; (void)offset;
    CHECK(!"unused filedfs mapping reached");
    return NULL;
}
int pacha_munmap(void *addr, uint64_t size) { return munmap(addr, size) ? -errno : 0; }

int filedfs_call(pacha_fd_t endpoint, kb2_fs_request_t *request,
    const char *path, const char *second_path, const void *data, size_t size,
    void *output, size_t capacity, kb2_fs_response_t *response,
    const pacha_fd_t *sources, size_t source_count,
    pacha_fd_t *replies, size_t *reply_count)
{
    (void)endpoint; (void)request; (void)path; (void)second_path; (void)data;
    (void)size; (void)output; (void)capacity; (void)response; (void)sources;
    (void)source_count; (void)replies; (void)reply_count;
    ++filed_calls;
    return -ENOSYS;
}

uint64_t netd_metrics_read_tsc(void)
{
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec;
}
void netd_metrics_record(const char *name, uint64_t start, uint64_t end)
{ (void)name; (void)start; (void)end; }
void netd_metrics_record_ex(const char *stage, const char *name, uint64_t start,
    uint64_t end, uint64_t size)
{ (void)stage; (void)name; (void)start; (void)end; (void)size; }

struct captured_frame { size_t size; uint8_t bytes[1600]; };
static struct captured_frame captured[1024];
static size_t captured_count;
static const uint8_t local_mac[6] = {2, 0, 0, 0, 0, 1};
static uint8_t peer_mac[6] = {2, 0, 0, 0, 0, 2};
static const uint8_t old_ip[4] = {192, 0, 2, 10};
static const uint8_t new_ip[4] = {198, 51, 100, 20};
static uint8_t peer_ip[4] = {192, 0, 2, 1};

static int capture(void *context, const void *frame, size_t length)
{
    (void)context;
    CHECK(captured_count < sizeof(captured) / sizeof(captured[0]));
    CHECK(length <= sizeof(captured[0].bytes));
    captured[captured_count].size = length;
    memcpy(captured[captured_count++].bytes, frame, length);
    return 0;
}

static int receive(void *context, uint64_t generation, const void *bytes, size_t size)
{
    (void)context; (void)generation;
    struct netd_upper_frame frame = {.bytes = bytes, .len = size};
    return netd_libuinet_receive_frame(&frame);
}

static void pump(unsigned milliseconds)
{
    uint64_t deadline = netd_metrics_read_tsc() + milliseconds * 1000000ull;
    do {
        netd_libuinet_poll();
        struct pollfd timer = {.fd = (int)g_libuinet_sts_timer_fd, .events = POLLIN};
        CHECK(poll(&timer, 1, 1) >= 0);
    } while (netd_metrics_read_tsc() < deadline);
    netd_libuinet_poll();
    CHECK(g_libuinet_sts_error == 0);
}

static void put16(uint8_t *p, unsigned value) { p[0] = value >> 8; p[1] = value; }
static void put32(uint8_t *p, uint32_t value)
{ p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value; }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t checksum(const uint8_t *p, size_t size, uint32_t sum)
{
    while (size > 1) { sum += (unsigned)p[0] * 256 + p[1]; p += 2; size -= 2; }
    if (size) sum += (unsigned)*p << 8;
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return (uint16_t)~sum;
}

static void arp(const uint8_t target[4], int reply)
{
    uint8_t bytes[60] = {0};
    memset(bytes, 255, 6); memcpy(bytes + 6, peer_mac, 6); put16(bytes + 12, 0x806);
    put16(bytes + 14, 1); put16(bytes + 16, 0x800); bytes[18] = 6; bytes[19] = 4;
    put16(bytes + 20, reply ? 2 : 1); memcpy(bytes + 22, peer_mac, 6);
    memcpy(bytes + 28, peer_ip, 4); memcpy(bytes + 32, local_mac, 6);
    memcpy(bytes + 38, target, 4);
    CHECK(netd_link_receive(1, bytes, sizeof(bytes)) == 0);
    pump(5);
}

static unsigned arp_replies(const uint8_t target[4])
{
    unsigned count = 0;
    for (size_t i = 0; i < captured_count; ++i) {
        const uint8_t *b = captured[i].bytes;
        if (captured[i].size >= 42 && b[12] == 8 && b[13] == 6 &&
            b[21] == 2 && !memcmp(b + 28, target, 4)) ++count;
    }
    return count;
}

static void tcp(const uint8_t target[4], uint16_t source, uint8_t flags,
    uint32_t sequence, uint32_t ack)
{
    uint8_t b[60] = {0};
    memcpy(b, local_mac, 6); memcpy(b + 6, peer_mac, 6); put16(b + 12, 0x800);
    b[14] = 0x45; put16(b + 16, 40); b[22] = 64; b[23] = 6;
    memcpy(b + 26, peer_ip, 4); memcpy(b + 30, target, 4);
    put16(b + 24, checksum(b + 14, 20, 0));
    put16(b + 34, source); put16(b + 36, 2222);
    put32(b + 38, sequence); put32(b + 42, ack); b[46] = 0x50; b[47] = flags;
    put16(b + 48, 65535);
    uint32_t pseudo = 6 + 20;
    for (unsigned i = 26; i < 34; i += 2) pseudo += (unsigned)b[i] * 256 + b[i + 1];
    put16(b + 50, checksum(b + 34, 20, pseudo));
    CHECK(netd_link_receive(1, b, sizeof(b)) == 0);
    pump(5);
}

static void handshake(const uint8_t address[4], uint16_t source)
{
    captured_count = 0;
    arp(address, 0);
    tcp(address, source, 2, 1000, 0);
    for (size_t i = 0; i < captured_count; ++i) {
        const uint8_t *b = captured[i].bytes;
        if (captured[i].size >= 54 && b[12] == 8 && b[13] == 0 &&
            b[23] == 6 && b[47] == 0x12 &&
            b[36] == (uint8_t)(source >> 8) && b[37] == (uint8_t)source) {
            tcp(address, source, 0x10, 1001, get32(b + 38) + 1);
            return;
        }
    }
    CHECK(!"wildcard listener failed real Ethernet TCP handshake");
}

struct test_socket { uint64_t handle; int notify, peer; };
static struct test_socket notification_channel(void)
{
    int pair[2]; CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair) == 0);
    struct test_socket s = {.notify = high_fd(pair[0]), .peer = high_fd(pair[1])};
    return s;
}

static struct test_socket open_socket(unsigned type)
{
    struct test_socket s = notification_channel();
    CHECK(netd_libuinet_socket_open(NETD_AF_INET, type, 0, s.notify, &s.handle) == 0);
    return s;
}

static void bind_socket(struct test_socket s, const uint8_t address[4], unsigned port)
{
    uint32_t addr, actual_addr; uint16_t actual_port;
    memcpy(&addr, address, 4);
    CHECK(netd_libuinet_socket_bind(s.handle, addr, htons(port), 0,
        &actual_addr, &actual_port) == 0);
    CHECK(actual_addr == addr && actual_port == htons(port));
}

static void close_socket(struct test_socket s)
{
    CHECK(netd_libuinet_socket_close(s.handle) == 0);
    CHECK(fcntl(s.notify, F_GETFD) == -1 && errno == EBADF);
    CHECK(close(s.peer) == 0);
}

static struct test_socket accept_socket(struct test_socket listener,
    const uint8_t address[4], unsigned source_port)
{
    struct test_socket s = notification_channel();
    uint32_t peer, local; uint16_t peer_port, local_port;
    CHECK(netd_libuinet_socket_accept(listener.handle, s.notify, &s.handle,
        &peer, &peer_port, &local, &local_port) == 0);
    CHECK(local_port == htons(2222));
    CHECK(peer_port == htons(source_port));
    CHECK(!memcmp(&peer, peer_ip, 4));
    CHECK(!memcmp(&local, address, 4));
    return s;
}

static void expect_no_accept(struct test_socket listener)
{
    struct test_socket channel = notification_channel();
    uint64_t handle = 0; uint32_t peer, local; uint16_t peer_port, local_port;
    CHECK(netd_libuinet_socket_accept(listener.handle, channel.notify, &handle,
        &peer, &peer_port, &local, &local_port) == -EAGAIN);
    CHECK(close(channel.notify) == 0);
    CHECK(close(channel.peer) == 0);
}

static void expect_tombstone(struct test_socket s)
{
    uint32_t events = 0; int32_t error = 0; size_t sent = 99, received = 99;
    uint32_t peer; uint16_t port; char byte = 'x';
    CHECK(netd_libuinet_socket_poll(s.handle, UINT32_MAX, &events, &error) == 0);
    CHECK((events & (NETD_POLLERR | NETD_POLLHUP)) == (NETD_POLLERR | NETD_POLLHUP));
    CHECK(error == ENETRESET);
    CHECK(netd_libuinet_socket_send(s.handle, &byte, 1, 0, 0, 0, &sent) == -ENETRESET);
    CHECK(sent == 0);
    CHECK(netd_libuinet_socket_recv(s.handle, &byte, 1, 0, &received, &peer, &port) == -ENETRESET);
    CHECK(netd_libuinet_socket_dup(s.handle) == 0);
    CHECK(netd_libuinet_socket_close(s.handle) == 0);
    CHECK(fcntl(s.notify, F_GETFD) >= 0);
    CHECK(netd_libuinet_api_socket_find(s.handle)->socket == NULL);
}

static uint16_t wildcard_udp_port(struct test_socket s)
{
    struct uinet_sockaddr *name = NULL;
    CHECK(uinet_sogetsockaddr(netd_libuinet_api_socket_find(s.handle)->socket, &name) == 0);
    CHECK(name && name->sa_family == UINET_AF_INET);
    const struct uinet_sockaddr_in *inet = (const void *)name;
    CHECK(inet->sin_addr.s_addr == 0 && inet->sin_port != 0);
    uint16_t port = inet->sin_port;
    uinet_free_sockaddr(name);
    return port;
}

static void source_gate(const uint8_t address[4], int allowed)
{
    uint8_t b[60] = {0}; memcpy(b, peer_mac, 6); memcpy(b + 6, local_mac, 6);
    put16(b + 12, 0x800); memcpy(b + 26, address, 4);
    size_t before = captured_count;
    CHECK(netd_libuinet_send_frame(b, sizeof(b)) == (allowed ? 0 : -EADDRNOTAVAIL));
    CHECK(captured_count == before + (allowed ? 1 : 0));
    put16(b + 12, 0x806); memcpy(b + 28, address, 4);
    before = captured_count;
    CHECK(netd_libuinet_send_frame(b, sizeof(b)) == (allowed ? 0 : -EADDRNOTAVAIL));
    CHECK(captured_count == before + (allowed ? 1 : 0));
}

static void crash_trace(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    fprintf(stderr, "FAULT addr=%p rip=%llx rsp=%llx\n", info->si_addr,
        (unsigned long long)uc->uc_mcontext.gregs[REG_RIP],
        (unsigned long long)uc->uc_mcontext.gregs[REG_RSP]);
    uintptr_t *sp = (void *)uc->uc_mcontext.gregs[REG_RSP];
    for (unsigned i = 0; i < 8; ++i)
        fprintf(stderr, "stack[%u]=%lx\n", i, (unsigned long)sp[i]);
    void *stack[40]; int n = backtrace(stack, 40);
    backtrace_symbols_fd(stack, n, STDERR_FILENO);
    _exit(128 + sig);
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    struct sigaction action = {.sa_sigaction = crash_trace, .sa_flags = SA_SIGINFO};
    CHECK(sigaction(SIGSEGV, &action, NULL) == 0);
    CHECK(argc == 1 || (argc == 2 && (!strcmp(argv[1], "--tombstone-hup") ||
        !strcmp(argv[1], "--tombstone-hup-stream") ||
        !strcmp(argv[1], "--source-gate"))));
    static const uint8_t sha256_abc[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    uint8_t digest[32];
    CHECK(mbedtls_sha256_ret((const void *)"abc", 3, digest, 0) == 0);
    CHECK(!memcmp(digest, sha256_abc, sizeof(digest)));
    struct netd_boot_config config = {0};
    memcpy(config.ipv4.address, old_ip, 4);
    memset(config.ipv4.netmask, 255, 3); memcpy(config.ipv4.gateway, peer_ip, 4);
    CHECK(netd_network_config_init(&config.ipv4) == 0);
    netd_link_init(receive, NULL);
    struct netd_link_info info = {.generation = 1, .interface_id = NETD_LINK_ID_PRIMARY,
        .mtu = 1500, .carrier = 1}; memcpy(info.mac, local_mac, 6);
    struct netd_link_ops ops = {.send = capture};
    CHECK(netd_link_attach(&info, &ops, NULL) == 0);
    struct netd_runtime runtime = {.cfg = &config};
    CHECK(netd_libuinet_start(&runtime) == 0); pump(1100);
    CHECK(timer_fires > 0);
    CHECK(filed_calls == 0);
    printf("PASS real host clock/timerfd/poll dispatches=%u / mbedTLS SHA256\n", timer_fires);

    if (argc == 2 && !strcmp(argv[1], "--source-gate")) {
        /* Exercise the actual TX callback independently of ifdestroy. This
         * changes only its policy input: it does NOT claim that interface,
         * routes or library sockets have been successfully reconfigured. */
        source_gate(old_ip, 1); source_gate(new_ip, 0);
        memcpy(config.ipv4.address, new_ip, 4);
        CHECK(netd_network_config_init(&config.ipv4) == 0);
        source_gate(old_ip, 0); source_gate(new_ip, 1);
        puts("PASS isolated actual TX callback rejects old IPv4/ARP source after policy change");
        netd_libuinet_ipv4_suspend();
        uint8_t bytes[60] = {0};
        size_t before = captured_count;
        CHECK(netd_libuinet_send_frame(bytes, sizeof(bytes)) == -ENETDOWN);
        CHECK(netd_link_receive(1, bytes, sizeof(bytes)) == -ENETDOWN);
        CHECK(captured_count == before && filed_calls == 0);
        puts("PASS isolated actual TX/RX suspension gate");
        return 0;
    }

    captured_count = 0; arp(old_ip, 0); CHECK(arp_replies(old_ip) > 0);
    struct test_socket listener = open_socket(NETD_SOCK_STREAM);
    const uint8_t any[4] = {0}; bind_socket(listener, any, 2222);
    CHECK(netd_libuinet_socket_listen(listener.handle, 8) == 0);
    struct uinet_socket *listener_identity = netd_libuinet_api_socket_find(listener.handle)->socket;
    handshake(old_ip, 42000); struct test_socket accepted = accept_socket(listener, old_ip, 42000);
    handshake(old_ip, 42001); /* Queued old-network accept must be purged. */
    struct test_socket udp = open_socket(NETD_SOCK_DGRAM); bind_socket(udp, old_ip, 3333);
    struct test_socket tcp_pending = open_socket(NETD_SOCK_STREAM);
    uint32_t peer; memcpy(&peer, peer_ip, 4);
    CHECK(netd_libuinet_socket_connect(tcp_pending.handle, peer, htons(9999), 0) == -EINPROGRESS);
    CHECK(netd_libuinet_reconfigure(&runtime) == 0); /* Unchanged lease preserves sockets. */
    CHECK(netd_libuinet_api_socket_find(accepted.handle)->socket != NULL);
    puts("PASS initial ARP/TCP and unchanged-lease preservation");

    if (argc == 2) {
        /* Isolate the notification-owner lifetime test from interface detach.
         * Construct exactly reconfigure's tombstone using its real abort
         * helper; this does not claim end-to-end reconfiguration coverage. */
        struct test_socket doomed[] = {udp, accepted};
        for (size_t i = 0; i < sizeof(doomed) / sizeof(doomed[0]); ++i) {
            struct netd_libuinet_api_socket *slot = netd_libuinet_api_socket_find(doomed[i].handle);
            slot->network_error = ENETRESET;
            CHECK(network_abort_socket(&slot->socket, slot->type == NETD_SOCK_STREAM) == 0);
            slot->notify_pending = 0;
            expect_tombstone(doomed[i]);
        }
        int stream_orphan = !strcmp(argv[1], "--tombstone-hup-stream");
        struct test_socket closed = stream_orphan ? udp : accepted;
        struct test_socket orphan = stream_orphan ? accepted : udp;
        close_socket(closed);
        CHECK(netd_libuinet_socket_dup(closed.handle) == -EBADF);
        CHECK(netd_libuinet_socket_close(closed.handle) == -EBADF);
        puts("PASS isolated tombstone dup/poll/last-close");
        CHECK(close(orphan.peer) == 0);
        struct pacha_service_wait_set waitset = {0};
        CHECK(netd_libuinet_socket_collect_wait_sources(&waitset) == 0);
        CHECK(pacha_fd_poll(waitset.fds, waitset.count) > 0);
        CHECK(pacha_service_wait_revents(&waitset, orphan.notify) & PACHA_FD_EVENT_HANGUP);
        netd_libuinet_socket_reap_hangups(&waitset);
        CHECK(netd_libuinet_api_socket_find(orphan.handle) == NULL);
        CHECK(fcntl(orphan.notify, F_GETFD) == -1 && errno == EBADF);
        puts("PASS isolated tombstone orphan HUP cleanup");
        return 0;
    }

    netd_libuinet_ipv4_suspend();
    uint8_t frame[60] = {0}; size_t before = captured_count;
    CHECK(netd_libuinet_send_frame(frame, sizeof(frame)) == -ENETDOWN);
    CHECK(netd_link_receive(1, frame, sizeof(frame)) == -ENETDOWN);
    size_t sent = 99; char byte = 'x';
    CHECK(netd_libuinet_socket_send(udp.handle, &byte, 1, 0, peer, htons(7777), &sent) == -ENETDOWN);
    /* Also cover an actual TCP retransmit timer while admission is closed. */
    CHECK(sent == 0); pump(1100); CHECK(captured_count == before);
    puts("PASS suspended TX/RX/source gate including real TCP timer interval");
    puts("TRACE entering same-IP forced reconfiguration");
    peer_mac[5] = 3;
    CHECK(netd_libuinet_reconfigure(&runtime) == 0); /* Same IP, forced new L2. */
    CHECK(netd_libuinet_api_socket_find(listener.handle)->socket == listener_identity);
    expect_no_accept(listener);
    expect_tombstone(accepted); expect_tombstone(udp); expect_tombstone(tcp_pending);
    puts("PASS suspend gate / same-IP new-L2 reset / tombstone dup-poll-send-recv");

    /* Public socket APIs must ask for fresh ARP, not reuse the first LAN's MAC. */
    struct test_socket fresh_udp = open_socket(NETD_SOCK_DGRAM);
    captured_count = 0;
    CHECK(netd_libuinet_socket_send(fresh_udp.handle, &byte, 1, 0, peer, htons(9998), &sent) == 0);
    pump(5);
    unsigned arp_requests = 0;
    for (size_t i = 0; i < captured_count; ++i)
        if (captured[i].size >= 42 && captured[i].bytes[12] == 8 &&
            captured[i].bytes[13] == 6 && captured[i].bytes[21] == 1) ++arp_requests;
    CHECK(arp_requests > 0);
    arp(old_ip, 1);
    unsigned new_mac_tx = 0;
    for (size_t i = 0; i < captured_count; ++i)
        if (captured[i].size >= 34 && captured[i].bytes[12] == 8 &&
            captured[i].bytes[13] == 0) {
            CHECK(!memcmp(captured[i].bytes, peer_mac, 6)); ++new_mac_tx;
        }
    CHECK(new_mac_tx > 0);
    /* Unconnected sendto autobinds a port while retaining INADDR_ANY. It
     * has no old-peer association to retire: distinguish it from udp above,
     * which was explicitly bound to the obsolete local address. */
    uint16_t wildcard_port = wildcard_udp_port(fresh_udp);
    struct uinet_socket *wildcard_identity = netd_libuinet_api_socket_find(fresh_udp.handle)->socket;
    source_gate(old_ip, 1);
    handshake(old_ip, 42002); struct test_socket accepted_same = accept_socket(listener, old_ip, 42002);
    puts("PASS fresh ARP/new MAC and preserved wildcard listener handshake");

    netd_libuinet_ipv4_suspend();
    memcpy(config.ipv4.address, new_ip, 4);
    peer_ip[0] = 198; peer_ip[1] = 51; peer_ip[2] = 100; peer_ip[3] = 1;
    memcpy(config.ipv4.gateway, peer_ip, 4);
    CHECK(netd_network_config_init(&config.ipv4) == 0);
    CHECK(netd_libuinet_reconfigure(&runtime) == 0);
    expect_no_accept(listener);
    expect_tombstone(accepted_same);
    CHECK(netd_libuinet_api_socket_find(fresh_udp.handle)->socket == wildcard_identity);
    CHECK(wildcard_udp_port(fresh_udp) == wildcard_port);
    uint32_t ready = 0; int32_t socket_error = 0;
    CHECK(netd_libuinet_socket_poll(fresh_udp.handle, NETD_POLLOUT, &ready, &socket_error) == 0);
    CHECK(!(ready & (NETD_POLLERR | NETD_POLLHUP)) && !socket_error);
    /* The surviving wildcard socket must actually use the new source IP
     * and resolve its new neighbor; a retained handle alone proves little. */
    uint32_t next_peer; memcpy(&next_peer, peer_ip, 4); captured_count = 0;
    CHECK(netd_libuinet_socket_send(fresh_udp.handle, &byte, 1, 0,
        next_peer, htons(9998), &sent) == 0 && sent == 1);
    pump(5);
    unsigned next_arp = 0;
    for (size_t i = 0; i < captured_count; ++i) {
        const uint8_t *b = captured[i].bytes;
        if (captured[i].size >= 42 && b[12] == 8 && b[13] == 6 && b[21] == 1 &&
            !memcmp(b + 38, peer_ip, 4)) {
            CHECK(!memcmp(b + 28, new_ip, 4)); ++next_arp;
        }
    }
    CHECK(next_arp > 0); arp(new_ip, 1);
    unsigned wildcard_tx = 0;
    for (size_t i = 0; i < captured_count; ++i) {
        const uint8_t *b = captured[i].bytes;
        if (captured[i].size >= 43 && b[12] == 8 && b[13] == 0 && b[23] == 17) {
            CHECK(!memcmp(b + 26, new_ip, 4) && !memcmp(b + 30, peer_ip, 4));
            CHECK(!memcmp(b, peer_mac, 6));
            CHECK(b[34] == (uint8_t)(ntohs(wildcard_port) >> 8) &&
                b[35] == (uint8_t)ntohs(wildcard_port));
            CHECK(b[42] == (uint8_t)byte); ++wildcard_tx;
        }
    }
    CHECK(wildcard_tx > 0);
    puts("PASS wildcard unconnected UDP retains port and uses new-IP/new-ARP dataplane");
    captured_count = 0; arp(old_ip, 0); CHECK(arp_replies(old_ip) == 0);
    arp(new_ip, 0); CHECK(arp_replies(new_ip) > 0);
    source_gate(old_ip, 0); source_gate(new_ip, 1);
    handshake(new_ip, 42003); struct test_socket accepted_new = accept_socket(listener, new_ip, 42003);
    CHECK(netd_libuinet_api_socket_find(listener.handle)->socket == listener_identity);
    puts("PASS changed-IP ARP retirement/source gate/new-IP wildcard TCP accept");

    struct test_socket routed = open_socket(NETD_SOCK_DGRAM);
    const uint8_t remote[4] = {203, 0, 113, 5}; uint32_t remote_be;
    memcpy(&remote_be, remote, 4); captured_count = 0;
    CHECK(netd_libuinet_socket_send(routed.handle, &byte, 1, 0,
        remote_be, htons(9998), &sent) == 0);
    pump(10);
    unsigned routed_frames = 0;
    for (size_t i = 0; i < captured_count; ++i) {
        const uint8_t *b = captured[i].bytes;
        if (captured[i].size >= 34 && b[12] == 8 && b[13] == 0 &&
            !memcmp(b + 30, remote, 4)) {
            CHECK(!memcmp(b + 26, new_ip, 4));
            CHECK(!memcmp(b, peer_mac, 6)); ++routed_frames;
        }
    }
    CHECK(routed_frames > 0); close_socket(routed);
    puts("PASS changed default route sends off-subnet UDP through new gateway");

    close_socket(accepted); close_socket(tcp_pending); close_socket(fresh_udp);
    close_socket(accepted_same); close_socket(accepted_new); close_socket(listener);
    CHECK(netd_libuinet_socket_close(accepted.handle) == -EBADF);
    CHECK(netd_libuinet_socket_dup(accepted.handle) == -EBADF);
    pump(1100); CHECK(timer_fires > 0); CHECK(filed_calls == 0);
    printf("PASS last-close invalidation / real timerfd dispatches=%u / filedfs unused\n", timer_fires);

    /* Run orphan cleanup in a child: a null-socket crash must not discard the
     * independently useful positive test output. Nonzero is still test FAIL. */
    {
        pid_t child = fork(); CHECK(child >= 0);
        if (child == 0) {
            CHECK(close(udp.peer) == 0);
            struct pollfd peer_closed = {.fd = udp.notify, .events = POLLIN};
            /* Parent must drop its inherited peer before HUP can appear. */
            uint64_t deadline = netd_metrics_read_tsc() + 1000000000ull;
            while (!(peer_closed.revents & POLLHUP)) {
                CHECK(poll(&peer_closed, 1, 10) >= 0);
                CHECK(netd_metrics_read_tsc() < deadline);
            }
            struct pacha_service_wait_set waitset = {0};
            CHECK(netd_libuinet_socket_collect_wait_sources(&waitset) == 0);
            CHECK(pacha_fd_poll(waitset.fds, waitset.count) > 0);
            CHECK(pacha_service_wait_revents(&waitset, udp.notify) & PACHA_FD_EVENT_HANGUP);
            netd_libuinet_socket_reap_hangups(&waitset);
            CHECK(netd_libuinet_api_socket_find(udp.handle) == NULL);
            CHECK(fcntl(udp.notify, F_GETFD) == -1 && errno == EBADF);
            _exit(0);
        }
        /* The parent must close its copy, or the child's peer is not hung up. */
        CHECK(close(udp.peer) == 0);
        int status; CHECK(waitpid(child, &status, 0) == child);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr, "FAIL tombstone orphan HUP cleanup status=%d\n", status);
            return 1;
        }
        CHECK(close(udp.notify) == 0);
        puts("PASS tombstone orphan HUP cleanup");
    }
    puts("NETD_LIBUINET_RECONFIGURATION=PASS");
    return 0;
}

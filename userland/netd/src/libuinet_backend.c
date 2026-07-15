
#include "libuinet_backend.h"

#include "netd_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NETD_WITH_LIBUINET)
#include "filed/ipc_protocol.h"
#include "linux_subsystem/net/net_device.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "netd/ipc_protocol.h"
#include "pacha/ipc.h"
#include "pacha/syscall.h"
#include "uinet_api.h"
#include "uinet_pachaos_api.h"
#endif

static enum netd_libuinet_state g_libuinet_state;
static uint64_t g_libuinet_rx_frames;
static uint64_t g_libuinet_rx_drops;
static int g_libuinet_trace;

#if defined(NETD_WITH_LIBUINET)
static int netd_libuinet_errno_to_linux(int error)
{
    switch (error) {
    case 0: return 0;
    case UINET_EAGAIN: return 11;
    case UINET_EINPROGRESS: return 115;
    case UINET_EALREADY: return 114;
    case UINET_ENETUNREACH: return 101;
    case UINET_EISCONN: return 106;
    case UINET_ETIMEDOUT: return 110;
    case UINET_ECONNREFUSED: return 111;
    default: return error;
    }
}

static int netd_libuinet_socket_connect_target_supported(uint32_t addr_be)
{
    const uint8_t *addr = (const uint8_t *)&addr_be;
    if (addr[0] == 10 && !(addr[1] == 0 && addr[2] == 2)) {
        return 0;
    }
    return 1;
}

#define NETD_IPV4_ADDR "10.0.2.15"
#define NETD_IPV4_BROADCAST "10.0.2.255"
#define NETD_IPV4_MASK "255.255.255.0"
#define NETD_IPV4_GATEWAY "10.0.2.2"
#define NETD_DNS_SERVER "10.0.2.3"
#define NETD_HTTP_HOST "example.com"
#define NETD_HTTP_PATH "/"
#define NETD_UDP_ECHO_PORT 7777u
#define NETD_TCP_ECHO_PORT 7778u
#define NETD_TCP_ECHO_MAX_CONNECTIONS 16u
#define NETD_SOCKET_API_MAX_SOCKETS 64u
#define NETD_HTTP_PORT 80u
#define NETD_HTTPS_PORT 443u
#define NETD_DNS_PORT 53u
#define NETD_DNS_TXID 0x5030u
#define NETD_CA_BUNDLE_PATH "/etc/ssl/certs/ca-certificates.crt"
#define NETD_CA_BUNDLE_FALLBACK_PATH "/etc/ssl/cert.pem"
#define NETD_FILED_ENDPOINT_FD 240

static const uint8_t k_netd_local_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static const uint8_t k_netd_local_ip[4] = { 10, 0, 2, 15 };
static const uint8_t k_netd_smoke_peer_mac[6] = { 0x52, 0x55, 0x00, 0xaa, 0xbb, 0xcc };
static const uint8_t k_netd_smoke_peer_ip[4] = { 10, 0, 2, 2 };

static uinet_if_t g_libuinet_if;
static struct uinet_socket *g_libuinet_udp_echo;
static struct uinet_socket *g_libuinet_tcp_listener;
static struct uinet_socket *g_libuinet_tcp_connections[NETD_TCP_ECHO_MAX_CONNECTIONS];
struct netd_libuinet_api_socket {
    uint64_t handle;
    uint64_t refcount;
    uint64_t type;
    uint64_t protocol;
    uint64_t recv_calls;
    uint64_t recv_bytes;
    uint64_t send_calls;
    uint64_t send_bytes;
    uint64_t poll_calls;
    uint8_t recv_idle_logged;
    uint8_t poll_ready_logged;
    uint8_t send_preview_logged;
    uint8_t recv_preview_logged;
    uint8_t reserved[4];
    int notify_fd;
    struct uinet_socket *socket;
};
static struct netd_libuinet_api_socket g_libuinet_api_sockets[NETD_SOCKET_API_MAX_SOCKETS];
static uint64_t g_libuinet_api_next_handle;

static int netd_libuinet_trace_poll_sample(uint64_t calls)
{
    return calls == 1 || calls == 16 || calls == 64 || calls == 256 ||
           calls == 1024 || (calls >= 4096 && (calls % 4096u) == 0);
}

static void netd_libuinet_api_pump(unsigned rounds)
{
    for (unsigned i = 0; i < rounds; i++) {
        netd_packet_io_pump_once();
    }
}

static void netd_libuinet_print_ascii_preview(const char *prefix, uint64_t handle, const void *data, size_t len)
{
    const unsigned char *bytes = (const unsigned char *)data;
    size_t preview = len;
    if (preview > 768u) {
        preview = 768u;
    }
    printf("%s handle=%llu len=%llu preview=%llu\n",
           prefix,
           (unsigned long long)handle,
           (unsigned long long)len,
           (unsigned long long)preview);
    for (size_t i = 0; i < preview; i++) {
        const unsigned char c = bytes[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n' || (c >= 0x20u && c <= 0x7eu)) {
            putchar((int)c);
        } else {
            putchar('.');
        }
    }
    putchar('\n');
    fflush(stdout);
}

enum netd_http_smoke_state {
    NETD_HTTP_SMOKE_IDLE = 0,
    NETD_HTTP_SMOKE_DNS_WAIT,
    NETD_HTTP_SMOKE_CONNECTING,
    NETD_HTTP_SMOKE_SENDING,
    NETD_HTTP_SMOKE_READING,
    NETD_HTTP_SMOKE_HTTPS_CONNECTING,
    NETD_HTTP_SMOKE_HTTPS_HANDSHAKE,
    NETD_HTTP_SMOKE_HTTPS_SENDING,
    NETD_HTTP_SMOKE_HTTPS_READING,
    NETD_HTTP_SMOKE_DONE,
    NETD_HTTP_SMOKE_ERROR,
};
static struct {
    enum netd_http_smoke_state state;
    struct uinet_socket *dns_socket;
    struct uinet_socket *http_socket;
    struct uinet_socket *https_socket;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config ssl_config;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt ca_chain;
    uint32_t resolved_ip_be;
    char resolved_ip_text[16];
    char request[256];
    char https_request[256];
    size_t request_len;
    size_t request_sent;
    size_t https_request_len;
    size_t https_request_sent;
    size_t response_bytes;
    size_t https_response_bytes;
    uint64_t https_connect_polls;
    uint64_t https_handshake_polls;
    char status_line[96];
    char https_status_line[96];
    size_t status_line_len;
    size_t https_status_line_len;
    int ssl_initialized;
} g_libuinet_http_smoke;
static uint64_t g_libuinet_tx_frames;
static uint64_t g_libuinet_arp_rx;
static uint64_t g_libuinet_arp_tx;
static uint64_t g_libuinet_ipv4_rx;
static uint64_t g_libuinet_ipv4_tx;
static uint64_t g_libuinet_icmp_rx;
static uint64_t g_libuinet_icmp_tx;
static uint64_t g_libuinet_arp_reply_tx;
static uint64_t g_libuinet_icmp_echo_reply_tx;
static uint64_t g_libuinet_control_replies;
static uint64_t g_libuinet_udp_echo_rx;
static uint64_t g_libuinet_udp_echo_tx;
static uint64_t g_libuinet_tcp_echo_accepts;
static uint64_t g_libuinet_tcp_echo_rx;
static uint64_t g_libuinet_tcp_echo_tx;
static uint64_t g_libuinet_tcp_echo_closes;
static uint64_t g_libuinet_dns_queries;
static uint64_t g_libuinet_dns_answers;
static uint64_t g_libuinet_http_requests;
static uint64_t g_libuinet_http_responses;
static uint64_t g_libuinet_https_requests;
static uint64_t g_libuinet_https_responses;
static uint64_t g_libuinet_tls_send_calls;
static uint64_t g_libuinet_tls_send_bytes;
static uint64_t g_libuinet_tls_recv_calls;
static uint64_t g_libuinet_tls_recv_bytes;
static uint64_t g_libuinet_tls_recv_want;

static void netd_cpuid(unsigned leaf, unsigned subleaf, unsigned *eax, unsigned *ebx, unsigned *ecx, unsigned *edx)
{
    unsigned a;
    unsigned b;
    unsigned c;
    unsigned d;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(subleaf));
    *eax = a;
    *ebx = b;
    *ecx = c;
    *edx = d;
}

static int netd_cpu_has_rdrand(void)
{
    unsigned eax;
    unsigned ebx;
    unsigned ecx;
    unsigned edx;
    netd_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    return (ecx & (1u << 30)) != 0;
}

static int netd_rdrand64(uint64_t *value)
{
    unsigned char ok;
    uint64_t out;
    for (unsigned i = 0; i < 16; i++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(out), "=qm"(ok));
        if (ok != 0) {
            *value = out;
            return 1;
        }
    }
    return 0;
}

static uint16_t netd_bswap16(uint16_t value)
{
    return (uint16_t)((value << 8) | (value >> 8));
}

static uint16_t netd_read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void netd_write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void netd_write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void netd_ipv4_be_to_text(uint32_t ip_be, char *buffer, size_t buffer_len)
{
    const uint8_t *ip = (const uint8_t *)&ip_be;
    snprintf(buffer, buffer_len, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static uint16_t netd_checksum(const void *data, size_t len)
{
    const uint8_t *bytes = data;
    uint32_t sum = 0;

    while (len >= 2) {
        sum += netd_read_be16(bytes);
        bytes += 2;
        len -= 2;
    }
    if (len != 0) {
        sum += (uint16_t)((uint16_t)bytes[0] << 8);
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static uint16_t netd_tcp_checksum_ipv4(const uint8_t *ip, const uint8_t *tcp, size_t tcp_len)
{
    uint32_t sum = 0;
    for (unsigned i = 0; i < 4; i += 2) {
        sum += netd_read_be16(ip + 12 + i);
        sum += netd_read_be16(ip + 16 + i);
    }
    sum += 6;
    sum += (uint16_t)tcp_len;
    const uint8_t *bytes = tcp;
    size_t len = tcp_len;
    while (len >= 2) {
        sum += netd_read_be16(bytes);
        bytes += 2;
        len -= 2;
    }
    if (len != 0) {
        sum += (uint16_t)((uint16_t)bytes[0] << 8);
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static int netd_frame_is_ipv4_icmp(const uint8_t *frame, size_t frame_len)
{
    if (frame_len < 14 + 20 || netd_read_be16(frame + 12) != 0x0800) {
        return 0;
    }

    const uint8_t *ip = frame + 14;
    size_t ip_header_len = (size_t)(ip[0] & 0x0fu) * 4u;
    if ((ip[0] >> 4) != 4 || ip_header_len < 20 || frame_len < 14 + ip_header_len) {
        return 0;
    }
    return ip[9] == 1;
}

static void netd_libuinet_trace_ipv4_tcp_rx(const uint8_t *frame, size_t frame_len)
{
    if (!g_libuinet_trace) {
        return;
    }
    if (frame_len < 14 + 20 || netd_read_be16(frame + 12) != 0x0800) {
        return;
    }
    const uint8_t *ip = frame + 14;
    size_t ip_header_len = (size_t)(ip[0] & 0x0fu) * 4u;
    if ((ip[0] >> 4) != 4 || ip_header_len < 20 || ip[9] != 6 || frame_len < 14 + ip_header_len + 20) {
        return;
    }
    const uint8_t *tcp = ip + ip_header_len;
    uint16_t src_port = netd_read_be16(tcp + 0);
    uint16_t dst_port = netd_read_be16(tcp + 2);
    uint8_t flags = tcp[13];
    if (src_port == NETD_HTTPS_PORT || dst_port == NETD_HTTPS_PORT) {
        uint16_t total_len = netd_read_be16(ip + 2);
        size_t tcp_len = total_len >= ip_header_len ? (size_t)total_len - ip_header_len : 0;
        uint16_t tcp_sum = tcp_len != 0 && 14u + ip_header_len + tcp_len <= frame_len ?
            netd_tcp_checksum_ipv4(ip, tcp, tcp_len) : 0xffffu;
        printf("[netd] tcp rx https src=%u dst=%u flags=0x%02x total_len=%u frame_len=%u\n",
               src_port,
               dst_port,
               flags,
               (unsigned)total_len,
               (unsigned)frame_len);
        printf("[netd] tcp rx https checksum=0x%04x tcp_len=%u\n",
               tcp_sum,
               (unsigned)tcp_len);
    }
}

static void netd_libuinet_observe_rx_frame(const void *frame_ptr, size_t frame_len)
{
    const uint8_t *frame = frame_ptr;
    if (frame == NULL || frame_len < 14) {
        return;
    }

    uint16_t ether_type = netd_read_be16(frame + 12);
    if (ether_type == 0x0806) {
        g_libuinet_arp_rx++;
    } else if (ether_type == 0x0800) {
        g_libuinet_ipv4_rx++;
        netd_libuinet_trace_ipv4_tcp_rx(frame, frame_len);
        if (netd_frame_is_ipv4_icmp(frame, frame_len)) {
            g_libuinet_icmp_rx++;
        }
    }
}

static void netd_libuinet_observe_tx_frame(const void *frame_ptr, size_t frame_len)
{
    const uint8_t *frame = frame_ptr;
    if (frame == NULL || frame_len < 14) {
        return;
    }

    uint16_t ether_type = netd_read_be16(frame + 12);
    if (ether_type == 0x0806) {
        g_libuinet_arp_tx++;
        if (frame_len >= 42 &&
            netd_read_be16(frame + 20) == 2 &&
            memcmp(frame + 28, k_netd_local_ip, sizeof(k_netd_local_ip)) == 0 &&
            memcmp(frame + 38, k_netd_smoke_peer_ip, sizeof(k_netd_smoke_peer_ip)) == 0) {
            g_libuinet_arp_reply_tx++;
        }
    } else if (ether_type == 0x0800) {
        g_libuinet_ipv4_tx++;
        if (netd_frame_is_ipv4_icmp(frame, frame_len)) {
            const uint8_t *ip = frame + 14;
            size_t ip_header_len = (size_t)(ip[0] & 0x0fu) * 4u;
            const uint8_t *icmp = ip + ip_header_len;
            g_libuinet_icmp_tx++;
            if (frame_len >= 14 + ip_header_len + 8 &&
                icmp[0] == 0 &&
                memcmp(ip + 12, k_netd_local_ip, sizeof(k_netd_local_ip)) == 0 &&
                memcmp(ip + 16, k_netd_smoke_peer_ip, sizeof(k_netd_smoke_peer_ip)) == 0) {
                g_libuinet_icmp_echo_reply_tx++;
            }
        }
    }
}

static int netd_libuinet_send_frame(const void *frame, size_t frame_len)
{
    g_libuinet_tx_frames++;
    netd_libuinet_observe_tx_frame(frame, frame_len);
    int status = kb_net_device_tx_frame(frame, frame_len);
    if (status != 0 || (g_libuinet_trace && frame_len > 1518)) {
        printf("[netd] tx frame status=%d len=%u tx=%llu\n",
               status,
               (unsigned)frame_len,
               (unsigned long long)g_libuinet_tx_frames);
    }
    return status;
}

static int netd_libuinet_tx(void *arg, const void *frame, size_t frame_len)
{
    (void)arg;
    return netd_libuinet_send_frame(frame, frame_len);
}

static int netd_libuinet_configure_ipv4(void)
{
    const char *ifname = uinet_ifgenericname(g_libuinet_if);
    if (ifname == NULL || ifname[0] == '\0') {
        ifname = uinet_ifaliasname(g_libuinet_if);
    }
    if (ifname == NULL || ifname[0] == '\0') {
        fprintf(stderr, "[netd] libuinet pachaos if has no name\n");
        return 7;
    }

    int status = uinet_interface_add_alias(uinet_instance_default(),
                                           ifname,
                                           NETD_IPV4_ADDR,
                                           NETD_IPV4_BROADCAST,
                                           NETD_IPV4_MASK);
    if (status != 0) {
        fprintf(stderr, "[netd] libuinet ipv4 alias failed if=%s status=%d\n", ifname, status);
        return 7;
    }

    status = uinet_interface_up(uinet_instance_default(), ifname, 0, 0);
    if (status != 0) {
        fprintf(stderr, "[netd] libuinet interface up failed if=%s status=%d\n", ifname, status);
        return 7;
    }

    printf("[netd] libuinet ipv4 if=%s addr=%s mask=%s\n", ifname, NETD_IPV4_ADDR, NETD_IPV4_MASK);
    return 0;
}

static int netd_libuinet_configure_default_route(void)
{
    int status = uinet_route_add_default(uinet_instance_default(), NETD_IPV4_GATEWAY);
    if (status != 0) {
        fprintf(stderr, "[netd] libuinet default route failed gateway=%s status=%d\n",
                NETD_IPV4_GATEWAY,
                status);
        return 7;
    }

    printf("[netd] libuinet route default gateway=%s\n", NETD_IPV4_GATEWAY);
    return 0;
}

static int netd_libuinet_start_udp_echo(void)
{
    struct uinet_sockaddr_in sin;
    struct uinet_in_addr addr;
    int optval = 1;
    int status = uinet_socreate(uinet_instance_default(),
                                UINET_PF_INET,
                                &g_libuinet_udp_echo,
                                UINET_SOCK_DGRAM,
                                UINET_IPPROTO_UDP);
    if (status != 0) {
        fprintf(stderr, "[netd] udp echo socket create failed status=%d\n", status);
        return 7;
    }

    status = uinet_sosetsockopt(g_libuinet_udp_echo,
                                UINET_SOL_SOCKET,
                                UINET_SO_REUSEADDR,
                                &optval,
                                sizeof(optval));
    if (status != 0) {
        fprintf(stderr, "[netd] udp echo reuseaddr failed status=%d\n", status);
        return 7;
    }

    if (uinet_inet_pton(UINET_AF_INET, NETD_IPV4_ADDR, &addr) <= 0) {
        fprintf(stderr, "[netd] udp echo invalid bind addr=%s\n", NETD_IPV4_ADDR);
        return 7;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_len = sizeof(sin);
    sin.sin_family = UINET_AF_INET;
    sin.sin_port = netd_bswap16((uint16_t)NETD_UDP_ECHO_PORT);
    sin.sin_addr = addr;

    status = uinet_sobind(g_libuinet_udp_echo, (struct uinet_sockaddr *)&sin);
    if (status != 0) {
        fprintf(stderr, "[netd] udp echo bind failed addr=%s port=%u status=%d\n",
                NETD_IPV4_ADDR,
                NETD_UDP_ECHO_PORT,
                status);
        return 7;
    }

    uinet_sosetnonblocking(g_libuinet_udp_echo, 1);
    printf("[netd] udp echo ready addr=%s port=%u hostfwd=127.0.0.1:10015\n",
           NETD_IPV4_ADDR,
           NETD_UDP_ECHO_PORT);
    return 0;
}

static void netd_libuinet_poll_udp_echo(void)
{
    if (g_libuinet_udp_echo == NULL) {
        return;
    }

    for (unsigned i = 0; i < 16; i++) {
        unsigned char buffer[2048];
        struct uinet_iovec iov;
        struct uinet_uio uio;
        struct uinet_sockaddr *peer = NULL;
        int flags = 0;

        if (!uinet_soreadable(g_libuinet_udp_echo, 0)) {
            break;
        }

        iov.iov_base = buffer;
        iov.iov_len = sizeof(buffer);
        uio.uio_iov = &iov;
        uio.uio_iovcnt = 1;
        uio.uio_offset = 0;
        uio.uio_resid = (int64_t)sizeof(buffer);

        int status = uinet_soreceive(g_libuinet_udp_echo, &peer, &uio, &flags);
        if (status != 0) {
            if (g_libuinet_trace) {
                printf("[netd] udp echo receive status=%d\n", status);
            }
            if (peer != NULL) {
                uinet_free_sockaddr(peer);
            }
            break;
        }

        size_t received = sizeof(buffer) - (size_t)uio.uio_resid;
        if (received == 0) {
            if (peer != NULL) {
                uinet_free_sockaddr(peer);
            }
            continue;
        }

        g_libuinet_udp_echo_rx++;
        iov.iov_base = buffer;
        iov.iov_len = received;
        uio.uio_iov = &iov;
        uio.uio_iovcnt = 1;
        uio.uio_offset = 0;
        uio.uio_resid = (int64_t)received;

        status = uinet_sosend(g_libuinet_udp_echo, peer, &uio, 0);
        if (status == 0) {
            g_libuinet_udp_echo_tx++;
            if (g_libuinet_trace || g_libuinet_udp_echo_tx <= 4) {
                printf("[netd] udp echo packet bytes=%u rx=%llu tx=%llu\n",
                       (unsigned)received,
                       (unsigned long long)g_libuinet_udp_echo_rx,
                       (unsigned long long)g_libuinet_udp_echo_tx);
            }
        } else {
            fprintf(stderr, "[netd] udp echo send failed status=%d bytes=%u\n",
                    status,
                    (unsigned)received);
        }

        if (peer != NULL) {
            uinet_free_sockaddr(peer);
        }
    }
}

static int netd_libuinet_bind_ipv4(struct uinet_socket *socket, uint16_t port, const char *service_name)
{
    struct uinet_sockaddr_in sin;
    struct uinet_in_addr addr;
    int optval = 1;

    int status = uinet_sosetsockopt(socket,
                                    UINET_SOL_SOCKET,
                                    UINET_SO_REUSEADDR,
                                    &optval,
                                    sizeof(optval));
    if (status != 0) {
        fprintf(stderr, "[netd] %s reuseaddr failed status=%d\n", service_name, status);
        return 7;
    }

    if (uinet_inet_pton(UINET_AF_INET, NETD_IPV4_ADDR, &addr) <= 0) {
        fprintf(stderr, "[netd] %s invalid bind addr=%s\n", service_name, NETD_IPV4_ADDR);
        return 7;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_len = sizeof(sin);
    sin.sin_family = UINET_AF_INET;
    sin.sin_port = netd_bswap16(port);
    sin.sin_addr = addr;

    status = uinet_sobind(socket, (struct uinet_sockaddr *)&sin);
    if (status != 0) {
        fprintf(stderr, "[netd] %s bind failed addr=%s port=%u status=%d\n",
                service_name,
                NETD_IPV4_ADDR,
                (unsigned)port,
                status);
        return 7;
    }
    return 0;
}

static int netd_libuinet_start_tcp_echo(void)
{
    int status = uinet_socreate(uinet_instance_default(),
                                UINET_PF_INET,
                                &g_libuinet_tcp_listener,
                                UINET_SOCK_STREAM,
                                UINET_IPPROTO_TCP);
    if (status != 0) {
        fprintf(stderr, "[netd] tcp echo socket create failed status=%d\n", status);
        return 7;
    }

    status = netd_libuinet_bind_ipv4(g_libuinet_tcp_listener,
                                     (uint16_t)NETD_TCP_ECHO_PORT,
                                     "tcp echo");
    if (status != 0) {
        return status;
    }

    int optval = 1;
    status = uinet_sosetsockopt(g_libuinet_tcp_listener,
                                UINET_IPPROTO_TCP,
                                UINET_TCP_NODELAY,
                                &optval,
                                sizeof(optval));
    if (status != 0) {
        fprintf(stderr, "[netd] tcp echo nodelay failed status=%d\n", status);
        return 7;
    }

    uinet_sosetnonblocking(g_libuinet_tcp_listener, 1);
    status = uinet_solisten(g_libuinet_tcp_listener, 16);
    if (status != 0) {
        fprintf(stderr, "[netd] tcp echo listen failed status=%d\n", status);
        return 7;
    }

    printf("[netd] tcp echo ready addr=%s port=%u hostfwd=127.0.0.1:10016\n",
           NETD_IPV4_ADDR,
           NETD_TCP_ECHO_PORT);
    return 0;
}

static int netd_libuinet_tcp_connection_slot(void)
{
    for (unsigned i = 0; i < NETD_TCP_ECHO_MAX_CONNECTIONS; i++) {
        if (g_libuinet_tcp_connections[i] == NULL) {
            return (int)i;
        }
    }
    return -1;
}

static void netd_libuinet_close_tcp_connection(unsigned index)
{
    struct uinet_socket *socket = g_libuinet_tcp_connections[index];
    if (socket == NULL) {
        return;
    }
    (void)uinet_soshutdown(socket, UINET_SHUT_RDWR);
    (void)uinet_soclose(socket);
    g_libuinet_tcp_connections[index] = NULL;
    g_libuinet_tcp_echo_closes++;
    if (g_libuinet_trace || g_libuinet_tcp_echo_closes <= 4) {
        printf("[netd] tcp echo close slot=%u closes=%llu\n",
               index,
               (unsigned long long)g_libuinet_tcp_echo_closes);
    }
}

static void netd_libuinet_poll_tcp_accept(void)
{
    if (g_libuinet_tcp_listener == NULL) {
        return;
    }

    for (unsigned i = 0; i < 8; i++) {
        if (uinet_soreadable(g_libuinet_tcp_listener, 0) <= 0) {
            break;
        }

        int slot = netd_libuinet_tcp_connection_slot();
        struct uinet_socket *accepted = NULL;
        struct uinet_sockaddr *peer = NULL;
        int status = uinet_soaccept(g_libuinet_tcp_listener, &peer, &accepted);
        if (peer != NULL) {
            uinet_free_sockaddr(peer);
        }
        if (status != 0) {
            if (g_libuinet_trace) {
                printf("[netd] tcp echo accept status=%d\n", status);
            }
            break;
        }
        if (accepted == NULL) {
            break;
        }
        if (slot < 0) {
            fprintf(stderr, "[netd] tcp echo connection table full\n");
            (void)uinet_soshutdown(accepted, UINET_SHUT_RDWR);
            (void)uinet_soclose(accepted);
            continue;
        }

        int optval = 1;
        (void)uinet_sosetsockopt(accepted, UINET_IPPROTO_TCP, UINET_TCP_NODELAY, &optval, sizeof(optval));
        uinet_sosetnonblocking(accepted, 1);
        g_libuinet_tcp_connections[slot] = accepted;
        g_libuinet_tcp_echo_accepts++;
        printf("[netd] tcp echo accept slot=%d accepts=%llu\n",
               slot,
               (unsigned long long)g_libuinet_tcp_echo_accepts);
    }
}

static void netd_libuinet_poll_tcp_connections(void)
{
    for (unsigned i = 0; i < NETD_TCP_ECHO_MAX_CONNECTIONS; i++) {
        struct uinet_socket *socket = g_libuinet_tcp_connections[i];
        if (socket == NULL) {
            continue;
        }

        int readable = uinet_soreadable(socket, 0);
        if (readable < 0) {
            netd_libuinet_close_tcp_connection(i);
            continue;
        }
        if (readable == 0) {
            continue;
        }

        int writable = uinet_sowritable(socket, 0);
        if (writable < 0) {
            netd_libuinet_close_tcp_connection(i);
            continue;
        }
        if (writable == 0) {
            continue;
        }

        unsigned char buffer[4096];
        size_t read_size = (size_t)readable;
        if (read_size > sizeof(buffer)) {
            read_size = sizeof(buffer);
        }
        if (read_size > (size_t)writable) {
            read_size = (size_t)writable;
        }
        if (read_size == 0) {
            continue;
        }

        struct uinet_iovec iov;
        struct uinet_uio uio;
        iov.iov_base = buffer;
        iov.iov_len = read_size;
        uio.uio_iov = &iov;
        uio.uio_iovcnt = 1;
        uio.uio_offset = 0;
        uio.uio_resid = (int64_t)read_size;

        int status = uinet_soreceive(socket, NULL, &uio, NULL);
        if (status != 0) {
            if (g_libuinet_trace) {
                printf("[netd] tcp echo receive slot=%u status=%d\n", i, status);
            }
            netd_libuinet_close_tcp_connection(i);
            continue;
        }

        size_t received = read_size - (size_t)uio.uio_resid;
        if (received == 0) {
            netd_libuinet_close_tcp_connection(i);
            continue;
        }
        g_libuinet_tcp_echo_rx++;

        iov.iov_base = buffer;
        iov.iov_len = received;
        uio.uio_iov = &iov;
        uio.uio_iovcnt = 1;
        uio.uio_offset = 0;
        uio.uio_resid = (int64_t)received;
        status = uinet_sosend(socket, NULL, &uio, 0);
        if (status != 0) {
            fprintf(stderr, "[netd] tcp echo send failed slot=%u status=%d bytes=%u\n",
                    i,
                    status,
                    (unsigned)received);
            netd_libuinet_close_tcp_connection(i);
            continue;
        }

        size_t sent = received - (size_t)uio.uio_resid;
        g_libuinet_tcp_echo_tx++;
        if (g_libuinet_trace || g_libuinet_tcp_echo_tx <= 4) {
            printf("[netd] tcp echo packet slot=%u rx_bytes=%u tx_bytes=%u rx=%llu tx=%llu\n",
                   i,
                   (unsigned)received,
                   (unsigned)sent,
                   (unsigned long long)g_libuinet_tcp_echo_rx,
                   (unsigned long long)g_libuinet_tcp_echo_tx);
        }
    }
}

static void netd_libuinet_poll_tcp_echo(void)
{
    netd_libuinet_poll_tcp_accept();
    netd_libuinet_poll_tcp_connections();
}

static size_t netd_dns_write_query(uint8_t *buffer, size_t buffer_len, const char *host)
{
    size_t off = 12;
    const char *label = host;

    if (buffer_len < 18) {
        return 0;
    }
    memset(buffer, 0, buffer_len);
    netd_write_be16(buffer + 0, NETD_DNS_TXID);
    netd_write_be16(buffer + 2, 0x0100);
    netd_write_be16(buffer + 4, 1);

    while (*label != '\0') {
        const char *dot = label;
        while (*dot != '\0' && *dot != '.') {
            dot++;
        }
        size_t label_len = (size_t)(dot - label);
        if (label_len == 0 || label_len > 63 || off + 1 + label_len + 5 > buffer_len) {
            return 0;
        }
        buffer[off++] = (uint8_t)label_len;
        memcpy(buffer + off, label, label_len);
        off += label_len;
        label = (*dot == '.') ? dot + 1 : dot;
    }

    buffer[off++] = 0;
    netd_write_be16(buffer + off, 1);
    off += 2;
    netd_write_be16(buffer + off, 1);
    off += 2;
    return off;
}

static int netd_dns_skip_name(const uint8_t *buffer, size_t len, size_t *off)
{
    size_t pos = *off;
    for (unsigned depth = 0; depth < 128; depth++) {
        if (pos >= len) {
            return 0;
        }
        uint8_t c = buffer[pos];
        if ((c & 0xc0u) == 0xc0u) {
            if (pos + 1 >= len) {
                return 0;
            }
            *off = pos + 2;
            return 1;
        }
        if (c == 0) {
            *off = pos + 1;
            return 1;
        }
        if ((c & 0xc0u) != 0 || pos + 1 + c > len) {
            return 0;
        }
        pos += 1 + c;
    }
    return 0;
}

static int netd_dns_parse_a_answer(const uint8_t *buffer, size_t len, uint32_t *ip_be)
{
    if (len < 12 ||
        netd_read_be16(buffer + 0) != NETD_DNS_TXID ||
        (netd_read_be16(buffer + 2) & 0x8000u) == 0 ||
        (netd_read_be16(buffer + 2) & 0x000fu) != 0) {
        return 0;
    }

    uint16_t qdcount = netd_read_be16(buffer + 4);
    uint16_t ancount = netd_read_be16(buffer + 6);
    size_t off = 12;

    for (uint16_t i = 0; i < qdcount; i++) {
        if (!netd_dns_skip_name(buffer, len, &off) || off + 4 > len) {
            return 0;
        }
        off += 4;
    }

    for (uint16_t i = 0; i < ancount; i++) {
        if (!netd_dns_skip_name(buffer, len, &off) || off + 10 > len) {
            return 0;
        }
        uint16_t type = netd_read_be16(buffer + off);
        uint16_t cls = netd_read_be16(buffer + off + 2);
        uint16_t rdlen = netd_read_be16(buffer + off + 8);
        off += 10;
        if (off + rdlen > len) {
            return 0;
        }
        if (type == 1 && cls == 1 && rdlen == 4) {
            memcpy(ip_be, buffer + off, 4);
            return 1;
        }
        off += rdlen;
    }
    return 0;
}

static void netd_mbedtls_error_text(int status, char *buffer, size_t buffer_len);

static void netd_libuinet_http_smoke_fail(const char *stage, int status)
{
    if (status < 0) {
        char error_text[96];
        netd_mbedtls_error_text(status, error_text, sizeof(error_text));
        fprintf(stderr, "[netd] http smoke %s failed status=%d text=\"%s\"\n",
                stage,
                status,
                error_text);
    } else {
        fprintf(stderr, "[netd] http smoke %s failed status=%d\n", stage, status);
    }
    if (g_libuinet_http_smoke.dns_socket != NULL) {
        (void)uinet_soclose(g_libuinet_http_smoke.dns_socket);
        g_libuinet_http_smoke.dns_socket = NULL;
    }
    if (g_libuinet_http_smoke.http_socket != NULL) {
        (void)uinet_soshutdown(g_libuinet_http_smoke.http_socket, UINET_SHUT_RDWR);
        (void)uinet_soclose(g_libuinet_http_smoke.http_socket);
        g_libuinet_http_smoke.http_socket = NULL;
    }
    if (g_libuinet_http_smoke.https_socket != NULL) {
        (void)uinet_soshutdown(g_libuinet_http_smoke.https_socket, UINET_SHUT_RDWR);
        (void)uinet_soclose(g_libuinet_http_smoke.https_socket);
        g_libuinet_http_smoke.https_socket = NULL;
    }
    if (g_libuinet_http_smoke.ssl_initialized) {
        mbedtls_ssl_free(&g_libuinet_http_smoke.ssl);
        mbedtls_ssl_config_free(&g_libuinet_http_smoke.ssl_config);
        mbedtls_ctr_drbg_free(&g_libuinet_http_smoke.ctr_drbg);
        mbedtls_x509_crt_free(&g_libuinet_http_smoke.ca_chain);
        g_libuinet_http_smoke.ssl_initialized = 0;
    }
    g_libuinet_http_smoke.state = NETD_HTTP_SMOKE_ERROR;
}

static int netd_libuinet_start_http_smoke(void)
{
    uint8_t query[256];
    struct uinet_iovec iov;
    struct uinet_uio uio;
    struct uinet_sockaddr_in dns_addr;
    struct uinet_in_addr addr;

    memset(&g_libuinet_http_smoke, 0, sizeof(g_libuinet_http_smoke));

    int status = uinet_socreate(uinet_instance_default(),
                                UINET_PF_INET,
                                &g_libuinet_http_smoke.dns_socket,
                                UINET_SOCK_DGRAM,
                                UINET_IPPROTO_UDP);
    if (status != 0) {
        netd_libuinet_http_smoke_fail("dns socket create", status);
        return 0;
    }
    uinet_sosetnonblocking(g_libuinet_http_smoke.dns_socket, 1);

    if (uinet_inet_pton(UINET_AF_INET, NETD_DNS_SERVER, &addr) <= 0) {
        netd_libuinet_http_smoke_fail("dns server parse", -22);
        return 0;
    }

    memset(&dns_addr, 0, sizeof(dns_addr));
    dns_addr.sin_len = sizeof(dns_addr);
    dns_addr.sin_family = UINET_AF_INET;
    dns_addr.sin_port = netd_bswap16((uint16_t)NETD_DNS_PORT);
    dns_addr.sin_addr = addr;

    size_t query_len = netd_dns_write_query(query, sizeof(query), NETD_HTTP_HOST);
    if (query_len == 0) {
        netd_libuinet_http_smoke_fail("dns query encode", -22);
        return 0;
    }

    iov.iov_base = query;
    iov.iov_len = query_len;
    uio.uio_iov = &iov;
    uio.uio_iovcnt = 1;
    uio.uio_offset = 0;
    uio.uio_resid = (int64_t)query_len;

    status = uinet_sosend(g_libuinet_http_smoke.dns_socket, (struct uinet_sockaddr *)&dns_addr, &uio, 0);
    if (status != 0) {
        netd_libuinet_http_smoke_fail("dns send", status);
        return 0;
    }

    g_libuinet_dns_queries++;
    g_libuinet_http_smoke.state = NETD_HTTP_SMOKE_DNS_WAIT;
    printf("[netd] dns query host=%s server=%s\n", NETD_HTTP_HOST, NETD_DNS_SERVER);
    return 0;
}

static int netd_mbedtls_entropy(void *ctx, unsigned char *output, size_t len)
{
    (void)ctx;
    while (len != 0) {
        long got = pacha_getrandom(output, len, 0);
        if (got > 0) {
            output += (size_t)got;
            len -= (size_t)got;
            continue;
        }
        break;
    }
    if (len == 0) {
        return 0;
    }

    static int rdrand_supported = -1;
    if (rdrand_supported < 0) {
        rdrand_supported = netd_cpu_has_rdrand();
    }
    if (!rdrand_supported) {
        return -0x003c;
    }

    while (len != 0) {
        uint64_t random_value;
        if (!netd_rdrand64(&random_value)) {
            return -0x003c;
        }
        size_t chunk = len < sizeof(random_value) ? len : sizeof(random_value);
        memcpy(output, &random_value, chunk);
        output += chunk;
        len -= chunk;
    }
    return 0;
}

static int netd_mbedtls_send(void *ctx, const unsigned char *buffer, size_t len)
{
    struct uinet_socket *socket = ctx;
    struct uinet_iovec iov;
    struct uinet_uio uio;

    if (socket == NULL || buffer == NULL || len == 0) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    iov.iov_base = (void *)buffer;
    iov.iov_len = len;
    uio.uio_iov = &iov;
    uio.uio_iovcnt = 1;
    uio.uio_offset = 0;
    uio.uio_resid = (int64_t)len;

    int status = uinet_sosend(socket, NULL, &uio, 0);
    if (status == UINET_EWOULDBLOCK || status == UINET_EAGAIN) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (status != 0) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    size_t sent = len - (size_t)uio.uio_resid;
    g_libuinet_tls_send_calls++;
    g_libuinet_tls_send_bytes += sent;
    if (g_libuinet_trace) {
        printf("[netd] tls send call=%llu requested=%u sent=%u total=%llu\n",
               (unsigned long long)g_libuinet_tls_send_calls,
               (unsigned)len,
               (unsigned)sent,
               (unsigned long long)g_libuinet_tls_send_bytes);
    }
    return sent == 0 ? MBEDTLS_ERR_SSL_WANT_WRITE : (int)sent;
}

static int netd_mbedtls_recv(void *ctx, unsigned char *buffer, size_t len)
{
    struct uinet_socket *socket = ctx;
    struct uinet_iovec iov;
    struct uinet_uio uio;

    if (socket == NULL || buffer == NULL || len == 0) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    int readable = uinet_soreadable(socket, 0);
    if (readable == 0) {
        g_libuinet_tls_recv_want++;
        if (g_libuinet_trace) {
            printf("[netd] tls recv want call=%llu len=%u readable=0\n",
                   (unsigned long long)g_libuinet_tls_recv_want,
                   (unsigned)len);
        }
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (readable < 0) {
        return 0;
    }

    size_t read_size = (size_t)readable;
    if (read_size > len) {
        read_size = len;
    }

    iov.iov_base = buffer;
    iov.iov_len = read_size;
    uio.uio_iov = &iov;
    uio.uio_iovcnt = 1;
    uio.uio_offset = 0;
    uio.uio_resid = (int64_t)read_size;

    int status = uinet_soreceive(socket, NULL, &uio, NULL);
    if (status == UINET_EWOULDBLOCK || status == UINET_EAGAIN) {
        g_libuinet_tls_recv_want++;
        if (g_libuinet_trace) {
            printf("[netd] tls recv want call=%llu len=%u readable=%d\n",
                   (unsigned long long)g_libuinet_tls_recv_want,
                   (unsigned)len,
                   readable);
        }
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (status != 0) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    size_t received = read_size - (size_t)uio.uio_resid;
    g_libuinet_tls_recv_calls++;
    g_libuinet_tls_recv_bytes += received;
    if (g_libuinet_trace) {
        printf("[netd] tls recv call=%llu requested=%u received=%u total=%llu\n",
               (unsigned long long)g_libuinet_tls_recv_calls,
               (unsigned)len,
               (unsigned)received,
               (unsigned long long)g_libuinet_tls_recv_bytes);
    }
    return received == 0 ? MBEDTLS_ERR_SSL_WANT_READ : (int)received;
}

static void netd_mbedtls_error_text(int status, char *buffer, size_t buffer_len)
{
    if (buffer_len == 0) {
        return;
    }
    mbedtls_strerror(status, buffer, buffer_len);
    buffer[buffer_len - 1] = '\0';
}

static uint64_t g_netd_filed_request_id;

static int netd_filed_create_wire_page(int *out_fd, void **out_page)
{
    if (out_fd == NULL || out_page == NULL) {
        return -22;
    }
    *out_fd = -1;
    *out_page = NULL;

    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    int fd = pacha_vmo_create(PACHA_SERVICE_PAGE_BYTES, rights, 0);
    if (fd < 16) {
        return fd;
    }
    void *page = pacha_mmap(
        fd,
        PACHA_SERVICE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (page == NULL) {
        (void)pacha_fd_close(fd);
        return -2;
    }
    memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
    *out_fd = fd;
    *out_page = page;
    return 0;
}

static void netd_filed_destroy_wire_page(int page_fd, void *page)
{
    if (page != NULL) {
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
    }
    if (page_fd >= 16) {
        (void)pacha_fd_close(page_fd);
    }
}

static void *netd_filed_payload(void *page)
{
    return page == NULL ? NULL : (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;
}

static int netd_filed_call(
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result,
    struct pacha_ipc_msg *out_reply,
    struct pacha_ipc_fd *reply_fds,
    uint64_t reply_fd_capacity)
{
    if (out_result == NULL || page_fd < 16 || page == NULL ||
        payload_size > PACHA_SERVICE_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES)
    {
        return -22;
    }
    if (NETD_FILED_ENDPOINT_FD < 16) {
        return -9;
    }

    const uint64_t request_id = ++g_netd_filed_request_id;
    pacha_service_envelope_t *header = (pacha_service_envelope_t *)page;
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = FILED_SERVICE_ID;
    header->op = op;
    header->flags = payload_size != 0 ? PACHA_SERVICE_FLAG_PAGE_PAYLOAD : 0;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = payload_size;

    struct pacha_ipc_fd fd_item;
    memset(&fd_item, 0, sizeof(fd_item));
    fd_item.fd = (uint64_t)(uint32_t)page_fd;
    fd_item.rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;

    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = request_id,
        .fds = &fd_item,
        .fd_count = 1u,
    };
    const int reply_fd = pacha_ipc_call(NETD_FILED_ENDPOINT_FD, &request);
    if (reply_fd < 16) {
        return reply_fd;
    }

    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    reply.fds = reply_fds;
    reply.fd_capacity = reply_fd_capacity;
    const int recv_status = pacha_ipc_recv_wait(reply_fd, &reply, PACHA_FD_WAIT_FOREVER);
    (void)pacha_fd_close(reply_fd);
    if (recv_status != 0) {
        return recv_status;
    }
    const pacha_service_envelope_t *reply_header =
        (const pacha_service_envelope_t *)page;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
        reply.word3 != request_id ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->service_id != FILED_SERVICE_ID ||
        reply_header->op != op ||
        reply_header->request_id != request_id)
    {
        return -71;
    }
    if (reply_header->status < 0) {
        return (int)reply_header->status;
    }
    if (out_reply != NULL) {
        *out_reply = reply;
    }
    *out_result = reply_header->result;
    return 0;
}

int netd_filed_close_handle(uint64_t handle)
{
    int page_fd = -1;
    void *page = NULL;
    uint64_t ignored = 0;
    int status = netd_filed_create_wire_page(&page_fd, &page);
    if (status != 0) {
        return status;
    }
    filed_handle_request_t *request = (filed_handle_request_t *)netd_filed_payload(page);
    request->handle = handle;
    status = netd_filed_call(
        FILED_OP_VFS_CLOSE,
        page_fd,
        page,
        sizeof(*request),
        &ignored,
        NULL,
        NULL,
        0);
    netd_filed_destroy_wire_page(page_fd, page);
    return status;
}

static int netd_filed_file_vmo(uint64_t handle, uint64_t file_offset, uint64_t length, uint64_t *out_loaded)
{
    if (handle == 0 || length == 0 || out_loaded == NULL) {
        return -22;
    }
    *out_loaded = 0;

    int page_fd = -1;
    void *page = NULL;
    int status = netd_filed_create_wire_page(&page_fd, &page);
    if (status != 0) {
        return status;
    }

    filed_file_vmo_request_t *file_vmo =
        (filed_file_vmo_request_t *)netd_filed_payload(page);
    memset(file_vmo, 0, sizeof(*file_vmo));
    file_vmo->handle = handle;
    file_vmo->file_offset = file_offset;
    file_vmo->length = length;

    struct pacha_ipc_fd reply_fd_item;
    memset(&reply_fd_item, 0, sizeof(reply_fd_item));
    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    status = netd_filed_call(
        FILED_OP_VFS_FILE_VMO,
        page_fd,
        page,
        sizeof(*file_vmo),
        out_loaded,
        &reply,
        &reply_fd_item,
        1);
    netd_filed_destroy_wire_page(page_fd, page);
    if (status != 0) {
        return status;
    }
    if (reply.fd_count != 1 || reply_fd_item.fd < 16) {
        return -5;
    }
    return (int)reply_fd_item.fd;
}

static int netd_filed_open_readonly(const char *path, uint64_t *out_handle)
{
    if (path == NULL || out_handle == NULL) {
        return -22;
    }
    int page_fd = -1;
    void *page = NULL;
    int status = netd_filed_create_wire_page(&page_fd, &page);
    if (status != 0) {
        return status;
    }

    filed_path_request_t *open_req = (filed_path_request_t *)netd_filed_payload(page);
    memset(open_req, 0, sizeof(*open_req));
    open_req->dir_handle = 0;
    open_req->rights = FILED_RIGHT_READ | FILED_RIGHT_STAT;
    open_req->flags = FILED_OPEN_CLOEXEC;
    snprintf(open_req->path, sizeof(open_req->path), "%s", path);

    printf("[netd] filed open begin path=%s\n", path);
    uint64_t handle = 0;
    status = netd_filed_call(
        FILED_OP_VFS_OPENAT,
        page_fd,
        page,
        sizeof(*open_req),
        &handle,
        NULL,
        NULL,
        0);
    printf("[netd] filed open done path=%s status=%d handle=%llu\n",
           path,
           status,
           (unsigned long long)handle);
    netd_filed_destroy_wire_page(page_fd, page);
    if (status != 0) {
        return status;
    }
    *out_handle = handle;
    return 0;
}

static int netd_read_file_alloc(const char *path, unsigned char **out_data, size_t *out_len)
{
    enum { NETD_CA_BUNDLE_MAX_BYTES = 1024u * 1024u };
    if (path == NULL || out_data == NULL || out_len == NULL) {
        return -22;
    }
    *out_data = NULL;
    *out_len = 0;

    uint64_t handle = 0;
    int status = netd_filed_open_readonly(path, &handle);
    if (status != 0) {
        printf("[netd] filed read open failed path=%s status=%d\n", path, status);
        return status;
    }

    uint64_t loaded = 0;
    printf("[netd] filed file_vmo begin path=%s handle=%llu length=%u\n",
           path,
           (unsigned long long)handle,
           NETD_CA_BUNDLE_MAX_BYTES);
    int vmo_fd = netd_filed_file_vmo(handle, 0, NETD_CA_BUNDLE_MAX_BYTES, &loaded);
    printf("[netd] filed file_vmo done path=%s status=%d loaded=%llu\n",
           path,
           vmo_fd >= 16 ? 0 : vmo_fd,
           (unsigned long long)loaded);
    if (vmo_fd < 16) {
        (void)netd_filed_close_handle(handle);
        return vmo_fd;
    }
    if (loaded == 0 || loaded > NETD_CA_BUNDLE_MAX_BYTES) {
        (void)pacha_fd_close(vmo_fd);
        (void)netd_filed_close_handle(handle);
        return -5;
    }

    uint64_t map_size = (NETD_CA_BUNDLE_MAX_BYTES + 4095u) & ~4095ull;
    const unsigned char *mapped = pacha_mmap(vmo_fd, map_size, PACHA_PROT_READ, PACHA_MMAP_SHARED, 0);
    if (mapped == NULL) {
        (void)pacha_fd_close(vmo_fd);
        (void)netd_filed_close_handle(handle);
        return -5;
    }

    unsigned char *data = malloc((size_t)loaded + 1u);
    if (data == NULL) {
        (void)pacha_munmap((void *)mapped, map_size);
        (void)pacha_fd_close(vmo_fd);
        (void)netd_filed_close_handle(handle);
        return -12;
    }
    memcpy(data, mapped, (size_t)loaded);
    (void)pacha_munmap((void *)mapped, map_size);
    (void)pacha_fd_close(vmo_fd);
    status = netd_filed_close_handle(handle);
    if (status != 0) {
        free(data);
        return status;
    }
    data[loaded] = '\0';
    *out_data = data;
    *out_len = (size_t)loaded + 1u;
    return 0;
}

static int netd_mbedtls_load_ca_bundle(void)
{
    const char *path = NETD_CA_BUNDLE_PATH;
    unsigned char *bundle = NULL;
    size_t bundle_len = 0;
    int status = netd_read_file_alloc(path, &bundle, &bundle_len);
    if (status != 0) {
        path = NETD_CA_BUNDLE_FALLBACK_PATH;
        status = netd_read_file_alloc(path, &bundle, &bundle_len);
    }
    if (status != 0) {
        return status;
    }

    status = mbedtls_x509_crt_parse(&g_libuinet_http_smoke.ca_chain, bundle, bundle_len);
    free(bundle);
    if (status > 0) {
        status = 0;
    }
    if (status != 0) {
        return status;
    }

    unsigned count = 0;
    for (const mbedtls_x509_crt *crt = &g_libuinet_http_smoke.ca_chain;
         crt != NULL && crt->raw.p != NULL;
         crt = crt->next) {
        count++;
    }
    printf("[netd] https ca bundle path=%s certs=%u\n", path, count);
    return 0;
}

static void netd_libuinet_start_http_connect(void)
{
    struct uinet_sockaddr_in sin;
    struct uinet_in_addr addr;

    int status = uinet_socreate(uinet_instance_default(),
                                UINET_PF_INET,
                                &g_libuinet_http_smoke.http_socket,
                                UINET_SOCK_STREAM,
                                UINET_IPPROTO_TCP);
    if (status != 0) {
        netd_libuinet_http_smoke_fail("http socket create", status);
        return;
    }
    uinet_sosetnonblocking(g_libuinet_http_smoke.http_socket, 1);

    memcpy(&addr.s_addr, &g_libuinet_http_smoke.resolved_ip_be, sizeof(addr.s_addr));
    memset(&sin, 0, sizeof(sin));
    sin.sin_len = sizeof(sin);
    sin.sin_family = UINET_AF_INET;
    sin.sin_port = netd_bswap16((uint16_t)NETD_HTTP_PORT);
    sin.sin_addr = addr;

    status = uinet_soconnect(g_libuinet_http_smoke.http_socket, (struct uinet_sockaddr *)&sin);
    if (status != 0 && status != UINET_EINPROGRESS && status != UINET_EALREADY && status != UINET_EISCONN) {
        netd_libuinet_http_smoke_fail("http connect", status);
        return;
    }

    g_libuinet_http_smoke.request_len =
        (size_t)snprintf(g_libuinet_http_smoke.request,
                         sizeof(g_libuinet_http_smoke.request),
                         "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: PachaOS-netd/0\r\nConnection: close\r\n\r\n",
                         NETD_HTTP_PATH,
                         NETD_HTTP_HOST);
    if (g_libuinet_http_smoke.request_len >= sizeof(g_libuinet_http_smoke.request)) {
        netd_libuinet_http_smoke_fail("http request encode", -22);
        return;
    }

    g_libuinet_http_smoke.state = NETD_HTTP_SMOKE_CONNECTING;
    printf("[netd] http connect host=%s addr=%s port=%u\n",
           NETD_HTTP_HOST,
           g_libuinet_http_smoke.resolved_ip_text,
           NETD_HTTP_PORT);
}

static void netd_libuinet_poll_dns_smoke(void)
{
    if (g_libuinet_http_smoke.state != NETD_HTTP_SMOKE_DNS_WAIT ||
        g_libuinet_http_smoke.dns_socket == NULL ||
        uinet_soreadable(g_libuinet_http_smoke.dns_socket, 0) <= 0) {
        return;
    }

    uint8_t response[512];
    struct uinet_iovec iov;
    struct uinet_uio uio;
    int flags = 0;

    iov.iov_base = response;
    iov.iov_len = sizeof(response);
    uio.uio_iov = &iov;
    uio.uio_iovcnt = 1;
    uio.uio_offset = 0;
    uio.uio_resid = (int64_t)sizeof(response);

    int status = uinet_soreceive(g_libuinet_http_smoke.dns_socket, NULL, &uio, &flags);
    if (status != 0) {
        netd_libuinet_http_smoke_fail("dns receive", status);
        return;
    }

    size_t received = sizeof(response) - (size_t)uio.uio_resid;
    uint32_t ip_be = 0;
    if (!netd_dns_parse_a_answer(response, received, &ip_be)) {
        netd_libuinet_http_smoke_fail("dns parse", -2);
        return;
    }

    (void)uinet_soclose(g_libuinet_http_smoke.dns_socket);
    g_libuinet_http_smoke.dns_socket = NULL;
    g_libuinet_http_smoke.resolved_ip_be = ip_be;
    netd_ipv4_be_to_text(ip_be,
                         g_libuinet_http_smoke.resolved_ip_text,
                         sizeof(g_libuinet_http_smoke.resolved_ip_text));
    g_libuinet_dns_answers++;
    printf("[netd] dns resolved host=%s addr=%s answers=%llu\n",
           NETD_HTTP_HOST,
           g_libuinet_http_smoke.resolved_ip_text,
           (unsigned long long)g_libuinet_dns_answers);
    netd_libuinet_start_http_connect();
}

static void netd_libuinet_poll_http_connect(void)
{
    if (g_libuinet_http_smoke.state != NETD_HTTP_SMOKE_CONNECTING ||
        g_libuinet_http_smoke.http_socket == NULL) {
        return;
    }

    int socket_error = uinet_sogeterror(g_libuinet_http_smoke.http_socket);
    if (socket_error != 0 && socket_error != UINET_EINPROGRESS && socket_error != UINET_EALREADY) {
        netd_libuinet_http_smoke_fail("http connect complete", socket_error);
        return;
    }

    int writable = uinet_sowritable(g_libuinet_http_smoke.http_socket, 0);
    if (writable < 0) {
        netd_libuinet_http_smoke_fail("http writable", writable);
        return;
    }
    if (writable == 0) {
        return;
    }
    g_libuinet_http_smoke.state = NETD_HTTP_SMOKE_SENDING;
}

static void netd_libuinet_poll_http_send(void)
{
    if (g_libuinet_http_smoke.state != NETD_HTTP_SMOKE_SENDING ||
        g_libuinet_http_smoke.http_socket == NULL) {
        return;
    }

    size_t remaining = g_libuinet_http_smoke.request_len - g_libuinet_http_smoke.request_sent;
    if (remaining == 0) {
        g_libuinet_http_smoke.state = NETD_HTTP_SMOKE_READING;
        g_libuinet_http_requests++;
        printf("[netd] http request sent host=%s requests=%llu\n",
               NETD_HTTP_HOST,
               (unsigned long long)g_libuinet_http_requests);
        return;
    }

    struct uinet_iovec iov;
    struct uinet_uio uio;
    iov.iov_base = g_libuinet_http_smoke.request + g_libuinet_http_smoke.request_sent;
    iov.iov_len = remaining;
    uio.uio_iov = &iov;
    uio.uio_iovcnt = 1;
    uio.uio_offset = 0;
    uio.uio_resid = (int64_t)remaining;

    int status = uinet_sosend(g_libuinet_http_smoke.http_socket, NULL, &uio, 0);
    if (status == UINET_EWOULDBLOCK || status == UINET_EAGAIN) {
        return;
    }
    if (status != 0) {
        netd_libuinet_http_smoke_fail("http send", status);
        return;
    }

    size_t sent = remaining - (size_t)uio.uio_resid;
    g_libuinet_http_smoke.request_sent += sent;
}

static void netd_libuinet_note_http_status(const char *buffer, size_t len)
{
    if (g_libuinet_http_smoke.status_line_len >= sizeof(g_libuinet_http_smoke.status_line) - 1) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        char c = buffer[i];
        if (c == '\r' || c == '\n') {
            g_libuinet_http_smoke.status_line[g_libuinet_http_smoke.status_line_len] = '\0';
            return;
        }
        if (g_libuinet_http_smoke.status_line_len + 1 < sizeof(g_libuinet_http_smoke.status_line)) {
            g_libuinet_http_smoke.status_line[g_libuinet_http_smoke.status_line_len++] = c;
        }
    }
}

static void netd_libuinet_note_https_status(const char *buffer, size_t len)
{
    if (g_libuinet_http_smoke.https_status_line_len >= sizeof(g_libuinet_http_smoke.https_status_line) - 1) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        char c = buffer[i];
        if (c == '\r' || c == '\n') {
            g_libuinet_http_smoke.https_status_line[g_libuinet_http_smoke.https_status_line_len] = '\0';
            return;
        }
        if (g_libuinet_http_smoke.https_status_line_len + 1 < sizeof(g_libuinet_http_smoke.https_status_line)) {
            g_libuinet_http_smoke.https_status_line[g_libuinet_http_smoke.https_status_line_len++] = c;
        }
    }
}

static void netd_libuinet_start_https_connect(void)
{
    struct uinet_sockaddr_in sin;
    struct uinet_in_addr addr;
    static const unsigned char personalization[] = "pachaos-netd-https-smoke";

    int status = uinet_socreate(uinet_instance_default(),
                                UINET_PF_INET,
                                &g_libuinet_http_smoke.https_socket,
                                UINET_SOCK_STREAM,
                                UINET_IPPROTO_TCP);
    if (status != 0) {
        netd_libuinet_http_smoke_fail("https socket create", status);
        return;
    }
    uinet_sosetnonblocking(g_libuinet_http_smoke.https_socket, 1);

    mbedtls_ssl_init(&g_libuinet_http_smoke.ssl);
    mbedtls_ssl_config_init(&g_libuinet_http_smoke.ssl_config);
    mbedtls_ctr_drbg_init(&g_libuinet_http_smoke.ctr_drbg);
    mbedtls_x509_crt_init(&g_libuinet_http_smoke.ca_chain);
    g_libuinet_http_smoke.ssl_initialized = 1;

    status = netd_mbedtls_load_ca_bundle();
    if (status != 0) {
        netd_libuinet_http_smoke_fail("https ca bundle", status);
        return;
    }

    status = mbedtls_ctr_drbg_seed(&g_libuinet_http_smoke.ctr_drbg,
                                   netd_mbedtls_entropy,
                                   NULL,
                                   personalization,
                                   sizeof(personalization) - 1);
    if (status != 0) {
        netd_libuinet_http_smoke_fail("https ctr_drbg seed", status);
        return;
    }

    status = mbedtls_ssl_config_defaults(&g_libuinet_http_smoke.ssl_config,
                                         MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (status != 0) {
        netd_libuinet_http_smoke_fail("https config defaults", status);
        return;
    }

    mbedtls_ssl_conf_authmode(&g_libuinet_http_smoke.ssl_config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&g_libuinet_http_smoke.ssl_config,
                              &g_libuinet_http_smoke.ca_chain,
                              NULL);
    mbedtls_ssl_conf_rng(&g_libuinet_http_smoke.ssl_config,
                         mbedtls_ctr_drbg_random,
                         &g_libuinet_http_smoke.ctr_drbg);

    status = mbedtls_ssl_setup(&g_libuinet_http_smoke.ssl, &g_libuinet_http_smoke.ssl_config);
    if (status != 0) {
        netd_libuinet_http_smoke_fail("https ssl setup", status);
        return;
    }

    status = mbedtls_ssl_set_hostname(&g_libuinet_http_smoke.ssl, NETD_HTTP_HOST);
    if (status != 0) {
        netd_libuinet_http_smoke_fail("https set hostname", status);
        return;
    }
    mbedtls_ssl_set_bio(&g_libuinet_http_smoke.ssl,
                        g_libuinet_http_smoke.https_socket,
                        netd_mbedtls_send,
                        netd_mbedtls_recv,
                        NULL);

    memcpy(&addr.s_addr, &g_libuinet_http_smoke.resolved_ip_be, sizeof(addr.s_addr));
    memset(&sin, 0, sizeof(sin));
    sin.sin_len = sizeof(sin);
    sin.sin_family = UINET_AF_INET;
    sin.sin_port = netd_bswap16((uint16_t)NETD_HTTPS_PORT);
    sin.sin_addr = addr;

    status = uinet_soconnect(g_libuinet_http_smoke.https_socket, (struct uinet_sockaddr *)&sin);
    if (status != 0 && status != UINET_EINPROGRESS && status != UINET_EALREADY && status != UINET_EISCONN) {
        netd_libuinet_http_smoke_fail("https connect", status);
        return;
    }

    g_libuinet_http_smoke.https_request_len =
        (size_t)snprintf(g_libuinet_http_smoke.https_request,
                         sizeof(g_libuinet_http_smoke.https_request),
                         "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: PachaOS-netd-mbedtls/0\r\nConnection: close\r\n\r\n",
                         NETD_HTTP_PATH,
                         NETD_HTTP_HOST);
    if (g_libuinet_http_smoke.https_request_len >= sizeof(g_libuinet_http_smoke.https_request)) {
        netd_libuinet_http_smoke_fail("https request encode", -22);
        return;
    }

    g_libuinet_http_smoke.state = NETD_HTTP_SMOKE_HTTPS_CONNECTING;
    printf("[netd] https connect host=%s addr=%s port=%u tls=mbedtls verify=required ca=%s\n",
           NETD_HTTP_HOST,
           g_libuinet_http_smoke.resolved_ip_text,
           NETD_HTTPS_PORT,
           NETD_CA_BUNDLE_PATH);
}

static void netd_libuinet_poll_https_connect(void)
{
    if (g_libuinet_http_smoke.state != NETD_HTTP_SMOKE_HTTPS_CONNECTING ||
        g_libuinet_http_smoke.https_socket == NULL) {
        return;
    }

    g_libuinet_http_smoke.https_connect_polls++;

    int socket_error = uinet_sogeterror(g_libuinet_http_smoke.https_socket);
    int socket_state = uinet_sogetstate(g_libuinet_http_smoke.https_socket);
    if (socket_error != 0 && socket_error != UINET_EINPROGRESS && socket_error != UINET_EALREADY) {
        netd_libuinet_http_smoke_fail("https connect complete", socket_error);
        return;
    }

    int writable = uinet_sowritable(g_libuinet_http_smoke.https_socket, 0);
    if (writable < 0) {
        netd_libuinet_http_smoke_fail("https writable", writable);
        return;
    }
    if (g_libuinet_trace) {
        printf("[netd] https connect poll polls=%llu connect=%d so_error=%d state=0x%x writable=%d\n",
               (unsigned long long)g_libuinet_http_smoke.https_connect_polls,
               0,
               socket_error,
               socket_state,
               writable);
    }
    if ((socket_state & UINET_SS_ISCONNECTED) == 0) {
        return;
    }
    g_libuinet_http_smoke.state = NETD_HTTP_SMOKE_HTTPS_HANDSHAKE;
}

static void netd_libuinet_poll_https_handshake(void)
{
    if (g_libuinet_http_smoke.state != NETD_HTTP_SMOKE_HTTPS_HANDSHAKE) {
        return;
    }

    g_libuinet_http_smoke.https_handshake_polls++;
    int status = mbedtls_ssl_handshake(&g_libuinet_http_smoke.ssl);
    if (status == MBEDTLS_ERR_SSL_WANT_READ || status == MBEDTLS_ERR_SSL_WANT_WRITE) {
        if (g_libuinet_trace) {
            printf("[netd] https handshake poll polls=%llu status=%d\n",
                   (unsigned long long)g_libuinet_http_smoke.https_handshake_polls,
                   status);
            printf("[netd] https counters tx_frames=%llu rx_frames=%llu tls_tx=%llu tls_rx=%llu\n",
                   (unsigned long long)g_libuinet_tx_frames,
                   (unsigned long long)g_libuinet_rx_frames,
                   (unsigned long long)g_libuinet_tls_send_bytes,
                   (unsigned long long)g_libuinet_tls_recv_bytes);
            printf("[netd] https recv want=%llu state=0x%x so_error=%d readable=%d\n",
                   (unsigned long long)g_libuinet_tls_recv_want,
                   uinet_sogetstate(g_libuinet_http_smoke.https_socket),
                   uinet_sogeterror(g_libuinet_http_smoke.https_socket),
                   uinet_soreadable(g_libuinet_http_smoke.https_socket, 0));
            struct uinet_tcpstat tcpstat;
            memset(&tcpstat, 0, sizeof(tcpstat));
            uinet_gettcpstat(uinet_instance_default(), &tcpstat);
            printf("[netd] tcpstat rcvtotal=%lu rcvpack=%lu rcvbyte=%lu badsum=%lu badoff=%lu memdrop=%lu oopack=%lu oobyte=%lu afterwin=%lu paws=%lu drops=%lu\n",
                   tcpstat.tcps_rcvtotal,
                   tcpstat.tcps_rcvpack,
                   tcpstat.tcps_rcvbyte,
                   tcpstat.tcps_rcvbadsum,
                   tcpstat.tcps_rcvbadoff,
                   tcpstat.tcps_rcvmemdrop,
                   tcpstat.tcps_rcvoopack,
                   tcpstat.tcps_rcvoobyte,
                   tcpstat.tcps_rcvpackafterwin,
                   tcpstat.tcps_pawsdrop,
                   tcpstat.tcps_drops);
        }
        return;
    }
    if (status != 0) {
        uint32_t verify_flags = mbedtls_ssl_get_verify_result(&g_libuinet_http_smoke.ssl);
        if (verify_flags != 0) {
            char verify_info[256];
            mbedtls_x509_crt_verify_info(verify_info, sizeof(verify_info), "  ! ", verify_flags);
            fprintf(stderr,
                    "[netd] https certificate verify failed flags=0x%x info=\"%s\"\n",
                    (unsigned)verify_flags,
                    verify_info);
        }
        netd_libuinet_http_smoke_fail("https handshake", status);
        return;
    }

    uint32_t verify_flags = mbedtls_ssl_get_verify_result(&g_libuinet_http_smoke.ssl);
    if (verify_flags != 0) {
        char verify_info[256];
        mbedtls_x509_crt_verify_info(verify_info, sizeof(verify_info), "  ! ", verify_flags);
        fprintf(stderr,
                "[netd] https certificate verify failed flags=0x%x info=\"%s\"\n",
                (unsigned)verify_flags,
                verify_info);
        netd_libuinet_http_smoke_fail("https verify", -1);
        return;
    }

    printf("[netd] https handshake complete host=%s ciphersuite=\"%s\" verify=ok\n",
           NETD_HTTP_HOST,
           mbedtls_ssl_get_ciphersuite(&g_libuinet_http_smoke.ssl));
    g_libuinet_http_smoke.state = NETD_HTTP_SMOKE_HTTPS_SENDING;
}

static void netd_libuinet_poll_https_send(void)
{
    if (g_libuinet_http_smoke.state != NETD_HTTP_SMOKE_HTTPS_SENDING) {
        return;
    }

    size_t remaining = g_libuinet_http_smoke.https_request_len - g_libuinet_http_smoke.https_request_sent;
    if (remaining == 0) {
        g_libuinet_http_smoke.state = NETD_HTTP_SMOKE_HTTPS_READING;
        g_libuinet_https_requests++;
        printf("[netd] https request sent host=%s requests=%llu\n",
               NETD_HTTP_HOST,
               (unsigned long long)g_libuinet_https_requests);
        return;
    }

    int status = mbedtls_ssl_write(&g_libuinet_http_smoke.ssl,
                                   (const unsigned char *)g_libuinet_http_smoke.https_request +
                                       g_libuinet_http_smoke.https_request_sent,
                                   remaining);
    if (status == MBEDTLS_ERR_SSL_WANT_READ || status == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return;
    }
    if (status < 0) {
        netd_libuinet_http_smoke_fail("https write", status);
        return;
    }
    if (status == 0) {
        return;
    }
    g_libuinet_http_smoke.https_request_sent += (size_t)status;
}

static void netd_libuinet_finish_https_smoke(void)
{
    g_libuinet_https_responses++;
    printf("[netd] https response host=%s bytes=%u status=\"%s\" responses=%llu tls=mbedtls\n",
           NETD_HTTP_HOST,
           (unsigned)g_libuinet_http_smoke.https_response_bytes,
           g_libuinet_http_smoke.https_status_line,
           (unsigned long long)g_libuinet_https_responses);
    if (g_libuinet_http_smoke.https_socket != NULL) {
        (void)uinet_soclose(g_libuinet_http_smoke.https_socket);
        g_libuinet_http_smoke.https_socket = NULL;
    }
    if (g_libuinet_http_smoke.ssl_initialized) {
        mbedtls_ssl_free(&g_libuinet_http_smoke.ssl);
        mbedtls_ssl_config_free(&g_libuinet_http_smoke.ssl_config);
        mbedtls_ctr_drbg_free(&g_libuinet_http_smoke.ctr_drbg);
        mbedtls_x509_crt_free(&g_libuinet_http_smoke.ca_chain);
        g_libuinet_http_smoke.ssl_initialized = 0;
    }
    g_libuinet_http_smoke.state = NETD_HTTP_SMOKE_DONE;
}

static void netd_libuinet_poll_https_read(void)
{
    if (g_libuinet_http_smoke.state != NETD_HTTP_SMOKE_HTTPS_READING) {
        return;
    }

    unsigned char buffer[1024];
    int status = mbedtls_ssl_read(&g_libuinet_http_smoke.ssl, buffer, sizeof(buffer));
    if (status == MBEDTLS_ERR_SSL_WANT_READ || status == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return;
    }
    if (status == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || status == 0) {
        if (g_libuinet_http_smoke.https_response_bytes != 0) {
            netd_libuinet_finish_https_smoke();
        } else {
            netd_libuinet_http_smoke_fail("https read close", status);
        }
        return;
    }
    if (status < 0) {
        netd_libuinet_http_smoke_fail("https read", status);
        return;
    }

    netd_libuinet_note_https_status((const char *)buffer, (size_t)status);
    g_libuinet_http_smoke.https_response_bytes += (size_t)status;
}

static void netd_libuinet_finish_http_smoke(void)
{
    g_libuinet_http_responses++;
    printf("[netd] http response host=%s bytes=%u status=\"%s\" responses=%llu\n",
           NETD_HTTP_HOST,
           (unsigned)g_libuinet_http_smoke.response_bytes,
           g_libuinet_http_smoke.status_line,
           (unsigned long long)g_libuinet_http_responses);
    if (g_libuinet_http_smoke.http_socket != NULL) {
        (void)uinet_soclose(g_libuinet_http_smoke.http_socket);
        g_libuinet_http_smoke.http_socket = NULL;
    }
    netd_libuinet_start_https_connect();
}

static void netd_libuinet_poll_http_read(void)
{
    if (g_libuinet_http_smoke.state != NETD_HTTP_SMOKE_READING ||
        g_libuinet_http_smoke.http_socket == NULL) {
        return;
    }

    int readable = uinet_soreadable(g_libuinet_http_smoke.http_socket, 0);
    if (readable < 0) {
        if (g_libuinet_http_smoke.response_bytes != 0) {
            netd_libuinet_finish_http_smoke();
        } else {
            netd_libuinet_http_smoke_fail("http read close", readable);
        }
        return;
    }
    if (readable == 0) {
        return;
    }

    char buffer[1024];
    size_t read_size = (size_t)readable;
    if (read_size > sizeof(buffer)) {
        read_size = sizeof(buffer);
    }

    struct uinet_iovec iov;
    struct uinet_uio uio;
    iov.iov_base = buffer;
    iov.iov_len = read_size;
    uio.uio_iov = &iov;
    uio.uio_iovcnt = 1;
    uio.uio_offset = 0;
    uio.uio_resid = (int64_t)read_size;

    int status = uinet_soreceive(g_libuinet_http_smoke.http_socket, NULL, &uio, NULL);
    if (status != 0) {
        netd_libuinet_http_smoke_fail("http receive", status);
        return;
    }

    size_t received = read_size - (size_t)uio.uio_resid;
    if (received == 0) {
        return;
    }
    netd_libuinet_note_http_status(buffer, received);
    g_libuinet_http_smoke.response_bytes += received;
}

static void netd_libuinet_poll_http_smoke(void)
{
    netd_libuinet_poll_dns_smoke();
    netd_libuinet_poll_http_connect();
    netd_libuinet_poll_http_send();
    netd_libuinet_poll_http_read();
    netd_libuinet_poll_https_connect();
    netd_libuinet_poll_https_handshake();
    netd_libuinet_poll_https_send();
    netd_libuinet_poll_https_read();
}

static void netd_libuinet_make_arp_request(uint8_t *frame, size_t frame_len)
{
    memset(frame, 0, frame_len);
    memset(frame, 0xff, 6);
    memcpy(frame + 6, k_netd_smoke_peer_mac, 6);
    netd_write_be16(frame + 12, 0x0806);
    netd_write_be16(frame + 14, 1);
    netd_write_be16(frame + 16, 0x0800);
    frame[18] = 6;
    frame[19] = 4;
    netd_write_be16(frame + 20, 1);
    memcpy(frame + 22, k_netd_smoke_peer_mac, 6);
    memcpy(frame + 28, k_netd_smoke_peer_ip, 4);
    memset(frame + 32, 0, 6);
    memcpy(frame + 38, k_netd_local_ip, 4);
}

static void netd_libuinet_make_arp_reply(uint8_t *frame, size_t frame_len)
{
    memset(frame, 0, frame_len);
    memcpy(frame, k_netd_local_mac, 6);
    memcpy(frame + 6, k_netd_smoke_peer_mac, 6);
    netd_write_be16(frame + 12, 0x0806);
    netd_write_be16(frame + 14, 1);
    netd_write_be16(frame + 16, 0x0800);
    frame[18] = 6;
    frame[19] = 4;
    netd_write_be16(frame + 20, 2);
    memcpy(frame + 22, k_netd_smoke_peer_mac, 6);
    memcpy(frame + 28, k_netd_smoke_peer_ip, 4);
    memcpy(frame + 32, k_netd_local_mac, 6);
    memcpy(frame + 38, k_netd_local_ip, 4);
}

static void netd_libuinet_make_icmp_echo_request(uint8_t *frame, size_t frame_len)
{
    static const uint8_t payload[] = { 'p', 'a', 'c', 'h', 'a', 'o', 's' };
    const size_t ip_len = 20 + 8 + sizeof(payload);
    uint8_t *ip = frame + 14;
    uint8_t *icmp = ip + 20;

    memset(frame, 0, frame_len);
    memcpy(frame, k_netd_local_mac, 6);
    memcpy(frame + 6, k_netd_smoke_peer_mac, 6);
    netd_write_be16(frame + 12, 0x0800);

    ip[0] = 0x45;
    ip[1] = 0;
    netd_write_be16(ip + 2, (uint16_t)ip_len);
    netd_write_be16(ip + 4, 0x1234);
    netd_write_be16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = 1;
    memcpy(ip + 12, k_netd_smoke_peer_ip, 4);
    memcpy(ip + 16, k_netd_local_ip, 4);
    netd_write_be16(ip + 10, netd_checksum(ip, 20));

    icmp[0] = 8;
    icmp[1] = 0;
    netd_write_be16(icmp + 4, 0x4242);
    netd_write_be16(icmp + 6, 1);
    memcpy(icmp + 8, payload, sizeof(payload));
    netd_write_be16(icmp + 2, netd_checksum(icmp, 8 + sizeof(payload)));
}

static int netd_libuinet_reply_arp_request(const uint8_t *request, size_t request_len)
{
    uint8_t reply[42];

    if (request_len < sizeof(reply) ||
        netd_read_be16(request + 12) != 0x0806 ||
        netd_read_be16(request + 20) != 1 ||
        memcmp(request + 38, k_netd_local_ip, sizeof(k_netd_local_ip)) != 0) {
        return 0;
    }

    memset(reply, 0, sizeof(reply));
    memcpy(reply, request + 6, 6);
    memcpy(reply + 6, k_netd_local_mac, 6);
    netd_write_be16(reply + 12, 0x0806);
    netd_write_be16(reply + 14, 1);
    netd_write_be16(reply + 16, 0x0800);
    reply[18] = 6;
    reply[19] = 4;
    netd_write_be16(reply + 20, 2);
    memcpy(reply + 22, k_netd_local_mac, 6);
    memcpy(reply + 28, k_netd_local_ip, 4);
    memcpy(reply + 32, request + 22, 6);
    memcpy(reply + 38, request + 28, 4);

    if (netd_libuinet_send_frame(reply, sizeof(reply)) == 0) {
        g_libuinet_control_replies++;
    }
    return 1;
}

static int netd_libuinet_reply_icmp_echo(const uint8_t *request, size_t request_len)
{
    uint8_t reply[1514];

    if (request_len < 14 + 20 + 8 ||
        netd_read_be16(request + 12) != 0x0800 ||
        !netd_frame_is_ipv4_icmp(request, request_len)) {
        return 0;
    }

    const uint8_t *ip = request + 14;
    size_t ip_header_len = (size_t)(ip[0] & 0x0fu) * 4u;
    uint16_t ip_total_len = netd_read_be16(ip + 2);
    if (ip_header_len != 20 ||
        ip_total_len < 28 ||
        14u + ip_total_len > request_len ||
        ip[8] == 0 ||
        memcmp(ip + 16, k_netd_local_ip, sizeof(k_netd_local_ip)) != 0) {
        return 0;
    }

    const uint8_t *icmp = ip + ip_header_len;
    if (icmp[0] != 8 || icmp[1] != 0) {
        return 0;
    }

    size_t reply_len = 14u + ip_total_len;
    if (reply_len > sizeof(reply)) {
        return 0;
    }
    memcpy(reply, request, reply_len);
    memcpy(reply, request + 6, 6);
    memcpy(reply + 6, k_netd_local_mac, 6);

    uint8_t *reply_ip = reply + 14;
    memcpy(reply_ip + 12, ip + 16, 4);
    memcpy(reply_ip + 16, ip + 12, 4);
    reply_ip[8] = 64;
    reply_ip[10] = 0;
    reply_ip[11] = 0;
    netd_write_be16(reply_ip + 10, netd_checksum(reply_ip, ip_header_len));

    uint8_t *reply_icmp = reply_ip + ip_header_len;
    reply_icmp[0] = 0;
    reply_icmp[2] = 0;
    reply_icmp[3] = 0;
    netd_write_be16(reply_icmp + 2, netd_checksum(reply_icmp, ip_total_len - (uint16_t)ip_header_len));

    if (netd_libuinet_send_frame(reply, reply_len) == 0) {
        g_libuinet_control_replies++;
    }
    return 1;
}

static void netd_libuinet_control_receive_frame(const uint8_t *frame, size_t frame_len)
{
    if (frame == NULL || frame_len < 14) {
        return;
    }
    if (netd_read_be16(frame + 12) == 0x0806) {
        (void)netd_libuinet_reply_arp_request(frame, frame_len);
    } else if (netd_read_be16(frame + 12) == 0x0800) {
        (void)netd_libuinet_reply_icmp_echo(frame, frame_len);
    }
}

static int netd_libuinet_run_ping_smoke(void)
{
    uint64_t arp_before = g_libuinet_arp_reply_tx;
    uint64_t icmp_before = g_libuinet_icmp_echo_reply_tx;
    uint8_t arp_request[42];
    uint8_t arp_reply[42];
    uint8_t icmp_request[14 + 20 + 8 + 7];

    netd_libuinet_make_arp_request(arp_request, sizeof(arp_request));
    netd_libuinet_observe_rx_frame(arp_request, sizeof(arp_request));
    netd_libuinet_control_receive_frame(arp_request, sizeof(arp_request));
    int status = uinet_pachaos_if_deliver(g_libuinet_if, arp_request, sizeof(arp_request));
    uinet_instance_sts_events_process(uinet_instance_default());
    if (status != 0) {
        fprintf(stderr, "[netd] libuinet arp smoke deliver failed status=%d\n", status);
        return 7;
    }

    netd_libuinet_make_arp_reply(arp_reply, sizeof(arp_reply));
    netd_libuinet_observe_rx_frame(arp_reply, sizeof(arp_reply));
    netd_libuinet_control_receive_frame(arp_reply, sizeof(arp_reply));
    status = uinet_pachaos_if_deliver(g_libuinet_if, arp_reply, sizeof(arp_reply));
    uinet_instance_sts_events_process(uinet_instance_default());
    if (status != 0) {
        fprintf(stderr, "[netd] libuinet arp reply smoke deliver failed status=%d\n", status);
        return 7;
    }

    netd_libuinet_make_icmp_echo_request(icmp_request, sizeof(icmp_request));
    netd_libuinet_observe_rx_frame(icmp_request, sizeof(icmp_request));
    netd_libuinet_control_receive_frame(icmp_request, sizeof(icmp_request));
    status = uinet_pachaos_if_deliver(g_libuinet_if, icmp_request, sizeof(icmp_request));
    uinet_instance_sts_events_process(uinet_instance_default());
    if (status != 0) {
        fprintf(stderr, "[netd] libuinet icmp smoke deliver failed status=%d\n", status);
        return 7;
    }

    uint64_t arp_replies = g_libuinet_arp_reply_tx - arp_before;
    uint64_t icmp_replies = g_libuinet_icmp_echo_reply_tx - icmp_before;
    printf("[netd] libuinet smoke arp_reply=%llu icmp_echo_reply=%llu rx_arp=%llu rx_icmp=%llu tx=%llu control=%llu\n",
           (unsigned long long)arp_replies,
           (unsigned long long)icmp_replies,
           (unsigned long long)g_libuinet_arp_rx,
           (unsigned long long)g_libuinet_icmp_rx,
           (unsigned long long)g_libuinet_tx_frames,
           (unsigned long long)g_libuinet_control_replies);
    return (arp_replies != 0 && icmp_replies != 0) ? 0 : 7;
}
#endif

int netd_libuinet_start(struct netd_runtime *runtime)
{
    if (runtime == NULL || runtime->cfg == NULL) {
        return 7;
    }
    g_libuinet_state = NETD_LIBUINET_UNLINKED;
    g_libuinet_rx_frames = 0;
    g_libuinet_rx_drops = 0;
    g_libuinet_trace = (runtime->cfg->flags & NETD_BOOT_FLAG_TRACE) != 0;

#if defined(NETD_WITH_LIBUINET)
    g_libuinet_if = NULL;
    g_libuinet_udp_echo = NULL;
    g_libuinet_tcp_listener = NULL;
    for (unsigned i = 0; i < NETD_TCP_ECHO_MAX_CONNECTIONS; i++) {
        g_libuinet_tcp_connections[i] = NULL;
    }
    memset(g_libuinet_api_sockets, 0, sizeof(g_libuinet_api_sockets));
    g_libuinet_api_next_handle = 0;
    g_libuinet_tx_frames = 0;
    g_libuinet_arp_rx = 0;
    g_libuinet_arp_tx = 0;
    g_libuinet_ipv4_rx = 0;
    g_libuinet_ipv4_tx = 0;
    g_libuinet_icmp_rx = 0;
    g_libuinet_icmp_tx = 0;
    g_libuinet_arp_reply_tx = 0;
    g_libuinet_icmp_echo_reply_tx = 0;
    g_libuinet_control_replies = 0;
    g_libuinet_udp_echo_rx = 0;
    g_libuinet_udp_echo_tx = 0;
    g_libuinet_tcp_echo_accepts = 0;
    g_libuinet_tcp_echo_rx = 0;
    g_libuinet_tcp_echo_tx = 0;
    g_libuinet_tcp_echo_closes = 0;
    g_libuinet_dns_queries = 0;
    g_libuinet_dns_answers = 0;
    g_libuinet_http_requests = 0;
    g_libuinet_http_responses = 0;
    g_libuinet_https_requests = 0;
    g_libuinet_https_responses = 0;
    g_libuinet_tls_send_calls = 0;
    g_libuinet_tls_send_bytes = 0;
    g_libuinet_tls_recv_calls = 0;
    g_libuinet_tls_recv_bytes = 0;
    g_libuinet_tls_recv_want = 0;
    memset(&g_libuinet_http_smoke, 0, sizeof(g_libuinet_http_smoke));

    struct uinet_global_cfg global_cfg;
    struct uinet_instance_cfg instance_cfg;
    memset(&global_cfg, 0, sizeof(global_cfg));
    memset(&instance_cfg, 0, sizeof(instance_cfg));

    uinet_default_cfg(&global_cfg, UINET_GLOBAL_CFG_SMALL);
    uinet_instance_default_cfg(&instance_cfg);

    uint64_t stage_start_cycles = netd_metrics_read_tsc();
    int status = uinet_init(&global_cfg, &instance_cfg);
    netd_metrics_record("libuinet_init", stage_start_cycles, netd_metrics_read_tsc());
    if (status != 0) {
        fprintf(stderr, "[netd] libuinet init failed status=%d\n", status);
        g_libuinet_state = NETD_LIBUINET_ERROR;
        return 7;
    }

    status = uinet_initialize_thread("netd");
    if (status != 0) {
        fprintf(stderr, "[netd] libuinet initialize thread failed status=%d\n", status);
        g_libuinet_state = NETD_LIBUINET_ERROR;
        return 7;
    }

    struct uinet_if_cfg if_cfg;
    memset(&if_cfg, 0, sizeof(if_cfg));
    uinet_if_default_config(UINET_IFTYPE_PACHAOS, &if_cfg);
    if_cfg.configstr = "netd";
    if_cfg.alias = "net0";
    if_cfg.rx_batch_size = 64;
    if_cfg.tx_inject_queue_len = 256;

    status = uinet_ifcreate(uinet_instance_default(), &if_cfg, &g_libuinet_if);
    if (status != 0) {
        fprintf(stderr, "[netd] libuinet pachaos ifcreate failed status=%d\n", status);
        g_libuinet_state = NETD_LIBUINET_ERROR;
        return 7;
    }

    status = uinet_pachaos_if_register_tx(g_libuinet_if, netd_libuinet_tx, NULL);
    if (status != 0) {
        fprintf(stderr, "[netd] libuinet pachaos tx register failed status=%d\n", status);
        g_libuinet_state = NETD_LIBUINET_ERROR;
        return 7;
    }

    status = netd_libuinet_configure_ipv4();
    if (status != 0) {
        g_libuinet_state = NETD_LIBUINET_ERROR;
        return status;
    }

    status = netd_libuinet_configure_default_route();
    if (status != 0) {
        g_libuinet_state = NETD_LIBUINET_ERROR;
        return status;
    }

    status = netd_libuinet_start_udp_echo();
    if (status != 0) {
        g_libuinet_state = NETD_LIBUINET_ERROR;
        return status;
    }

    status = netd_libuinet_start_tcp_echo();
    if (status != 0) {
        g_libuinet_state = NETD_LIBUINET_ERROR;
        return status;
    }

    g_libuinet_state = NETD_LIBUINET_READY;

    printf("[netd] libuinet ready\n");
#else
    if (g_libuinet_trace) {
        printf("[netd] libuinet not linked; upper sink active\n");
    }
#endif
    return 0;
}

int netd_libuinet_receive_frame(const struct netd_upper_frame *frame)
{
    if (frame == NULL || frame->bytes == NULL || frame->len == 0) {
        return -22;
    }

    if (g_libuinet_state != NETD_LIBUINET_READY) {
        g_libuinet_rx_frames++;
        return 0;
    }

#if defined(NETD_WITH_LIBUINET)
    netd_libuinet_observe_rx_frame(frame->bytes, frame->len);
    netd_libuinet_control_receive_frame(frame->bytes, frame->len);
    int status = uinet_pachaos_if_deliver(g_libuinet_if, frame->bytes, frame->len);
    if (status == 0) {
        g_libuinet_rx_frames++;
    } else {
        g_libuinet_rx_drops++;
    }
    return status == 0 ? 0 : -status;
#else
    g_libuinet_rx_frames++;
    return 0;
#endif
}

void netd_libuinet_poll(void)
{
#if defined(NETD_WITH_LIBUINET)
    if (g_libuinet_state == NETD_LIBUINET_READY) {
        uinet_instance_sts_events_process(uinet_instance_default());
        netd_libuinet_poll_udp_echo();
        netd_libuinet_poll_tcp_echo();
    }
#endif
}

int netd_libuinet_needs_periodic_poll(void)
{
#if defined(NETD_WITH_LIBUINET)
    if (g_libuinet_http_smoke.state != NETD_HTTP_SMOKE_IDLE &&
        g_libuinet_http_smoke.state != NETD_HTTP_SMOKE_DONE &&
        g_libuinet_http_smoke.state != NETD_HTTP_SMOKE_ERROR)
        return 1;
    for (unsigned i = 0; i < NETD_TCP_ECHO_MAX_CONNECTIONS; i++)
        if (g_libuinet_tcp_connections[i] != NULL) return 1;
    for (unsigned i = 0; i < NETD_SOCKET_API_MAX_SOCKETS; i++)
        if (g_libuinet_api_sockets[i].handle != 0) return 1;
#endif
    return 0;
}

enum netd_libuinet_state netd_libuinet_state(void)
{
    return g_libuinet_state;
}

uint64_t netd_libuinet_rx_frames(void)
{
    return g_libuinet_rx_frames;
}

uint64_t netd_libuinet_rx_drops(void)
{
    return g_libuinet_rx_drops;
}

int netd_libuinet_socket_open(
    uint64_t domain,
    uint64_t type,
    uint64_t protocol,
    int notify_fd,
    uint64_t *out_handle)
{
    if (out_handle == NULL || notify_fd < 16) {
        return -22;
    }
    *out_handle = 0;
#if defined(NETD_WITH_LIBUINET)
    if (g_libuinet_state != NETD_LIBUINET_READY || domain != NETD_AF_INET) {
        return -95;
    }
    int uinet_type = 0;
    int uinet_protocol = 0;
    if (type == NETD_SOCK_DGRAM) {
        uinet_type = UINET_SOCK_DGRAM;
        uinet_protocol = protocol == 0 ? UINET_IPPROTO_UDP : (int)protocol;
        if (uinet_protocol != UINET_IPPROTO_UDP) {
            return -93;
        }
    } else if (type == NETD_SOCK_STREAM) {
        uinet_type = UINET_SOCK_STREAM;
        uinet_protocol = protocol == 0 ? UINET_IPPROTO_TCP : (int)protocol;
        if (uinet_protocol != UINET_IPPROTO_TCP) {
            return -93;
        }
    } else {
        return -94;
    }

    struct netd_libuinet_api_socket *slot = NULL;
    for (unsigned i = 0; i < NETD_SOCKET_API_MAX_SOCKETS; i++) {
        if (g_libuinet_api_sockets[i].socket == NULL) {
            slot = &g_libuinet_api_sockets[i];
            break;
        }
    }
    if (slot == NULL) {
        return -24;
    }

    struct uinet_socket *socket = NULL;
    int status = uinet_socreate(uinet_instance_default(),
                                UINET_PF_INET,
                                &socket,
                                uinet_type,
                                uinet_protocol);
    if (status != 0 || socket == NULL) {
        return status == 0 ? -5 : -status;
    }
    uinet_sosetnonblocking(socket, 1);
    if (type == NETD_SOCK_STREAM) {
        int optval = 1;
        (void)uinet_sosetsockopt(socket, UINET_IPPROTO_TCP, UINET_TCP_NODELAY, &optval, sizeof(optval));
    }

    uint64_t handle = ++g_libuinet_api_next_handle;
    if (handle == 0) {
        handle = ++g_libuinet_api_next_handle;
    }
    slot->handle = handle;
    slot->refcount = 1;
    slot->type = type;
    slot->protocol = (uint64_t)uinet_protocol;
    slot->notify_fd = notify_fd;
    slot->socket = socket;
    *out_handle = handle;
    return 0;
#else
    (void)domain;
    (void)type;
    (void)protocol;
    (void)notify_fd;
    return -95;
#endif
}

int netd_libuinet_socket_collect_wait_sources(struct pacha_service_wait_set *wait_set)
{
#if defined(NETD_WITH_LIBUINET)
    if (wait_set == NULL) return -22;
    for (unsigned i = 0; i < NETD_SOCKET_API_MAX_SOCKETS; ++i) {
        const struct netd_libuinet_api_socket *slot = &g_libuinet_api_sockets[i];
        if (slot->socket == NULL || slot->notify_fd < 16) continue;
        if (pacha_service_wait_add(
                wait_set, slot->notify_fd, PACHA_FD_EVENT_HANGUP) != 0)
            return -24;
    }
#else
    (void)wait_set;
#endif
    return 0;
}

void netd_libuinet_socket_reap_hangups(void)
{
#if defined(NETD_WITH_LIBUINET)
    for (unsigned i = 0; i < NETD_SOCKET_API_MAX_SOCKETS; ++i) {
        struct netd_libuinet_api_socket *slot = &g_libuinet_api_sockets[i];
        if (slot->socket == NULL || slot->notify_fd < 16) continue;
        struct pacha_pollfd pollfd = {
            .fd = slot->notify_fd,
            .events = PACHA_FD_EVENT_HANGUP,
        };
        if (pacha_fd_poll(&pollfd, 1) <= 0 ||
            (pollfd.revents & PACHA_FD_EVENT_HANGUP) == 0) continue;
        const uint64_t handle = slot->handle;
        if (slot->type == NETD_SOCK_STREAM) {
            (void)uinet_soshutdown(slot->socket, UINET_SHUT_RDWR);
        }
        (void)uinet_soclose(slot->socket);
        (void)pacha_fd_close(slot->notify_fd);
        memset(slot, 0, sizeof(*slot));
        printf("[netd] inet_orphan_reap handle=%llu\n",
            (unsigned long long)handle);
    }
#endif
}

#if defined(NETD_WITH_LIBUINET)
static struct netd_libuinet_api_socket *netd_libuinet_api_socket_find(uint64_t handle)
{
    if (handle == 0) {
        return NULL;
    }
    for (unsigned i = 0; i < NETD_SOCKET_API_MAX_SOCKETS; i++) {
        if (g_libuinet_api_sockets[i].socket != NULL &&
            g_libuinet_api_sockets[i].handle == handle) {
            return &g_libuinet_api_sockets[i];
        }
    }
    return NULL;
}
#endif

int netd_libuinet_socket_dup(uint64_t handle)
{
#if defined(NETD_WITH_LIBUINET)
    struct netd_libuinet_api_socket *slot = netd_libuinet_api_socket_find(handle);
    if (slot == NULL) {
        return -9;
    }
    if (slot->refcount == UINT64_MAX) {
        return -24;
    }
    slot->refcount++;
    return 0;
#else
    (void)handle;
    return -95;
#endif
}

int netd_libuinet_socket_connect(uint64_t handle, uint32_t addr_be, uint16_t port_be, uint64_t flags)
{
    (void)flags;
#if defined(NETD_WITH_LIBUINET)
    struct netd_libuinet_api_socket *slot = netd_libuinet_api_socket_find(handle);
    if (slot == NULL) {
        return -9;
    }
    if (!netd_libuinet_socket_connect_target_supported(addr_be)) {
        const uint8_t *addr = (const uint8_t *)&addr_be;
        printf("[netd] socket connect rejected addr=%u.%u.%u.%u errno=ENETUNREACH\n",
               addr[0],
               addr[1],
               addr[2],
               addr[3]);
        fflush(stdout);
        return -101;
    }
    if (g_libuinet_trace) {
        const uint8_t *addr = (const uint8_t *)&addr_be;
        const uint8_t *port = (const uint8_t *)&port_be;
        printf("[netd] socket connect begin handle=%llu addr=%u.%u.%u.%u port=%u\n",
               (unsigned long long)handle,
               addr[0],
               addr[1],
               addr[2],
               addr[3],
               ((unsigned)port[0] << 8) | port[1]);
        fflush(stdout);
    }
    struct uinet_sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_len = sizeof(sin);
    sin.sin_family = UINET_AF_INET;
    sin.sin_port = port_be;
    memcpy(&sin.sin_addr.s_addr, &addr_be, sizeof(addr_be));

    int status = uinet_soconnect(slot->socket, (struct uinet_sockaddr *)&sin);
    if (status == 0 || status == UINET_EISCONN) {
        return 0;
    }
    return -netd_libuinet_errno_to_linux(status);
#else
    (void)handle;
    (void)addr_be;
    (void)port_be;
    return -95;
#endif
}

int netd_libuinet_socket_send(uint64_t handle, const void *data, size_t len, uint64_t flags, uint32_t addr_be, uint16_t port_be, size_t *out_sent)
{
    (void)flags;
    if (data == NULL || len == 0 || out_sent == NULL) {
        return -22;
    }
    *out_sent = 0;
#if defined(NETD_WITH_LIBUINET)
    struct netd_libuinet_api_socket *slot = netd_libuinet_api_socket_find(handle);
    if (slot == NULL) {
        return -9;
    }
    struct uinet_sockaddr_in sin;
    struct uinet_sockaddr *send_addr = NULL;
    if (slot->type == NETD_SOCK_DGRAM && (addr_be != 0 || port_be != 0)) {
        memset(&sin, 0, sizeof(sin));
        sin.sin_len = sizeof(sin);
        sin.sin_family = UINET_AF_INET;
        sin.sin_port = port_be;
        memcpy(&sin.sin_addr.s_addr, &addr_be, sizeof(addr_be));
        send_addr = (struct uinet_sockaddr *)&sin;
    }
    struct uinet_iovec iov;
    struct uinet_uio uio;
    iov.iov_base = (void *)data;
    iov.iov_len = len;
    uio.uio_iov = &iov;
    uio.uio_iovcnt = 1;
    uio.uio_offset = 0;
    uio.uio_resid = (int64_t)len;

    int status = uinet_sosend(slot->socket, send_addr, &uio, 0);
    if (status == UINET_EWOULDBLOCK || status == UINET_EAGAIN) {
        return -11;
    }
    if (status != 0) {
        return -netd_libuinet_errno_to_linux(status);
    }
    *out_sent = len - (size_t)uio.uio_resid;
    slot->send_calls++;
    slot->send_bytes += (uint64_t)*out_sent;
    if (g_libuinet_trace && slot->type == NETD_SOCK_STREAM && *out_sent != 0 && slot->send_calls <= 8u) {
        slot->send_preview_logged = 1;
        netd_libuinet_print_ascii_preview("[netd] socket stream send", slot->handle, data, *out_sent);
    }
    netd_libuinet_api_pump(4);
    return *out_sent == 0 ? -11 : 0;
#else
    (void)handle;
    (void)addr_be;
    (void)port_be;
    return -95;
#endif
}

int netd_libuinet_socket_recv(uint64_t handle, void *data, size_t capacity, uint64_t flags, size_t *out_received)
{
    (void)flags;
    if (data == NULL || capacity == 0 || out_received == NULL) {
        return -22;
    }
    *out_received = 0;
#if defined(NETD_WITH_LIBUINET)
    struct netd_libuinet_api_socket *slot = netd_libuinet_api_socket_find(handle);
    if (slot == NULL) {
        return -9;
    }
    int readable = 0;
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        readable = uinet_soreadable(slot->socket, 0);
        if (readable != 0) {
            break;
        }
        netd_libuinet_api_pump(1);
    }
    if (readable == 0) {
        if (g_libuinet_trace && slot->type == NETD_SOCK_STREAM && slot->recv_bytes != 0 && !slot->recv_idle_logged) {
            slot->recv_idle_logged = 1;
            printf("[netd] socket recv idle handle=%llu calls=%llu bytes=%llu state=0x%x error=%d\n",
                   (unsigned long long)slot->handle,
                   (unsigned long long)slot->recv_calls,
                   (unsigned long long)slot->recv_bytes,
                   uinet_sogetstate(slot->socket),
                   uinet_sogeterror(slot->socket));
            fflush(stdout);
        }
        return -11;
    }
    if (readable < 0) {
        const int error = uinet_sogeterror(slot->socket);
        if (error != 0 && error != UINET_EINPROGRESS && error != UINET_EALREADY) {
            return -netd_libuinet_errno_to_linux(error);
        }
        *out_received = 0;
        if (g_libuinet_trace && slot->type == NETD_SOCK_STREAM) {
            printf("[netd] socket recv eof handle=%llu calls=%llu bytes=%llu state=0x%x error=%d\n",
                   (unsigned long long)slot->handle,
                   (unsigned long long)slot->recv_calls,
                   (unsigned long long)slot->recv_bytes,
                   uinet_sogetstate(slot->socket),
                   error);
            fflush(stdout);
        }
        return 0;
    }

    size_t read_size = (size_t)readable;
    if (read_size > capacity) {
        read_size = capacity;
    }
    struct uinet_iovec iov;
    struct uinet_uio uio;
    iov.iov_base = data;
    iov.iov_len = read_size;
    uio.uio_iov = &iov;
    uio.uio_iovcnt = 1;
    uio.uio_offset = 0;
    uio.uio_resid = (int64_t)read_size;

    int status = uinet_soreceive(slot->socket, NULL, &uio, NULL);
    if (status == UINET_EWOULDBLOCK || status == UINET_EAGAIN) {
        return -11;
    }
    if (status != 0) {
        return -netd_libuinet_errno_to_linux(status);
    }
    *out_received = read_size - (size_t)uio.uio_resid;
    slot->recv_calls++;
    slot->recv_bytes += (uint64_t)*out_received;
    slot->recv_idle_logged = 0;
    if (g_libuinet_trace && slot->type == NETD_SOCK_STREAM && *out_received != 0 && slot->recv_calls <= 2u) {
        slot->recv_preview_logged = 1;
        netd_libuinet_print_ascii_preview("[netd] socket stream recv", slot->handle, data, *out_received);
    }
    return *out_received == 0 ? -11 : 0;
#else
    (void)handle;
    return -95;
#endif
}

int netd_libuinet_socket_poll(uint64_t handle, uint32_t events, uint32_t *out_revents, int32_t *out_error)
{
    if (out_revents == NULL || out_error == NULL) {
        return -22;
    }
    *out_revents = 0;
    *out_error = 0;
#if defined(NETD_WITH_LIBUINET)
    struct netd_libuinet_api_socket *slot = netd_libuinet_api_socket_find(handle);
    if (slot == NULL) {
        return -9;
    }
    slot->poll_calls++;
    netd_libuinet_api_pump(4);
    const int error = uinet_sogeterror(slot->socket);
    const int state = uinet_sogetstate(slot->socket);
    int readable_snapshot = -2;
    int writable_snapshot = -2;
    if (error != 0 && error != UINET_EINPROGRESS && error != UINET_EALREADY) {
        *out_error = netd_libuinet_errno_to_linux(error);
        *out_revents |= NETD_POLLERR;
    }
    if ((events & NETD_POLLIN) != 0) {
        const int readable = uinet_soreadable(slot->socket, 0);
        readable_snapshot = readable;
        if (readable > 0) {
            *out_revents |= NETD_POLLIN;
            slot->recv_idle_logged = 0;
        } else if (readable < 0) {
            const int read_error = uinet_sogeterror(slot->socket);
            if (read_error != 0 && read_error != UINET_EINPROGRESS && read_error != UINET_EALREADY) {
                *out_error = netd_libuinet_errno_to_linux(read_error);
                *out_revents |= NETD_POLLERR;
            } else {
                *out_revents |= NETD_POLLIN;
            }
        } else if (g_libuinet_trace && slot->type == NETD_SOCK_STREAM && slot->recv_bytes != 0 && !slot->recv_idle_logged) {
            slot->recv_idle_logged = 1;
            printf("[netd] socket poll idle handle=%llu calls=%llu bytes=%llu state=0x%x error=%d events=0x%x\n",
                   (unsigned long long)slot->handle,
                   (unsigned long long)slot->recv_calls,
                   (unsigned long long)slot->recv_bytes,
                   uinet_sogetstate(slot->socket),
                   uinet_sogeterror(slot->socket),
                   events);
            fflush(stdout);
        }
    }
    if ((events & NETD_POLLOUT) != 0) {
        const int writable = uinet_sowritable(slot->socket, 0);
        writable_snapshot = writable;
        if (writable > 0 || ((state & UINET_SS_ISCONNECTED) != 0 && error == 0)) {
            *out_revents |= NETD_POLLOUT;
        } else if (writable < 0) {
            *out_revents |= NETD_POLLERR;
        }
    }
    if (slot->type == NETD_SOCK_STREAM) {
        if (*out_revents != 0) {
            if ((g_libuinet_trace || (*out_revents & NETD_POLLERR) != 0) &&
                (!slot->poll_ready_logged || (*out_revents & NETD_POLLERR) != 0)) {
                slot->poll_ready_logged = 1;
                printf("[netd] socket poll ready handle=%llu polls=%llu events=0x%x revents=0x%x state=0x%x error=%d readable=%d writable=%d bytes=%llu\n",
                       (unsigned long long)slot->handle,
                       (unsigned long long)slot->poll_calls,
                       events,
                       *out_revents,
                       state,
                       error,
                       readable_snapshot,
                       writable_snapshot,
                       (unsigned long long)slot->recv_bytes);
                fflush(stdout);
            }
        } else if (g_libuinet_trace && netd_libuinet_trace_poll_sample(slot->poll_calls)) {
            printf("[netd] socket poll wait handle=%llu polls=%llu events=0x%x state=0x%x error=%d readable=%d writable=%d bytes=%llu\n",
                   (unsigned long long)slot->handle,
                   (unsigned long long)slot->poll_calls,
                   events,
                   state,
                   error,
                   readable_snapshot,
                   writable_snapshot,
                   (unsigned long long)slot->recv_bytes);
            fflush(stdout);
        }
    }
    return 0;
#else
    (void)handle;
    (void)events;
    (void)out_error;
    return -95;
#endif
}

int netd_libuinet_socket_close(uint64_t handle)
{
#if defined(NETD_WITH_LIBUINET)
    struct netd_libuinet_api_socket *slot = netd_libuinet_api_socket_find(handle);
    if (slot == NULL) {
        return -9;
    }
    if (slot->refcount > 1) {
        slot->refcount--;
        return 0;
    }
    if (slot->type == NETD_SOCK_STREAM) {
        const int close_error = uinet_sogeterror(slot->socket);
        if (g_libuinet_trace || close_error != 0) {
            printf("[netd] socket close handle=%llu calls=%llu bytes=%llu state=0x%x error=%d\n",
                   (unsigned long long)slot->handle,
                   (unsigned long long)slot->recv_calls,
                   (unsigned long long)slot->recv_bytes,
                   uinet_sogetstate(slot->socket),
                   close_error);
            fflush(stdout);
        }
        (void)uinet_soshutdown(slot->socket, UINET_SHUT_RDWR);
    }
    (void)uinet_soclose(slot->socket);
    if (slot->notify_fd >= 16) (void)pacha_fd_close(slot->notify_fd);
    memset(slot, 0, sizeof(*slot));
    return 0;
#else
    (void)handle;
    return -95;
#endif
}

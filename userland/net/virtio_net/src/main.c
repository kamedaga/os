typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

enum {
    SYSCALL_MAP_PAGE = 0x2,
    SYSCALL_LOG = 0x9,
    SYSCALL_ALLOC_MAP_PAGES = 0xC,
    SYSCALL_QUEUE_SUBMIT = 0xE,
    SYSCALL_QUEUE_NOTIFY = 0xF,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_ACCEPT_CAP_TRANSFER = 0x2A,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_IOMMU_AUTHORIZE = 0x35,
    SYSCALL_DMA_MAP_CREATE = 0x37,
    SYSCALL_DMA_MAP_SET_STATE = 0x38,
    SYSCALL_MAP_PAGE_ANYWHERE = 0x5C,
    SYSCALL_ACCEPT_IPC_BUFFER_TRANSFER = 0x61,
    SYSCALL_MAP_IPC_BUFFER_ANYWHERE = 0x62,

    SYSCALL_OK = 0,

    CONFIG_VA = 0x3C002000,
    VIRTIO_MMIO_BASE_VA = 0x30200000,
    RX_QUEUE_PAGE_VA = 0x30210000,
    TX_QUEUE_PAGE_VA = 0x30211000,
    RX_BUFFER_BASE_VA = 0x30220000,
    TX_BUFFER_VA = 0x30240000,

    NET_CONFIG_MAGIC = 0x4E455443,
    NET_CONFIG_VERSION = 1,
    NET_ENDPOINT_ID_INDEX = 2,
    NET_COMMON_PAGE_PADDR_INDEX = 3,
    NET_NOTIFY_PAGE_PADDR_INDEX = 4,
    NET_ISR_PAGE_PADDR_INDEX = 5,
    NET_DEVICE_PAGE_PADDR_INDEX = 6,
    NET_COMMON_PAGE_OFFSET_INDEX = 7,
    NET_NOTIFY_PAGE_OFFSET_INDEX = 8,
    NET_ISR_PAGE_OFFSET_INDEX = 9,
    NET_DEVICE_PAGE_OFFSET_INDEX = 10,
    NET_NOTIFY_OFF_MULTIPLIER_INDEX = 11,
    NET_IOMMU_TOKEN_INDEX = 12,
    NET_RX_QUEUE_SUBMIT_TOKEN_INDEX = 13,
    NET_RX_QUEUE_NOTIFY_TOKEN_INDEX = 14,
    NET_TX_QUEUE_SUBMIT_TOKEN_INDEX = 15,
    NET_TX_QUEUE_NOTIFY_TOKEN_INDEX = 16,
    NET_COMMAND_TOKEN_INDEX = 17,
    NET_RESOURCE_ID_INDEX = 18,
    NET_DRIVER_STATUS_INDEX = 19,
    NET_STATUS_READY = 0x4E524459,
    NET_STATUS_FAILED = 0x4E464149,

    COMMON_DEVICE_FEATURE_SELECT = 0x00,
    COMMON_DEVICE_FEATURE = 0x04,
    COMMON_DRIVER_FEATURE_SELECT = 0x08,
    COMMON_DRIVER_FEATURE = 0x0C,
    COMMON_DEVICE_STATUS = 0x14,
    COMMON_QUEUE_SELECT = 0x16,
    COMMON_QUEUE_SIZE = 0x18,
    COMMON_QUEUE_ENABLE = 0x1C,
    COMMON_QUEUE_NOTIFY_OFF = 0x1E,
    COMMON_QUEUE_DESC = 0x20,
    COMMON_QUEUE_AVAIL = 0x28,
    COMMON_QUEUE_USED = 0x30,

    STATUS_ACKNOWLEDGE = 0x01,
    STATUS_DRIVER = 0x02,
    STATUS_DRIVER_OK = 0x04,
    STATUS_FEATURES_OK = 0x08,
    STATUS_FAILED = 0x80,

    VIRTIO_NET_F_MAC = 5,
    VIRTIO_F_VERSION_1 = 32,

    QUEUE_SIZE = 16,
    QUEUE_USED_OFFSET = 2048,
    RX_QUEUE_INDEX = 0,
    TX_QUEUE_INDEX = 1,
    RX_BUFFER_COUNT = 8,
    NET_HDR_BYTES = 12,
    ETH_HDR_BYTES = 14,
    RX_BUFFER_BYTES = 2048,
    TX_BUFFER_BYTES = 2048,

    ETHERTYPE_IPV4 = 0x0800,
    ETHERTYPE_ARP = 0x0806,
    IP_PROTO_TCP = 6,
    IP_PROTO_UDP = 17,
    DHCP_CLIENT_PORT = 68,
    DHCP_SERVER_PORT = 67,
    DHCP_MAGIC_COOKIE = 0x63825363,
    DHCP_MSG_DISCOVER = 1,
    DHCP_MSG_OFFER = 2,
    DHCP_MSG_REQUEST = 3,
    DHCP_MSG_ACK = 5,

    NET_PROTOCOL_REQUEST_MAGIC = 0x514E4554,
    NET_PROTOCOL_RESPONSE_MAGIC = 0x524E4554,
    NET_PROTOCOL_VERSION = 1,
    NET_OP_CONNECT = 1,
    NET_OP_GET_STATUS = 2,
    NET_OP_BIND = 3,
    NET_OP_SEND_TO = 4,
    NET_OP_RECV_FROM = 5,
    NET_OP_CLOSE = 6,
    NET_OP_POLL = 7,
    NET_OP_TCP_CONNECT = 8,
    NET_OP_TCP_WRITE = 9,
    NET_OP_TCP_READ = 10,
    NET_STATUS_OK = 0,
    NET_STATUS_INVALID = 2,
    NET_STATUS_NOT_CONNECTED = 4,
    NET_STATUS_NO_ROUTE = 5,
    NET_STATUS_PORT_IN_USE = 6,
    NET_STATUS_WOULD_BLOCK = 7,
    NET_STATUS_TOO_BIG = 8,
    NET_STATUS_BUSY = 9,
    NET_REPLY_ENDPOINT_ID = 0xEA,
    CAP_TRANSFER_ID_MIN = 0x1000,
    IPC_BUFFER_TOKEN_TAG = 0xA000000000000000ULL,
    IPC_BUFFER_TOKEN_MASK = 0x0FFFFFFFFFFFFFFFULL,
    NET_FLAG_LINK_UP = 1 << 0,
    NET_FLAG_DHCP_BOUND = 1 << 1,
    NET_FLAG_GATEWAY_ARP = 1 << 2,
    NET_POLL_READABLE = 1 << 0,
    NET_POLL_WRITABLE = 1 << 2,
    NET_REQUEST_HEADER_BYTES = 56,
    NET_RESPONSE_HEADER_BYTES = 56,
    NET_PAYLOAD_BYTES = 4096 - NET_REQUEST_HEADER_BYTES,
    NET_UDP_MAX_PAYLOAD = 1200,
    NET_UDP_BINDINGS = 8,
    NET_UDP_PENDING = 8,
    NET_TCP_CONNECTIONS = 4,
    NET_TCP_MAX_PAYLOAD = 1200,
    NET_TCP_MSS = 1200,
    NET_TCP_OOO_SEGMENTS = 8,
    NET_TCP_OOO_BYTES = 1600,
    NET_TCP_RX_BYTES = 64 * 1024,
    NET_TCP_WINDOW_MAX = 8 * 1024,
    NET_SESSION_MAX = 4,
    NET_UDP_HANDLE_TAG = 0x5544500000000000ULL,
    NET_TCP_HANDLE_TAG = 0x5443500000000000ULL,
    NET_EPHEMERAL_PORT_BASE = 49152,
    TCP_STATE_SYN_SENT = 1,
    TCP_STATE_ESTABLISHED = 2,
    TCP_STATE_FIN_WAIT = 3,
    TCP_STATE_CLOSED = 4,
    TCP_FLAG_FIN = 0x01,
    TCP_FLAG_SYN = 0x02,
    TCP_FLAG_RST = 0x04,
    TCP_FLAG_PSH = 0x08,
    TCP_FLAG_ACK = 0x10,

    DESC_FLAG_NEXT = 1 << 0,
    DESC_FLAG_WRITE = 1 << 1,

    DMA_DIRECTION_READ = 0,
    DMA_DIRECTION_WRITE = 1,
    DMA_STATE_IN_FLIGHT = 1,
    IOMMU_OP_MAP_READ = 0,
    IOMMU_OP_MAP_WRITE = 1,

    DMA_MAPPING_TOKEN_TAG = (1ULL << 63) | (1ULL << 61),
};

struct virtq_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
};

struct virtq_used_elem {
    u32 id;
    u32 len;
};

struct queue_state {
    u16 index;
    u64 submit_token;
    u64 notify_token;
    u64 page_va;
    u64 page_paddr;
    u64 notify_addr;
    u16 last_used_idx;
};

struct boot_state {
    u64 endpoint_id;
    u64 resource_id;
    u64 common_page_paddr;
    u64 notify_page_paddr;
    u64 isr_page_paddr;
    u64 device_page_paddr;
    u64 common_page_offset;
    u64 notify_page_offset;
    u64 isr_page_offset;
    u64 device_page_offset;
    u64 notify_off_multiplier;
    u64 iommu_token;
    u64 command_token;
};

struct net_request_header {
    u32 magic;
    u16 version;
    u16 op;
    u64 request_seq;
    u64 session_nonce;
    u64 arg0;
    u64 arg1;
    u64 arg2;
    u64 reserved0;
};

struct net_response_header {
    u32 magic;
    u16 version;
    u16 op;
    u64 response_seq;
    int status;
    u32 inline_bytes;
    u64 arg0;
    u64 arg1;
    u64 arg2;
    u64 reserved0;
};

struct net_status_payload {
    u8 mac[6];
    u8 link_up;
    u8 dhcp_bound;
    u32 ipv4_addr;
    u32 gateway_addr;
    u32 dns_addr;
    u32 dhcp_server_addr;
    u32 flags;
    u64 rx_packets;
    u64 tx_completions;
    u64 tcp_rx_segments;
    u64 tcp_rx_payload_bytes;
    u64 tcp_rx_in_order_bytes;
    u64 tcp_rx_duplicate_segments;
    u64 tcp_rx_out_of_order_segments;
    u64 tcp_rx_ooo_stored;
    u64 tcp_rx_ooo_drained;
    u64 tcp_rx_ooo_dropped;
    u64 tcp_rx_append_failed;
    u64 tcp_ack_sent;
    u64 tcp_ack_deferred;
    u64 tcp_ack_flushed;
    u64 tcp_tx_busy;
    u64 tcp_connect_requests;
    u64 tcp_connect_established;
    u64 tcp_tx_segments;
    u64 tcp_tx_payload_bytes;
    u64 tcp_read_requests;
    u64 tcp_read_would_block;
    u64 tcp_read_bytes;
    u64 tcp_poll_requests;
    u64 tcp_poll_readable;
    u64 tcp_rx_syn_ack;
    u64 tcp_rx_fin;
    u64 tcp_rx_rst;
    u64 tcp_active_connections;
    u64 tcp_established_connections;
    u64 tcp_rx_buffered_bytes;
    u64 tcp_rx_buffer_max_bytes;
    u64 tcp_ack_pending_connections;
    u64 net_service_requests;
    u64 net_service_work_loops;
    u64 net_service_idle_sleeps;
};

struct net_session {
    int active;
    u64 request_paddr;
    u64 response_paddr;
    u64 request_token;
    u64 response_token;
    u64 request_va;
    u64 response_va;
    u64 reply_endpoint_id;
    u64 session_nonce;
    u64 last_completed_seq;
};

struct udp_packet {
    u8 used;
    u8 reserved0[7];
    u32 src_ip;
    u16 src_port;
    u16 dst_port;
    u32 len;
    u8 bytes[NET_UDP_MAX_PAYLOAD];
};

struct udp_binding {
    u8 active;
    u8 reserved0[7];
    u64 handle;
    u16 local_port;
    u16 head;
    u16 count;
    u16 reserved1;
    struct udp_packet packets[NET_UDP_PENDING];
};

struct tcp_ooo_segment {
    u8 used;
    u8 reserved0[3];
    u32 seq;
    u32 len;
    u8 bytes[NET_TCP_OOO_BYTES];
};

struct tcp_connection {
    u8 active;
    u8 state;
    u8 peer_closed;
    u8 ack_pending;
    u64 handle;
    u32 local_ip;
    u32 remote_ip;
    u16 local_port;
    u16 remote_port;
    u32 seq;
    u32 ack;
    u32 initial_seq;
    u32 rx_head;
    u32 rx_len;
    struct tcp_ooo_segment ooo[NET_TCP_OOO_SEGMENTS];
    u8 rx[NET_TCP_RX_BYTES];
};

static struct boot_state g_boot;
static struct net_session g_sessions[NET_SESSION_MAX];
static struct net_session *g_current_session;
static struct udp_binding g_udp_bindings[NET_UDP_BINDINGS];
static struct tcp_connection g_tcp_connections[NET_TCP_CONNECTIONS];
static struct queue_state g_rx_queue = { RX_QUEUE_INDEX, 0, 0, RX_QUEUE_PAGE_VA, 0, 0, 0 };
static struct queue_state g_tx_queue = { TX_QUEUE_INDEX, 0, 0, TX_QUEUE_PAGE_VA, 0, 0, 0 };
static u64 g_common_base;
static u64 g_notify_base;
static u64 g_isr_base;
static u64 g_device_base;
static u64 g_rx_buffer_paddrs[RX_BUFFER_COUNT];
static u64 g_rx_dma_tokens[RX_BUFFER_COUNT];
static u64 g_tx_buffer_paddr;
static u64 g_tx_dma_token;
static u8 g_mac[6];
static u8 g_gateway_mac[6];
static u64 g_rx_packets;
static u32 g_dhcp_xid = 0x50414348;
static u32 g_ipv4_addr;
static u32 g_dhcp_offer_addr;
static u32 g_dhcp_server_addr;
static u32 g_gateway_addr;
static u32 g_dns_addr;
static int g_dhcp_sent;
static int g_dhcp_request_sent;
static int g_dhcp_configured;
static int g_arp_sent;
static int g_arp_reply_seen;
static int g_gateway_mac_ready;
static int g_tx_complete_logged;
static int g_tx_in_flight;
static u64 g_tx_completions;
static u16 g_next_ephemeral_port = NET_EPHEMERAL_PORT_BASE;
static u64 g_tcp_rx_segments;
static u64 g_tcp_rx_payload_bytes;
static u64 g_tcp_rx_in_order_bytes;
static u64 g_tcp_rx_duplicate_segments;
static u64 g_tcp_rx_out_of_order_segments;
static u64 g_tcp_rx_ooo_stored;
static u64 g_tcp_rx_ooo_drained;
static u64 g_tcp_rx_ooo_dropped;
static u64 g_tcp_rx_append_failed;
static u64 g_tcp_ack_sent;
static u64 g_tcp_ack_deferred;
static u64 g_tcp_ack_flushed;
static u64 g_tcp_tx_busy;
static u64 g_tcp_connect_requests;
static u64 g_tcp_connect_established;
static u64 g_tcp_tx_segments;
static u64 g_tcp_tx_payload_bytes;
static u64 g_tcp_read_requests;
static u64 g_tcp_read_would_block;
static u64 g_tcp_read_bytes;
static u64 g_tcp_poll_requests;
static u64 g_tcp_poll_readable;
static u64 g_tcp_rx_syn_ack;
static u64 g_tcp_rx_fin;
static u64 g_tcp_rx_rst;
static u64 g_net_service_requests;
static u64 g_net_service_work_loops;
static u64 g_net_service_idle_sleeps;
static u64 g_tcp_next_progress_log_bytes = 64 * 1024;

void *memset(void *dst, int value, u64 n) {
    u8 *d = (u8 *)dst;
    for (u64 i = 0; i < n; i++) d[i] = (u8)value;
    return dst;
}

void *memcpy(void *dst, const void *src, u64 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static u64 cstr_len(const char *s) {
    u64 n = 0;
    while (s[n] != 0) n++;
    return n;
}

static void user_log_len(const char *message, u64 len) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"((u64)SYSCALL_LOG), "D"((u64)message), "S"(len)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    (void)ret;
}

static void user_log(const char *message) {
    user_log_len(message, cstr_len(message));
}

static u64 syscall2(u64 nr, u64 a0, u64 a1) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall1(u64 nr, u64 a0) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall3(u64 nr, u64 a0, u64 a1, u64 a2) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall4(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "c"(a3)
        : "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 wait_event(u64 wait_mailbox, u64 timeout_ticks) {
    return syscall2(SYSCALL_WAIT_EVENT, wait_mailbox, timeout_ticks);
}

static u64 accept_cap_transfer(u64 transfer_id) {
    return syscall1(SYSCALL_ACCEPT_CAP_TRANSFER, transfer_id);
}

static u64 install_endpoint(u64 endpoint_id, u64 target_process_slot) {
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, endpoint_id, target_process_slot);
}

static u64 signal_endpoint(u64 endpoint_id) {
    return syscall2(SYSCALL_SIGNAL_ENDPOINT, endpoint_id, 0);
}

static u64 alloc_map_pages(u64 base_va, u64 page_count, u64 writable, u64 paddrs_out) {
    return syscall4(SYSCALL_ALLOC_MAP_PAGES, base_va, page_count, writable, paddrs_out);
}

static u64 map_mmio_page(u64 va, u64 paddr, u64 writable) {
    return syscall3(SYSCALL_MAP_PAGE, va, paddr, writable);
}

static u64 map_page_anywhere(u64 paddr, u64 writable) {
    return syscall2(SYSCALL_MAP_PAGE_ANYWHERE, paddr, writable);
}

static int is_ipc_buffer_token(u64 token) {
    return (token & ~IPC_BUFFER_TOKEN_MASK) == IPC_BUFFER_TOKEN_TAG && (token & IPC_BUFFER_TOKEN_MASK) != 0;
}

static u64 accept_ipc_buffer_transfer(u64 transfer_id) {
    return syscall1(SYSCALL_ACCEPT_IPC_BUFFER_TRANSFER, transfer_id);
}

static u64 map_ipc_buffer_anywhere(u64 token, u64 writable) {
    return syscall2(SYSCALL_MAP_IPC_BUFFER_ANYWHERE, token, writable);
}

static u64 queue_submit(u64 token, u64 queue_index) {
    return syscall2(SYSCALL_QUEUE_SUBMIT, token, queue_index);
}

static u64 queue_notify(u64 token, u64 queue_index) {
    return syscall2(SYSCALL_QUEUE_NOTIFY, token, queue_index);
}

static u64 iommu_authorize(u64 token, u64 device, u64 op) {
    return syscall3(SYSCALL_IOMMU_AUTHORIZE, token, device, op);
}

static u64 dma_map_create(u64 device, u64 paddr_start, u64 length, u64 direction) {
    const u64 raw = syscall4(SYSCALL_DMA_MAP_CREATE, device, paddr_start, length, direction);
    if ((raw & DMA_MAPPING_TOKEN_TAG) != DMA_MAPPING_TOKEN_TAG) return 0;
    return raw & ~DMA_MAPPING_TOKEN_TAG;
}

static u64 dma_map_set_state(u64 token, u64 state) {
    return syscall2(SYSCALL_DMA_MAP_SET_STATE, token, state);
}

static u8 mmio_read_u8(u64 addr) {
    volatile u8 *p = (volatile u8 *)addr;
    return *p;
}

static u16 mmio_read_u16(u64 addr) {
    volatile u16 *p = (volatile u16 *)addr;
    return *p;
}

static u32 mmio_read_u32(u64 addr) {
    volatile u32 *p = (volatile u32 *)addr;
    return *p;
}

static void mmio_write_u8(u64 addr, u8 value) {
    volatile u8 *p = (volatile u8 *)addr;
    *p = value;
}

static void mmio_write_u16(u64 addr, u16 value) {
    volatile u16 *p = (volatile u16 *)addr;
    *p = value;
}

static void mmio_write_u32(u64 addr, u32 value) {
    volatile u32 *p = (volatile u32 *)addr;
    *p = value;
}

static void mmio_write_u64(u64 addr, u64 value) {
    volatile u64 *p = (volatile u64 *)addr;
    *p = value;
}

static u64 cfg_read(u64 index) {
    volatile u64 *cfg = (volatile u64 *)CONFIG_VA;
    return cfg[index];
}

static void cfg_write(u64 index, u64 value) {
    volatile u64 *cfg = (volatile u64 *)CONFIG_VA;
    cfg[index] = value;
}

static void append_char(char *buf, u64 cap, u64 *len, char ch) {
    if (*len + 1 >= cap) return;
    buf[*len] = ch;
    *len = *len + 1;
    buf[*len] = 0;
}

static void append_str(char *buf, u64 cap, u64 *len, const char *s) {
    for (u64 i = 0; s[i] != 0; i++) append_char(buf, cap, len, s[i]);
}

static void append_hex_nibble(char *buf, u64 cap, u64 *len, u8 value) {
    append_char(buf, cap, len, value < 10 ? (char)('0' + value) : (char)('a' + value - 10));
}

static void append_hex_byte(char *buf, u64 cap, u64 *len, u8 value) {
    append_hex_nibble(buf, cap, len, (u8)(value >> 4));
    append_hex_nibble(buf, cap, len, (u8)(value & 0x0F));
}

static void append_u64_dec(char *buf, u64 cap, u64 *len, u64 value) {
    char tmp[20];
    u64 n = 0;
    if (value == 0) {
        append_char(buf, cap, len, '0');
        return;
    }
    while (value != 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (n != 0) append_char(buf, cap, len, tmp[--n]);
}

static u16 read_be16(const u8 *p) {
    return (u16)(((u16)p[0] << 8) | p[1]);
}

static u32 read_be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void write_be16(u8 *p, u16 value) {
    p[0] = (u8)(value >> 8);
    p[1] = (u8)value;
}

static void write_be32(u8 *p, u32 value) {
    p[0] = (u8)(value >> 24);
    p[1] = (u8)(value >> 16);
    p[2] = (u8)(value >> 8);
    p[3] = (u8)value;
}

static void append_ipv4(char *buf, u64 cap, u64 *len, u32 addr) {
    append_u64_dec(buf, cap, len, (addr >> 24) & 0xFF);
    append_char(buf, cap, len, '.');
    append_u64_dec(buf, cap, len, (addr >> 16) & 0xFF);
    append_char(buf, cap, len, '.');
    append_u64_dec(buf, cap, len, (addr >> 8) & 0xFF);
    append_char(buf, cap, len, '.');
    append_u64_dec(buf, cap, len, addr & 0xFF);
}

static void tcp_connection_totals(u64 *active, u64 *established, u64 *buffered, u64 *max_buffered, u64 *ack_pending) {
    *active = 0;
    *established = 0;
    *buffered = 0;
    *max_buffered = 0;
    *ack_pending = 0;
    for (u64 i = 0; i < NET_TCP_CONNECTIONS; i++) {
        const struct tcp_connection *conn = &g_tcp_connections[i];
        if (!conn->active) continue;
        *active = *active + 1;
        if (conn->state == TCP_STATE_ESTABLISHED) *established = *established + 1;
        *buffered = *buffered + conn->rx_len;
        if (conn->rx_len > *max_buffered) *max_buffered = conn->rx_len;
        if (conn->ack_pending) *ack_pending = *ack_pending + 1;
    }
}

static void tcp_log_progress_if_needed(void) {
    if (g_tcp_rx_in_order_bytes < g_tcp_next_progress_log_bytes) return;
    while (g_tcp_rx_in_order_bytes >= g_tcp_next_progress_log_bytes) {
        g_tcp_next_progress_log_bytes += 64 * 1024;
    }
    u64 active = 0;
    u64 established = 0;
    u64 buffered = 0;
    u64 max_buffered = 0;
    u64 ack_pending = 0;
    tcp_connection_totals(&active, &established, &buffered, &max_buffered, &ack_pending);
    char line[192];
    u64 len = 0;
    line[0] = 0;
    append_str(line, sizeof(line), &len, "[virtio_net] VirtioNet: tcp progress rx_in=");
    append_u64_dec(line, sizeof(line), &len, g_tcp_rx_in_order_bytes);
    append_str(line, sizeof(line), &len, " read=");
    append_u64_dec(line, sizeof(line), &len, g_tcp_read_bytes);
    append_str(line, sizeof(line), &len, " tx=");
    append_u64_dec(line, sizeof(line), &len, g_tcp_tx_payload_bytes);
    append_str(line, sizeof(line), &len, " queued=");
    append_u64_dec(line, sizeof(line), &len, buffered);
    append_str(line, sizeof(line), &len, " active=");
    append_u64_dec(line, sizeof(line), &len, active);
    append_str(line, sizeof(line), &len, " est=");
    append_u64_dec(line, sizeof(line), &len, established);
    append_str(line, sizeof(line), &len, " ackp=");
    append_u64_dec(line, sizeof(line), &len, ack_pending);
    append_char(line, sizeof(line), &len, '\n');
    user_log_len(line, len);
}

static void tcp_log_rx_full(const struct tcp_connection *conn, u32 append_len) {
    if (g_tcp_rx_append_failed > 8) return;
    char line[192];
    u64 len = 0;
    line[0] = 0;
    append_str(line, sizeof(line), &len, "[virtio_net] VirtioNet: tcp rx full queued=");
    append_u64_dec(line, sizeof(line), &len, conn->rx_len);
    append_str(line, sizeof(line), &len, " append=");
    append_u64_dec(line, sizeof(line), &len, append_len);
    append_str(line, sizeof(line), &len, " ackp=");
    append_u64_dec(line, sizeof(line), &len, conn->ack_pending ? 1 : 0);
    append_str(line, sizeof(line), &len, " failures=");
    append_u64_dec(line, sizeof(line), &len, g_tcp_rx_append_failed);
    append_char(line, sizeof(line), &len, '\n');
    user_log_len(line, len);
}

static u16 ipv4_checksum(const u8 *header, u32 header_len) {
    u32 sum = 0;
    for (u32 i = 0; i + 1 < header_len; i += 2) {
        sum += read_be16(header + i);
        while ((sum >> 16) != 0) sum = (sum & 0xFFFF) + (sum >> 16);
    }
    if ((header_len & 1) != 0) {
        sum += (u32)header[header_len - 1] << 8;
        while ((sum >> 16) != 0) sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (u16)~sum;
}

static u16 udp_ipv4_checksum(u32 src_ip, u32 dst_ip, const u8 *udp, u32 udp_len) {
    u32 sum = 0;
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += IP_PROTO_UDP;
    sum += udp_len;
    for (u32 i = 0; i + 1 < udp_len; i += 2) {
        sum += read_be16(udp + i);
        while ((sum >> 16) != 0) sum = (sum & 0xFFFF) + (sum >> 16);
    }
    if ((udp_len & 1) != 0) {
        sum += (u32)udp[udp_len - 1] << 8;
        while ((sum >> 16) != 0) sum = (sum & 0xFFFF) + (sum >> 16);
    }
    while ((sum >> 16) != 0) sum = (sum & 0xFFFF) + (sum >> 16);
    const u16 checksum = (u16)~sum;
    return checksum == 0 ? 0xFFFF : checksum;
}

static u16 tcp_ipv4_checksum(u32 src_ip, u32 dst_ip, const u8 *tcp, u32 tcp_len) {
    u32 sum = 0;
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += IP_PROTO_TCP;
    sum += tcp_len;
    for (u32 i = 0; i + 1 < tcp_len; i += 2) {
        sum += read_be16(tcp + i);
        while ((sum >> 16) != 0) sum = (sum & 0xFFFF) + (sum >> 16);
    }
    if ((tcp_len & 1) != 0) {
        sum += (u32)tcp[tcp_len - 1] << 8;
        while ((sum >> 16) != 0) sum = (sum & 0xFFFF) + (sum >> 16);
    }
    while ((sum >> 16) != 0) sum = (sum & 0xFFFF) + (sum >> 16);
    const u16 checksum = (u16)~sum;
    return checksum == 0 ? 0xFFFF : checksum;
}

static void log_mac(void) {
    char line[96];
    u64 len = 0;
    line[0] = 0;
    append_str(line, sizeof(line), &len, "[virtio_net] VirtioNet: mac=");
    for (u64 i = 0; i < 6; i++) {
        if (i != 0) append_char(line, sizeof(line), &len, ':');
        append_hex_byte(line, sizeof(line), &len, g_mac[i]);
    }
    append_char(line, sizeof(line), &len, '\n');
    user_log_len(line, len);
}

static void log_rx_packet(u64 bytes, u16 ethertype) {
    char line[128];
    u64 len = 0;
    line[0] = 0;
    append_str(line, sizeof(line), &len, "[virtio_net] VirtioNet: rx packet len=");
    append_u64_dec(line, sizeof(line), &len, bytes);
    append_str(line, sizeof(line), &len, " ethertype=0x");
    append_hex_byte(line, sizeof(line), &len, (u8)(ethertype >> 8));
    append_hex_byte(line, sizeof(line), &len, (u8)(ethertype & 0xFF));
    append_char(line, sizeof(line), &len, '\n');
    user_log_len(line, len);
}

static void log_arp(u16 op) {
    char line[96];
    u64 len = 0;
    line[0] = 0;
    append_str(line, sizeof(line), &len, "[virtio_net] VirtioNet: arp op=");
    append_u64_dec(line, sizeof(line), &len, op);
    append_char(line, sizeof(line), &len, '\n');
    user_log_len(line, len);
}

static void log_arp_reply(const u8 *sha, u32 spa) {
    char line[160];
    u64 len = 0;
    line[0] = 0;
    append_str(line, sizeof(line), &len, "[virtio_net] VirtioNet: arp reply ");
    append_ipv4(line, sizeof(line), &len, spa);
    append_str(line, sizeof(line), &len, " mac=");
    for (u64 i = 0; i < 6; i++) {
        if (i != 0) append_char(line, sizeof(line), &len, ':');
        append_hex_byte(line, sizeof(line), &len, sha[i]);
    }
    append_char(line, sizeof(line), &len, '\n');
    user_log_len(line, len);
}

static void log_udp(u32 src_ip, u32 dst_ip, u16 src_port, u16 dst_port) {
    char line[160];
    u64 len = 0;
    line[0] = 0;
    append_str(line, sizeof(line), &len, "[virtio_net] VirtioNet: udp ");
    append_ipv4(line, sizeof(line), &len, src_ip);
    append_char(line, sizeof(line), &len, ':');
    append_u64_dec(line, sizeof(line), &len, src_port);
    append_str(line, sizeof(line), &len, " -> ");
    append_ipv4(line, sizeof(line), &len, dst_ip);
    append_char(line, sizeof(line), &len, ':');
    append_u64_dec(line, sizeof(line), &len, dst_port);
    append_char(line, sizeof(line), &len, '\n');
    user_log_len(line, len);
}

static void log_dhcp(u8 message_type, u32 yiaddr) {
    char line[192];
    u64 len = 0;
    line[0] = 0;
    append_str(line, sizeof(line), &len, "[virtio_net] VirtioNet: dhcp ");
    if (message_type == DHCP_MSG_OFFER) {
        append_str(line, sizeof(line), &len, "offer");
    } else if (message_type == DHCP_MSG_ACK) {
        append_str(line, sizeof(line), &len, "ack");
    } else {
        append_str(line, sizeof(line), &len, "message=");
        append_u64_dec(line, sizeof(line), &len, message_type);
    }
    append_str(line, sizeof(line), &len, " yiaddr=");
    append_ipv4(line, sizeof(line), &len, yiaddr);
    if (g_gateway_addr != 0) {
        append_str(line, sizeof(line), &len, " gw=");
        append_ipv4(line, sizeof(line), &len, g_gateway_addr);
    }
    if (g_dns_addr != 0) {
        append_str(line, sizeof(line), &len, " dns=");
        append_ipv4(line, sizeof(line), &len, g_dns_addr);
    }
    append_char(line, sizeof(line), &len, '\n');
    user_log_len(line, len);
}

static void write_net_response(u16 op, u64 seq, int status, u32 inline_bytes, u64 arg0, u64 arg1, u64 arg2) {
    if (g_current_session == 0) return;
    volatile struct net_response_header *response = (volatile struct net_response_header *)g_current_session->response_va;
    response->magic = NET_PROTOCOL_RESPONSE_MAGIC;
    response->version = NET_PROTOCOL_VERSION;
    response->op = op;
    response->status = status;
    response->inline_bytes = inline_bytes;
    response->arg0 = arg0;
    response->arg1 = arg1;
    response->arg2 = arg2;
    response->reserved0 = 0;
    __asm__ volatile("" ::: "memory");
    response->response_seq = seq;
    if (g_current_session != 0 && g_current_session->reply_endpoint_id != 0) (void)signal_endpoint(g_current_session->reply_endpoint_id);
}

static void write_net_status_payload(void) {
    volatile struct net_status_payload *payload = (volatile struct net_status_payload *)(g_current_session->response_va + sizeof(struct net_response_header));
    for (u64 i = 0; i < 6; i++) payload->mac[i] = g_mac[i];
    payload->link_up = 1;
    payload->dhcp_bound = g_dhcp_configured ? 1 : 0;
    payload->ipv4_addr = g_ipv4_addr;
    payload->gateway_addr = g_gateway_addr;
    payload->dns_addr = g_dns_addr;
    payload->dhcp_server_addr = g_dhcp_server_addr;
    payload->flags = NET_FLAG_LINK_UP |
        (g_dhcp_configured ? NET_FLAG_DHCP_BOUND : 0) |
        (g_arp_reply_seen ? NET_FLAG_GATEWAY_ARP : 0);
    payload->rx_packets = g_rx_packets;
    payload->tx_completions = g_tx_completions;
    payload->tcp_rx_segments = g_tcp_rx_segments;
    payload->tcp_rx_payload_bytes = g_tcp_rx_payload_bytes;
    payload->tcp_rx_in_order_bytes = g_tcp_rx_in_order_bytes;
    payload->tcp_rx_duplicate_segments = g_tcp_rx_duplicate_segments;
    payload->tcp_rx_out_of_order_segments = g_tcp_rx_out_of_order_segments;
    payload->tcp_rx_ooo_stored = g_tcp_rx_ooo_stored;
    payload->tcp_rx_ooo_drained = g_tcp_rx_ooo_drained;
    payload->tcp_rx_ooo_dropped = g_tcp_rx_ooo_dropped;
    payload->tcp_rx_append_failed = g_tcp_rx_append_failed;
    payload->tcp_ack_sent = g_tcp_ack_sent;
    payload->tcp_ack_deferred = g_tcp_ack_deferred;
    payload->tcp_ack_flushed = g_tcp_ack_flushed;
    payload->tcp_tx_busy = g_tcp_tx_busy;
    payload->tcp_connect_requests = g_tcp_connect_requests;
    payload->tcp_connect_established = g_tcp_connect_established;
    payload->tcp_tx_segments = g_tcp_tx_segments;
    payload->tcp_tx_payload_bytes = g_tcp_tx_payload_bytes;
    payload->tcp_read_requests = g_tcp_read_requests;
    payload->tcp_read_would_block = g_tcp_read_would_block;
    payload->tcp_read_bytes = g_tcp_read_bytes;
    payload->tcp_poll_requests = g_tcp_poll_requests;
    payload->tcp_poll_readable = g_tcp_poll_readable;
    payload->tcp_rx_syn_ack = g_tcp_rx_syn_ack;
    payload->tcp_rx_fin = g_tcp_rx_fin;
    payload->tcp_rx_rst = g_tcp_rx_rst;
    u64 active = 0;
    u64 established = 0;
    u64 buffered = 0;
    u64 max_buffered = 0;
    u64 ack_pending = 0;
    tcp_connection_totals(&active, &established, &buffered, &max_buffered, &ack_pending);
    payload->tcp_active_connections = active;
    payload->tcp_established_connections = established;
    payload->tcp_rx_buffered_bytes = buffered;
    payload->tcp_rx_buffer_max_bytes = max_buffered;
    payload->tcp_ack_pending_connections = ack_pending;
    payload->net_service_requests = g_net_service_requests;
    payload->net_service_work_loops = g_net_service_work_loops;
    payload->net_service_idle_sleeps = g_net_service_idle_sleeps;
}

static const u8 *net_request_payload(void) {
    return (const u8 *)(g_current_session->request_va + sizeof(struct net_request_header));
}

static u8 *net_response_payload(void) {
    return (u8 *)(g_current_session->response_va + sizeof(struct net_response_header));
}

static int udp_handle_index(u64 handle, u64 *index_out) {
    if ((handle & 0xFFFFFFFF00000000ULL) != NET_UDP_HANDLE_TAG) return 0;
    const u64 index = handle & 0xFFFF;
    if (index == 0 || index > NET_UDP_BINDINGS) return 0;
    *index_out = index - 1;
    return 1;
}

static struct udp_binding *find_udp_binding_by_handle(u64 handle) {
    u64 index = 0;
    if (!udp_handle_index(handle, &index)) return 0;
    struct udp_binding *binding = &g_udp_bindings[index];
    if (!binding->active || binding->handle != handle) return 0;
    return binding;
}

static struct udp_binding *find_udp_binding_by_port(u16 port) {
    for (u64 i = 0; i < NET_UDP_BINDINGS; i++) {
        if (g_udp_bindings[i].active && g_udp_bindings[i].local_port == port) return &g_udp_bindings[i];
    }
    return 0;
}

static int udp_port_in_use(u16 port) {
    return find_udp_binding_by_port(port) != 0;
}

static int tcp_port_in_use(u16 port);

static u16 allocate_ephemeral_port(void) {
    for (u32 attempts = 0; attempts < 16384; attempts++) {
        u16 port = g_next_ephemeral_port++;
        if (g_next_ephemeral_port < NET_EPHEMERAL_PORT_BASE) g_next_ephemeral_port = NET_EPHEMERAL_PORT_BASE;
        if (!udp_port_in_use(port) && !tcp_port_in_use(port)) return port;
    }
    return 0;
}

static struct udp_binding *alloc_udp_binding(u16 local_port) {
    if (local_port == 0) local_port = allocate_ephemeral_port();
    if (local_port == 0 || udp_port_in_use(local_port)) return 0;

    for (u64 i = 0; i < NET_UDP_BINDINGS; i++) {
        struct udp_binding *binding = &g_udp_bindings[i];
        if (binding->active) continue;
        memset(binding, 0, sizeof(*binding));
        binding->active = 1;
        binding->local_port = local_port;
        binding->handle = NET_UDP_HANDLE_TAG |
            (((g_rx_packets + g_tx_completions + (u64)local_port) & 0xFFFF) << 16) |
            (i + 1);
        return binding;
    }
    return 0;
}

static int close_udp_binding(u64 handle) {
    struct udp_binding *binding = find_udp_binding_by_handle(handle);
    if (binding == 0) return 0;
    memset(binding, 0, sizeof(*binding));
    return 1;
}

static int enqueue_udp_packet(u32 src_ip, u16 src_port, u16 dst_port, const u8 *payload, u32 len) {
    struct udp_binding *binding = find_udp_binding_by_port(dst_port);
    if (binding == 0 || len > NET_UDP_MAX_PAYLOAD) return 0;
    if (binding->count >= NET_UDP_PENDING) return 0;

    const u16 slot = (u16)((binding->head + binding->count) % NET_UDP_PENDING);
    struct udp_packet *packet = &binding->packets[slot];
    memset(packet, 0, sizeof(*packet));
    packet->used = 1;
    packet->src_ip = src_ip;
    packet->src_port = src_port;
    packet->dst_port = dst_port;
    packet->len = len;
    memcpy(packet->bytes, payload, len);
    binding->count++;
    return 1;
}

static int dequeue_udp_packet(u64 handle, u8 *out, u32 out_cap, u32 *out_len, u32 *src_ip, u16 *src_port) {
    struct udp_binding *binding = find_udp_binding_by_handle(handle);
    if (binding == 0) return NET_STATUS_INVALID;
    if (binding->count == 0) return NET_STATUS_WOULD_BLOCK;

    struct udp_packet *packet = &binding->packets[binding->head];
    const u32 copy_len = packet->len < out_cap ? packet->len : out_cap;
    memcpy(out, packet->bytes, copy_len);
    *out_len = copy_len;
    *src_ip = packet->src_ip;
    *src_port = packet->src_port;
    memset(packet, 0, sizeof(*packet));
    binding->head = (u16)((binding->head + 1) % NET_UDP_PENDING);
    binding->count--;
    return NET_STATUS_OK;
}

static int poll_udp_binding(u64 handle, u64 *events_out) {
    struct udp_binding *binding = find_udp_binding_by_handle(handle);
    if (binding == 0) return NET_STATUS_INVALID;
    u64 events = 0;
    if (binding->count != 0) events |= NET_POLL_READABLE;
    if (g_dhcp_configured && g_ipv4_addr != 0 && g_gateway_addr != 0 && g_gateway_mac_ready && !g_tx_in_flight) {
        events |= NET_POLL_WRITABLE;
    }
    *events_out = events;
    return NET_STATUS_OK;
}

static int parse_boot_state(void) {
    if (cfg_read(0) != NET_CONFIG_MAGIC || cfg_read(1) != NET_CONFIG_VERSION) return 0;
    g_rx_queue.submit_token = cfg_read(NET_RX_QUEUE_SUBMIT_TOKEN_INDEX);
    g_rx_queue.notify_token = cfg_read(NET_RX_QUEUE_NOTIFY_TOKEN_INDEX);
    g_tx_queue.submit_token = cfg_read(NET_TX_QUEUE_SUBMIT_TOKEN_INDEX);
    g_tx_queue.notify_token = cfg_read(NET_TX_QUEUE_NOTIFY_TOKEN_INDEX);
    g_boot.endpoint_id = cfg_read(NET_ENDPOINT_ID_INDEX);
    g_boot.resource_id = cfg_read(NET_RESOURCE_ID_INDEX);
    g_boot.common_page_paddr = cfg_read(NET_COMMON_PAGE_PADDR_INDEX);
    g_boot.notify_page_paddr = cfg_read(NET_NOTIFY_PAGE_PADDR_INDEX);
    g_boot.isr_page_paddr = cfg_read(NET_ISR_PAGE_PADDR_INDEX);
    g_boot.device_page_paddr = cfg_read(NET_DEVICE_PAGE_PADDR_INDEX);
    g_boot.common_page_offset = cfg_read(NET_COMMON_PAGE_OFFSET_INDEX);
    g_boot.notify_page_offset = cfg_read(NET_NOTIFY_PAGE_OFFSET_INDEX);
    g_boot.isr_page_offset = cfg_read(NET_ISR_PAGE_OFFSET_INDEX);
    g_boot.device_page_offset = cfg_read(NET_DEVICE_PAGE_OFFSET_INDEX);
    g_boot.notify_off_multiplier = cfg_read(NET_NOTIFY_OFF_MULTIPLIER_INDEX);
    g_boot.iommu_token = cfg_read(NET_IOMMU_TOKEN_INDEX);
    g_boot.command_token = cfg_read(NET_COMMAND_TOKEN_INDEX);
    return g_boot.endpoint_id != 0 &&
        g_boot.resource_id != 0 &&
        g_boot.common_page_paddr != 0 &&
        g_boot.notify_page_paddr != 0 &&
        g_rx_queue.submit_token != 0 &&
        g_rx_queue.notify_token != 0 &&
        g_tx_queue.submit_token != 0 &&
        g_tx_queue.notify_token != 0;
}

static void wait_for_boot_resources(void) {
    while (!parse_boot_state()) wait_event(0, 1);
}

static struct virtq_desc *desc_ptr(struct queue_state *queue, u64 index) {
    return (struct virtq_desc *)(queue->page_va + index * sizeof(struct virtq_desc));
}

static volatile u16 *avail_idx_ptr(struct queue_state *queue) {
    return (volatile u16 *)(queue->page_va + QUEUE_SIZE * sizeof(struct virtq_desc) + 2);
}

static volatile u16 *avail_ring_ptr(struct queue_state *queue) {
    return (volatile u16 *)(queue->page_va + QUEUE_SIZE * sizeof(struct virtq_desc) + 4);
}

static volatile u16 *used_idx_ptr(struct queue_state *queue) {
    return (volatile u16 *)(queue->page_va + QUEUE_USED_OFFSET + 2);
}

static volatile struct virtq_used_elem *used_ring_ptr(struct queue_state *queue) {
    return (volatile struct virtq_used_elem *)(queue->page_va + QUEUE_USED_OFFSET + 4);
}

static void queue_push_avail(struct queue_state *queue, u16 desc_index) {
    const u16 idx = *avail_idx_ptr(queue);
    avail_ring_ptr(queue)[idx % QUEUE_SIZE] = desc_index;
    __asm__ volatile("" ::: "memory");
    *avail_idx_ptr(queue) = (u16)(idx + 1);
}

static int setup_queue(struct queue_state *queue) {
    mmio_write_u16(g_common_base + COMMON_QUEUE_SELECT, queue->index);
    const u16 max_size = mmio_read_u16(g_common_base + COMMON_QUEUE_SIZE);
    if (max_size == 0 || max_size < QUEUE_SIZE) return 0;
    mmio_write_u16(g_common_base + COMMON_QUEUE_SIZE, QUEUE_SIZE);
    mmio_write_u64(g_common_base + COMMON_QUEUE_DESC, queue->page_paddr);
    mmio_write_u64(g_common_base + COMMON_QUEUE_AVAIL, queue->page_paddr + QUEUE_SIZE * sizeof(struct virtq_desc));
    mmio_write_u64(g_common_base + COMMON_QUEUE_USED, queue->page_paddr + QUEUE_USED_OFFSET);
    const u16 queue_notify_off = mmio_read_u16(g_common_base + COMMON_QUEUE_NOTIFY_OFF);
    queue->notify_addr = g_notify_base + (u64)queue_notify_off * g_boot.notify_off_multiplier;
    mmio_write_u16(g_common_base + COMMON_QUEUE_ENABLE, 1);
    return 1;
}

static int init_queue_memory(void) {
    u64 q_paddrs[2] = { 0, 0 };
    if (alloc_map_pages(RX_QUEUE_PAGE_VA, 1, 1, (u64)&q_paddrs[0]) != SYSCALL_OK) return 0;
    if (alloc_map_pages(TX_QUEUE_PAGE_VA, 1, 1, (u64)&q_paddrs[1]) != SYSCALL_OK) return 0;
    g_rx_queue.page_paddr = q_paddrs[0];
    g_tx_queue.page_paddr = q_paddrs[1];
    memset((void *)RX_QUEUE_PAGE_VA, 0, 4096);
    memset((void *)TX_QUEUE_PAGE_VA, 0, 4096);

    for (u64 i = 0; i < RX_BUFFER_COUNT; i++) {
        if (alloc_map_pages(RX_BUFFER_BASE_VA + i * 4096, 1, 1, (u64)&g_rx_buffer_paddrs[i]) != SYSCALL_OK) return 0;
        memset((void *)(RX_BUFFER_BASE_VA + i * 4096), 0, 4096);
    }
    if (alloc_map_pages(TX_BUFFER_VA, 1, 1, (u64)&g_tx_buffer_paddr) != SYSCALL_OK) return 0;
    memset((void *)TX_BUFFER_VA, 0, 4096);
    return 1;
}

static int authorize_dma(void) {
    if (g_boot.iommu_token == 0) return 1;
    if (iommu_authorize(g_boot.iommu_token, g_boot.resource_id, IOMMU_OP_MAP_READ) != SYSCALL_OK) return 0;
    for (u64 i = 0; i < RX_BUFFER_COUNT; i++) {
        g_rx_dma_tokens[i] = dma_map_create(g_boot.resource_id, g_rx_buffer_paddrs[i], RX_BUFFER_BYTES, DMA_DIRECTION_WRITE);
        if (g_rx_dma_tokens[i] == 0) return 0;
        if (dma_map_set_state(g_rx_dma_tokens[i], DMA_STATE_IN_FLIGHT) != SYSCALL_OK) return 0;
    }
    if (iommu_authorize(g_boot.iommu_token, g_boot.resource_id, IOMMU_OP_MAP_READ) != SYSCALL_OK) return 0;
    g_tx_dma_token = dma_map_create(g_boot.resource_id, g_tx_buffer_paddr, TX_BUFFER_BYTES, DMA_DIRECTION_READ);
    if (g_tx_dma_token == 0) return 0;
    return dma_map_set_state(g_tx_dma_token, DMA_STATE_IN_FLIGHT) == SYSCALL_OK;
}

static int prime_rx_queue(void) {
    for (u64 i = 0; i < RX_BUFFER_COUNT; i++) {
        struct virtq_desc *desc = desc_ptr(&g_rx_queue, i);
        desc->addr = g_rx_buffer_paddrs[i];
        desc->len = RX_BUFFER_BYTES;
        desc->flags = DESC_FLAG_WRITE;
        desc->next = 0;
        queue_push_avail(&g_rx_queue, (u16)i);
    }
    if (queue_submit(g_rx_queue.submit_token, g_rx_queue.index) != SYSCALL_OK) return 0;
    if (queue_notify(g_rx_queue.notify_token, g_rx_queue.index) != SYSCALL_OK) return 0;
    mmio_write_u16(g_rx_queue.notify_addr, g_rx_queue.index);
    return 1;
}

static int send_packet(const u8 *frame, u32 frame_len) {
    if (g_tx_in_flight) return 0;
    if (frame_len + NET_HDR_BYTES > TX_BUFFER_BYTES) return 0;
    u8 *tx = (u8 *)TX_BUFFER_VA;
    memset(tx, 0, NET_HDR_BYTES);
    memcpy(tx + NET_HDR_BYTES, frame, frame_len);

    struct virtq_desc *desc = desc_ptr(&g_tx_queue, 0);
    desc->addr = g_tx_buffer_paddr;
    desc->len = frame_len + NET_HDR_BYTES;
    desc->flags = 0;
    desc->next = 0;
    if (queue_submit(g_tx_queue.submit_token, g_tx_queue.index) != SYSCALL_OK) return 0;
    queue_push_avail(&g_tx_queue, 0);
    if (queue_notify(g_tx_queue.notify_token, g_tx_queue.index) != SYSCALL_OK) return 0;
    mmio_write_u16(g_tx_queue.notify_addr, g_tx_queue.index);
    g_tx_in_flight = 1;
    return 1;
}

static int send_arp_request(u32 source_ip, u32 target_ip);

static int send_ipv4_udp(u64 handle, u32 remote_ip, u16 remote_port, const u8 *payload, u32 payload_len) {
    struct udp_binding *binding = find_udp_binding_by_handle(handle);
    if (binding == 0 || remote_ip == 0 || remote_port == 0) return NET_STATUS_INVALID;
    if (payload_len > NET_UDP_MAX_PAYLOAD || payload_len > NET_PAYLOAD_BYTES) return NET_STATUS_TOO_BIG;
    if (!g_dhcp_configured || g_ipv4_addr == 0 || g_gateway_addr == 0) return NET_STATUS_NO_ROUTE;
    if (!g_gateway_mac_ready) {
        if (!g_tx_in_flight) (void)send_arp_request(g_ipv4_addr, g_gateway_addr);
        return NET_STATUS_NO_ROUTE;
    }
    if (g_tx_in_flight) return NET_STATUS_BUSY;

    u8 frame[ETH_HDR_BYTES + 20 + 8 + NET_UDP_MAX_PAYLOAD];
    memset(frame, 0, sizeof(frame));
    memcpy(frame + 0, g_gateway_mac, 6);
    memcpy(frame + 6, g_mac, 6);
    write_be16(frame + 12, ETHERTYPE_IPV4);

    u8 *ip = frame + ETH_HDR_BYTES;
    u8 *udp = ip + 20;
    const u16 udp_len = (u16)(8 + payload_len);
    const u16 ip_len = (u16)(20 + udp_len);
    const u16 frame_len = (u16)(ETH_HDR_BYTES + ip_len);

    ip[0] = 0x45;
    ip[1] = 0;
    write_be16(ip + 2, ip_len);
    write_be16(ip + 4, (u16)(3 + (g_tx_completions & 0x7FFF)));
    write_be16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = IP_PROTO_UDP;
    write_be32(ip + 12, g_ipv4_addr);
    write_be32(ip + 16, remote_ip);
    write_be16(ip + 10, ipv4_checksum(ip, 20));

    write_be16(udp + 0, binding->local_port);
    write_be16(udp + 2, remote_port);
    write_be16(udp + 4, udp_len);
    write_be16(udp + 6, 0);
    memcpy(udp + 8, payload, payload_len);
    write_be16(udp + 6, udp_ipv4_checksum(g_ipv4_addr, remote_ip, udp, udp_len));

    return send_packet(frame, frame_len) ? NET_STATUS_OK : NET_STATUS_BUSY;
}

static int tcp_handle_index(u64 handle, u64 *index_out) {
    if ((handle & 0xFFFFFFFF00000000ULL) != NET_TCP_HANDLE_TAG) return 0;
    const u64 index = handle & 0xFFFF;
    if (index == 0 || index > NET_TCP_CONNECTIONS) return 0;
    *index_out = index - 1;
    return 1;
}

static struct tcp_connection *find_tcp_connection_by_handle(u64 handle) {
    u64 index = 0;
    if (!tcp_handle_index(handle, &index)) return 0;
    struct tcp_connection *conn = &g_tcp_connections[index];
    if (!conn->active || conn->handle != handle) return 0;
    return conn;
}

static struct tcp_connection *find_tcp_connection_by_tuple(u32 remote_ip, u16 remote_port, u16 local_port) {
    for (u64 i = 0; i < NET_TCP_CONNECTIONS; i++) {
        struct tcp_connection *conn = &g_tcp_connections[i];
        if (!conn->active) continue;
        if (conn->remote_ip == remote_ip && conn->remote_port == remote_port && conn->local_port == local_port) return conn;
    }
    return 0;
}

static int tcp_port_in_use(u16 port) {
    for (u64 i = 0; i < NET_TCP_CONNECTIONS; i++) {
        if (g_tcp_connections[i].active && g_tcp_connections[i].local_port == port) return 1;
    }
    return 0;
}

static int tcp_rx_append(struct tcp_connection *conn, const u8 *payload, u32 len) {
    if (len > NET_TCP_RX_BYTES - conn->rx_len) return 0;
    for (u32 i = 0; i < len; i++) {
        const u32 index = (conn->rx_head + conn->rx_len + i) % NET_TCP_RX_BYTES;
        conn->rx[index] = payload[i];
    }
    conn->rx_len += len;
    return 1;
}

static int tcp_seq_lt(u32 a, u32 b) {
    return (int)(a - b) < 0;
}

static int tcp_seq_leq(u32 a, u32 b) {
    return (int)(a - b) <= 0;
}

static int tcp_rx_append_counted(struct tcp_connection *conn, const u8 *payload, u32 len) {
    if (!tcp_rx_append(conn, payload, len)) {
        g_tcp_rx_append_failed++;
        tcp_log_rx_full(conn, len);
        return 0;
    }
    g_tcp_rx_in_order_bytes += len;
    tcp_log_progress_if_needed();
    return 1;
}

static int tcp_ooo_range_overlaps(const struct tcp_ooo_segment *seg, u32 seq, u32 len) {
    const u32 seg_end = seg->seq + seg->len;
    const u32 end = seq + len;
    return tcp_seq_lt(seq, seg_end) && tcp_seq_lt(seg->seq, end);
}

static int tcp_store_ooo(struct tcp_connection *conn, u32 seq, const u8 *payload, u32 len) {
    if (len == 0) return 1;
    if (len > NET_TCP_OOO_BYTES) {
        g_tcp_rx_ooo_dropped++;
        return 0;
    }
    for (u64 i = 0; i < NET_TCP_OOO_SEGMENTS; i++) {
        const struct tcp_ooo_segment *seg = &conn->ooo[i];
        if (seg->used && tcp_ooo_range_overlaps(seg, seq, len)) {
            if (seg->seq == seq && seg->len == len) return 1;
            g_tcp_rx_ooo_dropped++;
            return 0;
        }
    }
    for (u64 i = 0; i < NET_TCP_OOO_SEGMENTS; i++) {
        struct tcp_ooo_segment *seg = &conn->ooo[i];
        if (seg->used) continue;
        seg->used = 1;
        seg->seq = seq;
        seg->len = len;
        memcpy(seg->bytes, payload, len);
        g_tcp_rx_ooo_stored++;
        return 1;
    }
    g_tcp_rx_ooo_dropped++;
    return 0;
}

static void tcp_drain_ooo(struct tcp_connection *conn) {
    for (;;) {
        struct tcp_ooo_segment *match = 0;
        for (u64 i = 0; i < NET_TCP_OOO_SEGMENTS; i++) {
            struct tcp_ooo_segment *seg = &conn->ooo[i];
            if (!seg->used) continue;
            const u32 seg_end = seg->seq + seg->len;
            if (tcp_seq_leq(seg_end, conn->ack)) {
                seg->used = 0;
                seg->len = 0;
                continue;
            }
            if (tcp_seq_leq(seg->seq, conn->ack) && tcp_seq_lt(conn->ack, seg_end)) {
                match = seg;
                break;
            }
        }
        if (match == 0) return;
        const u32 skip = conn->ack - match->seq;
        u32 append_len = match->len - skip;
        const u32 free_bytes = NET_TCP_RX_BYTES - conn->rx_len;
        if (free_bytes == 0) return;
        if (append_len > free_bytes) append_len = free_bytes;
        if (!tcp_rx_append_counted(conn, match->bytes + skip, append_len)) return;
        conn->ack += append_len;
        if (skip + append_len >= match->len) {
            match->used = 0;
            match->len = 0;
            g_tcp_rx_ooo_drained++;
        }
    }
}

static u16 tcp_advertised_window(const struct tcp_connection *conn) {
    const u32 free_bytes = NET_TCP_RX_BYTES - conn->rx_len;
    return (u16)(free_bytes > NET_TCP_WINDOW_MAX ? NET_TCP_WINDOW_MAX : free_bytes);
}

static u32 tcp_rx_pop(struct tcp_connection *conn, u8 *out, u32 out_cap) {
    const u32 n = conn->rx_len < out_cap ? conn->rx_len : out_cap;
    for (u32 i = 0; i < n; i++) {
        out[i] = conn->rx[(conn->rx_head + i) % NET_TCP_RX_BYTES];
    }
    conn->rx_head = (conn->rx_head + n) % NET_TCP_RX_BYTES;
    conn->rx_len -= n;
    return n;
}

static int send_ipv4_tcp_segment(struct tcp_connection *conn, u8 flags, const u8 *payload, u32 payload_len) {
    if (conn == 0 || !conn->active || conn->remote_ip == 0 || conn->remote_port == 0) return NET_STATUS_INVALID;
    if (payload_len > NET_TCP_MAX_PAYLOAD || payload_len > NET_PAYLOAD_BYTES) return NET_STATUS_TOO_BIG;
    if (!g_dhcp_configured || g_ipv4_addr == 0 || g_gateway_addr == 0) return NET_STATUS_NO_ROUTE;
    if (!g_gateway_mac_ready) {
        if (!g_tx_in_flight) (void)send_arp_request(g_ipv4_addr, g_gateway_addr);
        return NET_STATUS_NO_ROUTE;
    }
    if (g_tx_in_flight) {
        g_tcp_tx_busy++;
        return NET_STATUS_BUSY;
    }

    const u32 tcp_header_len = (flags & TCP_FLAG_SYN) != 0 ? 24 : 20;
    u8 frame[ETH_HDR_BYTES + 20 + 24 + NET_TCP_MAX_PAYLOAD];
    memset(frame, 0, sizeof(frame));
    memcpy(frame + 0, g_gateway_mac, 6);
    memcpy(frame + 6, g_mac, 6);
    write_be16(frame + 12, ETHERTYPE_IPV4);

    u8 *ip = frame + ETH_HDR_BYTES;
    u8 *tcp = ip + 20;
    const u16 tcp_len = (u16)(tcp_header_len + payload_len);
    const u16 ip_len = (u16)(20 + tcp_len);
    const u16 frame_len = (u16)(ETH_HDR_BYTES + ip_len);

    ip[0] = 0x45;
    ip[1] = 0;
    write_be16(ip + 2, ip_len);
    write_be16(ip + 4, (u16)(0x4000 | (g_tx_completions & 0x3FFF)));
    write_be16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = IP_PROTO_TCP;
    write_be32(ip + 12, conn->local_ip);
    write_be32(ip + 16, conn->remote_ip);
    write_be16(ip + 10, ipv4_checksum(ip, 20));

    write_be16(tcp + 0, conn->local_port);
    write_be16(tcp + 2, conn->remote_port);
    write_be32(tcp + 4, conn->seq);
    write_be32(tcp + 8, conn->ack);
    tcp[12] = (u8)((tcp_header_len / 4) << 4);
    tcp[13] = flags;
    write_be16(tcp + 14, tcp_advertised_window(conn));
    write_be16(tcp + 16, 0);
    write_be16(tcp + 18, 0);
    if ((flags & TCP_FLAG_SYN) != 0) {
        tcp[20] = 2;
        tcp[21] = 4;
        write_be16(tcp + 22, NET_TCP_MSS);
    }
    if (payload_len != 0) memcpy(tcp + tcp_header_len, payload, payload_len);
    write_be16(tcp + 16, tcp_ipv4_checksum(conn->local_ip, conn->remote_ip, tcp, tcp_len));

    if (!send_packet(frame, frame_len)) {
        g_tcp_tx_busy++;
        return NET_STATUS_BUSY;
    }
    g_tcp_tx_segments++;
    g_tcp_tx_payload_bytes += payload_len;
    if ((flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) != 0) conn->seq++;
    conn->seq += payload_len;
    return NET_STATUS_OK;
}

static void tcp_send_or_defer_ack(struct tcp_connection *conn) {
    if (conn == 0 || !conn->active) return;
    if (!g_tx_in_flight && send_ipv4_tcp_segment(conn, TCP_FLAG_ACK, 0, 0) == NET_STATUS_OK) {
        conn->ack_pending = 0;
        g_tcp_ack_sent++;
        return;
    }
    conn->ack_pending = 1;
    g_tcp_ack_deferred++;
}

static int flush_pending_tcp_acks(void) {
    if (g_tx_in_flight) return 0;
    for (u64 i = 0; i < NET_TCP_CONNECTIONS; i++) {
        struct tcp_connection *conn = &g_tcp_connections[i];
        if (!conn->active || !conn->ack_pending) continue;
        conn->ack_pending = 0;
        if (send_ipv4_tcp_segment(conn, TCP_FLAG_ACK, 0, 0) != NET_STATUS_OK) {
            conn->ack_pending = 1;
        } else {
            g_tcp_ack_sent++;
            g_tcp_ack_flushed++;
            return 1;
        }
        return 0;
    }
    return 0;
}

static struct tcp_connection *alloc_tcp_connection(u32 remote_ip, u16 remote_port) {
    const u16 local_port = allocate_ephemeral_port();
    if (local_port == 0) return 0;
    for (u64 i = 0; i < NET_TCP_CONNECTIONS; i++) {
        struct tcp_connection *conn = &g_tcp_connections[i];
        if (conn->active) continue;
        memset(conn, 0, sizeof(*conn));
        conn->active = 1;
        conn->state = TCP_STATE_SYN_SENT;
        conn->local_ip = g_ipv4_addr;
        conn->remote_ip = remote_ip;
        conn->local_port = local_port;
        conn->remote_port = remote_port;
        conn->initial_seq = (u32)(0x43415000u + ((g_rx_packets + g_tx_completions + local_port) & 0xFFFF));
        conn->seq = conn->initial_seq;
        conn->ack = 0;
        conn->handle = NET_TCP_HANDLE_TAG |
            (((u64)local_port & 0xFFFF) << 16) |
            (i + 1);
        return conn;
    }
    return 0;
}

static int net_tcp_connect(u32 remote_ip, u16 remote_port, u64 *handle_out, u16 *local_port_out, u32 *local_ip_out) {
    if (remote_ip == 0 || remote_port == 0) return NET_STATUS_INVALID;
    if (!g_dhcp_configured || g_ipv4_addr == 0 || g_gateway_addr == 0) return NET_STATUS_NO_ROUTE;
    g_tcp_connect_requests++;
    struct tcp_connection *conn = alloc_tcp_connection(remote_ip, remote_port);
    if (conn == 0) return NET_STATUS_PORT_IN_USE;
    const int status = send_ipv4_tcp_segment(conn, TCP_FLAG_SYN, 0, 0);
    if (status != NET_STATUS_OK) {
        memset(conn, 0, sizeof(*conn));
        return status;
    }
    *handle_out = conn->handle;
    *local_port_out = conn->local_port;
    *local_ip_out = conn->local_ip;
    return NET_STATUS_OK;
}

static int tcp_write(u64 handle, const u8 *payload, u32 payload_len, u32 *written_out) {
    struct tcp_connection *conn = find_tcp_connection_by_handle(handle);
    if (conn == 0) return NET_STATUS_INVALID;
    if (conn->state != TCP_STATE_ESTABLISHED) return NET_STATUS_NOT_CONNECTED;
    const int status = send_ipv4_tcp_segment(conn, TCP_FLAG_ACK | TCP_FLAG_PSH, payload, payload_len);
    if (status == NET_STATUS_OK) *written_out = payload_len;
    return status;
}

static int tcp_read(u64 handle, u8 *out, u32 out_cap, u32 *out_len) {
    struct tcp_connection *conn = find_tcp_connection_by_handle(handle);
    if (conn == 0) return NET_STATUS_INVALID;
    g_tcp_read_requests++;
    if (conn->state == TCP_STATE_ESTABLISHED) tcp_drain_ooo(conn);
    if (conn->rx_len == 0) {
        if (conn->peer_closed || conn->state == TCP_STATE_CLOSED) {
            *out_len = 0;
            return NET_STATUS_OK;
        }
        g_tcp_read_would_block++;
        return NET_STATUS_WOULD_BLOCK;
    }
    *out_len = tcp_rx_pop(conn, out, out_cap);
    g_tcp_read_bytes += *out_len;
    if (conn->state == TCP_STATE_ESTABLISHED) {
        tcp_drain_ooo(conn);
        tcp_send_or_defer_ack(conn);
    }
    return NET_STATUS_OK;
}

static int poll_tcp_connection(u64 handle, u64 *events_out) {
    struct tcp_connection *conn = find_tcp_connection_by_handle(handle);
    if (conn == 0) return NET_STATUS_INVALID;
    g_tcp_poll_requests++;
    if (conn->state == TCP_STATE_ESTABLISHED) tcp_drain_ooo(conn);
    u64 events = 0;
    if (conn->rx_len != 0 || conn->peer_closed) events |= NET_POLL_READABLE;
    if (conn->state == TCP_STATE_ESTABLISHED && !g_tx_in_flight) events |= NET_POLL_WRITABLE;
    if ((events & NET_POLL_READABLE) != 0) g_tcp_poll_readable++;
    *events_out = events;
    return NET_STATUS_OK;
}

static int close_tcp_connection(u64 handle) {
    struct tcp_connection *conn = find_tcp_connection_by_handle(handle);
    if (conn == 0) return 0;
    if (conn->state == TCP_STATE_ESTABLISHED && !g_tx_in_flight) {
        (void)send_ipv4_tcp_segment(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
    }
    memset(conn, 0, sizeof(*conn));
    return 1;
}

static int send_dhcp_discover(void) {
    u8 frame[512];
    memset(frame, 0, sizeof(frame));

    for (u32 i = 0; i < 6; i++) frame[i] = 0xFF;
    memcpy(frame + 6, g_mac, 6);
    write_be16(frame + 12, ETHERTYPE_IPV4);

    u8 *ip = frame + ETH_HDR_BYTES;
    u8 *udp = ip + 20;
    u8 *dhcp = udp + 8;
    u8 *opt = dhcp + 236;

    dhcp[0] = 1; // BOOTREQUEST
    dhcp[1] = 1; // Ethernet
    dhcp[2] = 6; // MAC length
    write_be32(dhcp + 4, g_dhcp_xid);
    write_be16(dhcp + 10, 0x8000);
    memcpy(dhcp + 28, g_mac, 6);

    write_be32(opt, DHCP_MAGIC_COOKIE);
    opt += 4;
    *opt++ = 53;
    *opt++ = 1;
    *opt++ = DHCP_MSG_DISCOVER;
    *opt++ = 55;
    *opt++ = 8;
    *opt++ = 1;
    *opt++ = 3;
    *opt++ = 6;
    *opt++ = 15;
    *opt++ = 28;
    *opt++ = 51;
    *opt++ = 54;
    *opt++ = 58;
    *opt++ = 57;
    *opt++ = 2;
    write_be16(opt, 576);
    opt += 2;
    *opt++ = 12;
    *opt++ = 7;
    *opt++ = 'p';
    *opt++ = 'a';
    *opt++ = 'c';
    *opt++ = 'h';
    *opt++ = 'a';
    *opt++ = 'o';
    *opt++ = 's';
    *opt++ = 255;
    while ((u32)(opt - dhcp) < 300) *opt++ = 0;

    const u16 dhcp_len = (u16)(opt - dhcp);
    const u16 udp_len = (u16)(8 + dhcp_len);
    const u16 ip_len = (u16)(20 + udp_len);
    const u16 frame_len = (u16)(ETH_HDR_BYTES + ip_len);

    ip[0] = 0x45;
    ip[1] = 0;
    write_be16(ip + 2, ip_len);
    write_be16(ip + 4, 1);
    write_be16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = IP_PROTO_UDP;
    write_be32(ip + 12, 0);
    write_be32(ip + 16, 0xFFFFFFFF);
    write_be16(ip + 10, ipv4_checksum(ip, 20));

    write_be16(udp + 0, DHCP_CLIENT_PORT);
    write_be16(udp + 2, DHCP_SERVER_PORT);
    write_be16(udp + 4, udp_len);
    write_be16(udp + 6, 0);
    write_be16(udp + 6, udp_ipv4_checksum(0, 0xFFFFFFFF, udp, udp_len));

    if (!send_packet(frame, frame_len)) {
        user_log("[virtio_net] VirtioNet: dhcp discover failed\n");
        return 0;
    }
    g_dhcp_sent = 1;
    user_log("[virtio_net] VirtioNet: dhcp discover sent\n");
    return 1;
}

static int send_dhcp_request(void) {
    if (g_dhcp_offer_addr == 0) return 0;
    u8 frame[512];
    memset(frame, 0, sizeof(frame));

    for (u32 i = 0; i < 6; i++) frame[i] = 0xFF;
    memcpy(frame + 6, g_mac, 6);
    write_be16(frame + 12, ETHERTYPE_IPV4);

    u8 *ip = frame + ETH_HDR_BYTES;
    u8 *udp = ip + 20;
    u8 *dhcp = udp + 8;
    u8 *opt = dhcp + 236;

    dhcp[0] = 1;
    dhcp[1] = 1;
    dhcp[2] = 6;
    write_be32(dhcp + 4, g_dhcp_xid);
    write_be16(dhcp + 10, 0x8000);
    memcpy(dhcp + 28, g_mac, 6);

    write_be32(opt, DHCP_MAGIC_COOKIE);
    opt += 4;
    *opt++ = 53;
    *opt++ = 1;
    *opt++ = DHCP_MSG_REQUEST;
    *opt++ = 50;
    *opt++ = 4;
    write_be32(opt, g_dhcp_offer_addr);
    opt += 4;
    if (g_dhcp_server_addr != 0) {
        *opt++ = 54;
        *opt++ = 4;
        write_be32(opt, g_dhcp_server_addr);
        opt += 4;
    }
    *opt++ = 55;
    *opt++ = 8;
    *opt++ = 1;
    *opt++ = 3;
    *opt++ = 6;
    *opt++ = 15;
    *opt++ = 28;
    *opt++ = 51;
    *opt++ = 54;
    *opt++ = 58;
    *opt++ = 57;
    *opt++ = 2;
    write_be16(opt, 576);
    opt += 2;
    *opt++ = 12;
    *opt++ = 7;
    *opt++ = 'p';
    *opt++ = 'a';
    *opt++ = 'c';
    *opt++ = 'h';
    *opt++ = 'a';
    *opt++ = 'o';
    *opt++ = 's';
    *opt++ = 255;
    while ((u32)(opt - dhcp) < 300) *opt++ = 0;

    const u16 dhcp_len = (u16)(opt - dhcp);
    const u16 udp_len = (u16)(8 + dhcp_len);
    const u16 ip_len = (u16)(20 + udp_len);
    const u16 frame_len = (u16)(ETH_HDR_BYTES + ip_len);

    ip[0] = 0x45;
    ip[1] = 0;
    write_be16(ip + 2, ip_len);
    write_be16(ip + 4, 2);
    write_be16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = IP_PROTO_UDP;
    write_be32(ip + 12, 0);
    write_be32(ip + 16, 0xFFFFFFFF);
    write_be16(ip + 10, ipv4_checksum(ip, 20));

    write_be16(udp + 0, DHCP_CLIENT_PORT);
    write_be16(udp + 2, DHCP_SERVER_PORT);
    write_be16(udp + 4, udp_len);
    write_be16(udp + 6, 0);
    write_be16(udp + 6, udp_ipv4_checksum(0, 0xFFFFFFFF, udp, udp_len));

    if (!send_packet(frame, frame_len)) {
        user_log("[virtio_net] VirtioNet: dhcp request failed\n");
        return 0;
    }
    g_dhcp_request_sent = 1;
    user_log("[virtio_net] VirtioNet: dhcp request sent\n");
    return 1;
}

static int send_arp_request(u32 source_ip, u32 target_ip) {
    u8 frame[ETH_HDR_BYTES + 28];
    memset(frame, 0, sizeof(frame));
    for (u32 i = 0; i < 6; i++) frame[i] = 0xFF;
    memcpy(frame + 6, g_mac, 6);
    write_be16(frame + 12, ETHERTYPE_ARP);

    u8 *arp = frame + ETH_HDR_BYTES;
    write_be16(arp + 0, 1);
    write_be16(arp + 2, ETHERTYPE_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    write_be16(arp + 6, 1);
    memcpy(arp + 8, g_mac, 6);
    write_be32(arp + 14, source_ip);
    write_be32(arp + 24, target_ip);

    if (!send_packet(frame, sizeof(frame))) {
        user_log("[virtio_net] VirtioNet: arp request failed\n");
        return 0;
    }
    g_arp_sent = 1;
    user_log("[virtio_net] VirtioNet: arp request sent\n");
    return 1;
}

static u64 read_device_features(u32 select) {
    mmio_write_u32(g_common_base + COMMON_DEVICE_FEATURE_SELECT, select);
    return mmio_read_u32(g_common_base + COMMON_DEVICE_FEATURE);
}

static int negotiate_features(void) {
    const u32 device_features_low = (u32)read_device_features(0);
    const u32 device_features_high = (u32)read_device_features(1);
    u32 driver_features_low = 0;
    u32 driver_features_high = 0;
    if ((device_features_low & (1U << VIRTIO_NET_F_MAC)) != 0) {
        driver_features_low |= 1U << VIRTIO_NET_F_MAC;
    }
    if ((device_features_high & (1U << (VIRTIO_F_VERSION_1 - 32))) != 0) {
        driver_features_high |= 1U << (VIRTIO_F_VERSION_1 - 32);
    }
    mmio_write_u32(g_common_base + COMMON_DRIVER_FEATURE_SELECT, 0);
    mmio_write_u32(g_common_base + COMMON_DRIVER_FEATURE, driver_features_low);
    mmio_write_u32(g_common_base + COMMON_DRIVER_FEATURE_SELECT, 1);
    mmio_write_u32(g_common_base + COMMON_DRIVER_FEATURE, driver_features_high);
    mmio_write_u8(g_common_base + COMMON_DEVICE_STATUS, (u8)(mmio_read_u8(g_common_base + COMMON_DEVICE_STATUS) | STATUS_FEATURES_OK));
    if ((mmio_read_u8(g_common_base + COMMON_DEVICE_STATUS) & STATUS_FEATURES_OK) == 0) return 0;
    return 1;
}

static void read_mac(void) {
    if (g_device_base == 0) return;
    for (u64 i = 0; i < 6; i++) g_mac[i] = mmio_read_u8(g_device_base + i);
}

static int init_virtio(void) {
    while (map_mmio_page(VIRTIO_MMIO_BASE_VA, g_boot.common_page_paddr, 1) != SYSCALL_OK) wait_event(0, 1);
    while (map_mmio_page(VIRTIO_MMIO_BASE_VA + 0x1000, g_boot.notify_page_paddr, 1) != SYSCALL_OK) wait_event(0, 1);
    if (g_boot.isr_page_paddr != 0) {
        while (map_mmio_page(VIRTIO_MMIO_BASE_VA + 0x2000, g_boot.isr_page_paddr, 0) != SYSCALL_OK) wait_event(0, 1);
    }
    if (g_boot.device_page_paddr != 0) {
        while (map_mmio_page(VIRTIO_MMIO_BASE_VA + 0x3000, g_boot.device_page_paddr, 1) != SYSCALL_OK) wait_event(0, 1);
    }
    if (!init_queue_memory()) return 0;

    g_common_base = VIRTIO_MMIO_BASE_VA + g_boot.common_page_offset;
    g_notify_base = VIRTIO_MMIO_BASE_VA + 0x1000 + g_boot.notify_page_offset;
    g_isr_base = g_boot.isr_page_paddr != 0 ? VIRTIO_MMIO_BASE_VA + 0x2000 + g_boot.isr_page_offset : 0;
    g_device_base = g_boot.device_page_paddr != 0 ? VIRTIO_MMIO_BASE_VA + 0x3000 + g_boot.device_page_offset : 0;

    mmio_write_u8(g_common_base + COMMON_DEVICE_STATUS, 0);
    mmio_write_u8(g_common_base + COMMON_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
    if (!negotiate_features()) return 0;
    read_mac();
    if (!setup_queue(&g_rx_queue)) return 0;
    if (!setup_queue(&g_tx_queue)) return 0;
    if (!authorize_dma()) return 0;
    if (!prime_rx_queue()) return 0;
    mmio_write_u8(g_common_base + COMMON_DEVICE_STATUS, (u8)(mmio_read_u8(g_common_base + COMMON_DEVICE_STATUS) | STATUS_DRIVER_OK));
    return 1;
}

static int is_known_ethertype(u16 ethertype) {
    return ethertype == ETHERTYPE_IPV4 || ethertype == ETHERTYPE_ARP;
}

static void parse_dhcp_payload(const u8 *dhcp, u32 len) {
    if (len < 240) return;
    if (dhcp[0] != 2 || dhcp[1] != 1 || dhcp[2] != 6) return;
    if (read_be32(dhcp + 4) != g_dhcp_xid) return;
    if (read_be32(dhcp + 236) != DHCP_MAGIC_COOKIE) return;

    u8 msg_type = 0;
    u32 server_id = 0;
    u32 router = 0;
    u32 dns = 0;
    u32 cursor = 240;
    while (cursor < len) {
        const u8 tag = dhcp[cursor++];
        if (tag == 0) continue;
        if (tag == 255) break;
        if (cursor >= len) break;
        const u8 opt_len = dhcp[cursor++];
        if (cursor + opt_len > len) break;
        const u8 *value = dhcp + cursor;
        if (tag == 53 && opt_len >= 1) msg_type = value[0];
        if (tag == 54 && opt_len >= 4) server_id = read_be32(value);
        if (tag == 3 && opt_len >= 4) router = read_be32(value);
        if (tag == 6 && opt_len >= 4) dns = read_be32(value);
        cursor += opt_len;
    }

    if (msg_type == 0) return;
    if (server_id != 0) g_dhcp_server_addr = server_id;
    if (router != 0) g_gateway_addr = router;
    if (dns != 0) g_dns_addr = dns;
    if (msg_type == DHCP_MSG_OFFER || msg_type == DHCP_MSG_ACK) {
        const u32 yiaddr = read_be32(dhcp + 16);
        if (msg_type == DHCP_MSG_OFFER) g_dhcp_offer_addr = yiaddr;
        if (msg_type == DHCP_MSG_ACK) {
            g_ipv4_addr = yiaddr;
            g_dhcp_configured = 1;
        }
        log_dhcp(msg_type, yiaddr);
    }
}

static void parse_ipv4_packet(const u8 *frame, u32 frame_len) {
    if (frame_len < ETH_HDR_BYTES + 20) return;
    const u8 *ip = frame + ETH_HDR_BYTES;
    const u32 ihl = (u32)(ip[0] & 0x0F) * 4;
    if ((ip[0] >> 4) != 4 || ihl < 20) return;
    if (frame_len < ETH_HDR_BYTES + ihl) return;
    const u16 total_len = read_be16(ip + 2);
    if (total_len < ihl || frame_len < ETH_HDR_BYTES + total_len) return;
    if (ip[9] == IP_PROTO_TCP) {
        const u8 *tcp = ip + ihl;
        const u32 tcp_packet_len = (u32)total_len - ihl;
        if (tcp_packet_len < 20) return;
        const u16 src_port = read_be16(tcp + 0);
        const u16 dst_port = read_be16(tcp + 2);
        const u32 seq = read_be32(tcp + 4);
        const u32 ack_num = read_be32(tcp + 8);
        const u32 tcp_header_len = (u32)(tcp[12] >> 4) * 4;
        const u8 flags = tcp[13];
        if (tcp_header_len < 20 || tcp_header_len > tcp_packet_len) return;
        const u8 *payload = tcp + tcp_header_len;
        const u32 payload_len = tcp_packet_len - tcp_header_len;
        struct tcp_connection *conn = find_tcp_connection_by_tuple(read_be32(ip + 12), src_port, dst_port);
        if (conn == 0) return;
        if ((flags & TCP_FLAG_RST) != 0) {
            g_tcp_rx_rst++;
            conn->state = TCP_STATE_CLOSED;
            conn->peer_closed = 1;
            return;
        }
        if (conn->state == TCP_STATE_SYN_SENT && (flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
            g_tcp_rx_syn_ack++;
            if (ack_num == conn->seq) {
                conn->ack = seq + 1;
                conn->state = TCP_STATE_ESTABLISHED;
                g_tcp_connect_established++;
                tcp_send_or_defer_ack(conn);
            }
            return;
        }
        if (conn->state != TCP_STATE_ESTABLISHED && conn->state != TCP_STATE_FIN_WAIT) return;
        if (payload_len != 0) {
            g_tcp_rx_segments++;
            g_tcp_rx_payload_bytes += payload_len;
            const u32 payload_end = seq + payload_len;
            if (tcp_seq_leq(seq, conn->ack) && tcp_seq_lt(conn->ack, payload_end)) {
                const u32 skip = conn->ack - seq;
                const u32 append_len = payload_len - skip;
                if (tcp_rx_append_counted(conn, payload + skip, append_len)) {
                    conn->ack += append_len;
                    tcp_drain_ooo(conn);
                } else {
                    (void)tcp_store_ooo(conn, seq + skip, payload + skip, append_len);
                }
                tcp_send_or_defer_ack(conn);
            } else if (tcp_seq_leq(payload_end, conn->ack)) {
                g_tcp_rx_duplicate_segments++;
                tcp_send_or_defer_ack(conn);
            } else {
                g_tcp_rx_out_of_order_segments++;
                (void)tcp_store_ooo(conn, seq, payload, payload_len);
                tcp_send_or_defer_ack(conn);
            }
        }
        if ((flags & TCP_FLAG_FIN) != 0) {
            g_tcp_rx_fin++;
            const u32 fin_seq = seq + payload_len;
            if (fin_seq == conn->ack) {
                conn->ack++;
                conn->peer_closed = 1;
                conn->state = TCP_STATE_CLOSED;
            }
            tcp_send_or_defer_ack(conn);
        }
        return;
    }
    if (ip[9] != IP_PROTO_UDP) return;

    const u8 *udp = ip + ihl;
    const u32 udp_packet_len = (u32)total_len - ihl;
    if (udp_packet_len < 8) return;
    const u16 src_port = read_be16(udp + 0);
    const u16 dst_port = read_be16(udp + 2);
    const u16 udp_len = read_be16(udp + 4);
    if (udp_len < 8 || udp_len > udp_packet_len) return;

    if (g_rx_packets <= 16) {
        log_udp(read_be32(ip + 12), read_be32(ip + 16), src_port, dst_port);
    }
    if (src_port == DHCP_SERVER_PORT && dst_port == DHCP_CLIENT_PORT) {
        parse_dhcp_payload(udp + 8, (u32)udp_len - 8);
        return;
    }
    (void)enqueue_udp_packet(
        read_be32(ip + 12),
        src_port,
        dst_port,
        udp + 8,
        (u32)udp_len - 8
    );
}

static void parse_ethernet_frame(const u8 *frame, u32 frame_len) {
    if (frame_len < ETH_HDR_BYTES) return;
    const u16 ethertype = read_be16(frame + 12);
    if (g_rx_packets <= 16) log_rx_packet(frame_len, ethertype);
    if (ethertype == ETHERTYPE_ARP) {
        if (frame_len >= ETH_HDR_BYTES + 28) {
            const u8 *arp = frame + ETH_HDR_BYTES;
            const u16 op = read_be16(arp + 6);
            if (g_rx_packets <= 16) log_arp(op);
            if (op == 2 && read_be16(arp + 2) == ETHERTYPE_IPV4 && arp[4] == 6 && arp[5] == 4) {
                g_arp_reply_seen = 1;
                const u32 sender_ip = read_be32(arp + 14);
                if (sender_ip == g_gateway_addr) {
                    memcpy(g_gateway_mac, arp + 8, 6);
                    g_gateway_mac_ready = 1;
                }
                log_arp_reply(arp + 8, sender_ip);
            }
        }
        return;
    }
    if (ethertype == ETHERTYPE_IPV4) parse_ipv4_packet(frame, frame_len);
}

static int poll_rx_queue_budget(u32 budget) {
    int processed = 0;
    u32 count = 0;
    while (*used_idx_ptr(&g_rx_queue) != g_rx_queue.last_used_idx && count < budget) {
        volatile struct virtq_used_elem *used = &used_ring_ptr(&g_rx_queue)[g_rx_queue.last_used_idx % QUEUE_SIZE];
        const u16 desc_index = (u16)(used->id % QUEUE_SIZE);
        const u32 packet_len = used->len;
        g_rx_queue.last_used_idx++;
        g_rx_packets++;
        count++;
        processed = 1;
        if (packet_len > NET_HDR_BYTES) {
            const u8 *src = (const u8 *)(RX_BUFFER_BASE_VA + (u64)desc_index * 4096);
            u32 offset = NET_HDR_BYTES;
            if (packet_len >= NET_HDR_BYTES + ETH_HDR_BYTES) {
                u16 ethertype = read_be16(src + NET_HDR_BYTES + 12);
                if (!is_known_ethertype(ethertype) && packet_len >= 12 + ETH_HDR_BYTES) {
                    ethertype = read_be16(src + 12 + 12);
                    if (is_known_ethertype(ethertype)) offset = 12;
                }
            }
            parse_ethernet_frame(src + offset, packet_len - offset);
        }
        queue_push_avail(&g_rx_queue, desc_index);
    }
    if (processed && queue_notify(g_rx_queue.notify_token, g_rx_queue.index) == SYSCALL_OK) {
        mmio_write_u16(g_rx_queue.notify_addr, g_rx_queue.index);
    }
    if (g_isr_base != 0) (void)mmio_read_u8(g_isr_base);
    return processed;
}

static int poll_tx_queue(void) {
    int processed = 0;
    while (*used_idx_ptr(&g_tx_queue) != g_tx_queue.last_used_idx) {
        g_tx_in_flight = 0;
        g_tx_completions++;
        processed = 1;
        if (!g_tx_complete_logged) {
            user_log("[virtio_net] VirtioNet: tx complete\n");
            g_tx_complete_logged = 1;
        }
        g_tx_queue.last_used_idx++;
        if (g_dhcp_offer_addr != 0 && !g_dhcp_request_sent) {
            send_dhcp_request();
        } else if (g_dhcp_sent && !g_arp_sent) {
            const u32 source_ip = g_ipv4_addr != 0 ? g_ipv4_addr : 0x0A00020F;
            const u32 target_ip = g_gateway_addr != 0 ? g_gateway_addr : 0x0A000202;
            send_arp_request(source_ip, target_ip);
        }
    }
    if (!g_tx_in_flight && g_dhcp_offer_addr != 0 && !g_dhcp_request_sent) {
        if (send_dhcp_request()) processed = 1;
    }
    if (flush_pending_tcp_acks()) processed = 1;
    return processed;
}

static struct net_session *find_or_alloc_net_session(u64 request_paddr, u64 request_token, u64 *session_index_out) {
    for (u64 i = 0; i < NET_SESSION_MAX; i++) {
        if (g_sessions[i].active &&
            ((request_token != 0 && g_sessions[i].request_token == request_token) ||
             (request_token == 0 && g_sessions[i].request_paddr == request_paddr)))
        {
            *session_index_out = i;
            return &g_sessions[i];
        }
    }
    for (u64 i = 0; i < NET_SESSION_MAX; i++) {
        if (!g_sessions[i].active) {
            *session_index_out = i;
            return &g_sessions[i];
        }
    }
    return 0;
}

static int finish_net_connect(
    volatile struct net_request_header *request,
    u64 request_va,
    u64 response_va,
    u64 request_paddr,
    u64 response_paddr,
    u64 request_token,
    u64 response_token
) {
    memset((void *)response_va, 0, 4096);
    u64 session_index = 0;
    struct net_session *session = find_or_alloc_net_session(request_paddr, request_token, &session_index);
    if (session == 0) {
        user_log("[virtio_net] VirtioNet: session table full\n");
        return 0;
    }
    session->active = 1;
    session->request_paddr = request_paddr;
    session->response_paddr = response_paddr;
    session->request_token = request_token;
    session->response_token = response_token;
    session->request_va = request_va;
    session->response_va = response_va;
    const u64 reply_endpoint_id = NET_REPLY_ENDPOINT_ID + session_index;
    session->reply_endpoint_id = install_endpoint(reply_endpoint_id, request->arg1) == SYSCALL_OK ? reply_endpoint_id : 0;
    session->session_nonce = request->session_nonce;
    session->last_completed_seq = 0;
    g_current_session = session;
    write_net_response(NET_OP_CONNECT, request->request_seq, NET_STATUS_OK, 0, 0, 0, 0);
    session->last_completed_seq = request->request_seq;
    g_current_session = 0;
    return 1;
}

static int handle_net_connect_token(u64 request_token) {
    const u64 request_va = map_ipc_buffer_anywhere(request_token, 0);
    if (request_va < 0x1000) return 0;
    volatile struct net_request_header *request = (volatile struct net_request_header *)request_va;
    if (request->magic != NET_PROTOCOL_REQUEST_MAGIC ||
        request->version != NET_PROTOCOL_VERSION ||
        request->op != NET_OP_CONNECT ||
        request->request_seq == 0 ||
        !is_ipc_buffer_token(request->arg0) ||
        request->session_nonce == 0)
    {
        user_log("[virtio_net] VirtioNet: invalid ipc-buffer connect request\n");
        return 0;
    }
    const u64 response_token = request->arg0;
    const u64 response_va = map_ipc_buffer_anywhere(response_token, 1);
    if (response_va < 0x1000) return 0;
    if (!finish_net_connect(request, request_va, response_va, 0, 0, request_token, response_token)) return 0;
    user_log("[virtio_net] VirtioNet: ipc-buffer session connect ok\n");
    return 1;
}

static void handle_net_connect_paddr_transfer(u64 transfer_id) {
    const u64 request_paddr = accept_cap_transfer(transfer_id);
    if (request_paddr < 0x1000) {
        user_log("[virtio_net] VirtioNet: accept cap transfer failed\n");
        return;
    }
    const u64 request_va = map_page_anywhere(request_paddr, 0);
    if (request_va < 0x1000) return;
    volatile struct net_request_header *request = (volatile struct net_request_header *)request_va;
    if (request->magic != NET_PROTOCOL_REQUEST_MAGIC ||
        request->version != NET_PROTOCOL_VERSION ||
        request->op != NET_OP_CONNECT ||
        request->request_seq == 0 ||
        request->arg0 < 0x1000 ||
        request->session_nonce == 0)
    {
        user_log("[virtio_net] VirtioNet: invalid connect request\n");
        return;
    }
    const u64 response_va = map_page_anywhere(request->arg0, 1);
    if (response_va < 0x1000) return;
    if (!finish_net_connect(request, request_va, response_va, request_paddr, request->arg0, 0, 0)) return;
    user_log("[virtio_net] VirtioNet: session connect ok\n");
}

static void handle_net_connect_transfer(u64 transfer_id) {
    const u64 request_token = accept_ipc_buffer_transfer(transfer_id);
    if (is_ipc_buffer_token(request_token) && handle_net_connect_token(request_token)) return;
    handle_net_connect_paddr_transfer(transfer_id);
}

static int handle_net_request_for_session(struct net_session *session) {
    if (session == 0 || !session->active) return 0;
    g_current_session = session;
    volatile struct net_request_header *request = (volatile struct net_request_header *)session->request_va;
    if (request->magic != NET_PROTOCOL_REQUEST_MAGIC || request->version != NET_PROTOCOL_VERSION) {
        g_current_session = 0;
        return 0;
    }
    const u64 seq = request->request_seq;
    if (seq == 0 || seq <= session->last_completed_seq) {
        g_current_session = 0;
        return 0;
    }
    if (request->session_nonce != session->session_nonce) {
        g_current_session = 0;
        return 0;
    }

    g_net_service_requests++;
    if (request->op == NET_OP_GET_STATUS) {
        write_net_status_payload();
        write_net_response(
            NET_OP_GET_STATUS,
            seq,
            NET_STATUS_OK,
            sizeof(struct net_status_payload),
            g_ipv4_addr,
            g_gateway_addr,
            g_dns_addr
        );
    } else if (request->op == NET_OP_CONNECT) {
        write_net_response(NET_OP_CONNECT, seq, NET_STATUS_OK, 0, 0, 0, 0);
    } else if (request->op == NET_OP_BIND) {
        const u16 requested_port = (u16)request->arg0;
        if (requested_port != 0 && udp_port_in_use(requested_port)) {
            write_net_response(NET_OP_BIND, seq, NET_STATUS_PORT_IN_USE, 0, 0, requested_port, 0);
        } else {
            struct udp_binding *binding = alloc_udp_binding(requested_port);
            if (binding == 0) {
                write_net_response(NET_OP_BIND, seq, NET_STATUS_PORT_IN_USE, 0, 0, requested_port, 0);
            } else {
                write_net_response(NET_OP_BIND, seq, NET_STATUS_OK, 0, binding->handle, binding->local_port, g_ipv4_addr);
            }
        }
    } else if (request->op == NET_OP_SEND_TO) {
        const u32 payload_len = (u32)request->reserved0;
        int status = NET_STATUS_OK;
        if (payload_len > NET_PAYLOAD_BYTES) {
            status = NET_STATUS_TOO_BIG;
        } else {
            status = send_ipv4_udp(
                request->arg0,
                (u32)request->arg1,
                (u16)request->arg2,
                net_request_payload(),
                payload_len
            );
        }
        write_net_response(NET_OP_SEND_TO, seq, status, 0, status == NET_STATUS_OK ? payload_len : 0, 0, 0);
    } else if (request->op == NET_OP_RECV_FROM) {
        u32 out_len = 0;
        u32 src_ip = 0;
        u16 src_port = 0;
        u32 out_cap = (u32)request->reserved0;
        if (out_cap > NET_PAYLOAD_BYTES) out_cap = NET_PAYLOAD_BYTES;
        const int status = dequeue_udp_packet(request->arg0, net_response_payload(), out_cap, &out_len, &src_ip, &src_port);
        write_net_response(NET_OP_RECV_FROM, seq, status, status == NET_STATUS_OK ? out_len : 0, src_ip, src_port, out_len);
    } else if (request->op == NET_OP_CLOSE) {
        int ok = close_udp_binding(request->arg0);
        if (!ok) ok = close_tcp_connection(request->arg0);
        write_net_response(NET_OP_CLOSE, seq, ok ? NET_STATUS_OK : NET_STATUS_INVALID, 0, 0, 0, 0);
    } else if (request->op == NET_OP_POLL) {
        u64 events = 0;
        int status = poll_udp_binding(request->arg0, &events);
        if (status == NET_STATUS_INVALID) status = poll_tcp_connection(request->arg0, &events);
        write_net_response(NET_OP_POLL, seq, status, 0, events, 0, 0);
    } else if (request->op == NET_OP_TCP_CONNECT) {
        u64 handle = 0;
        u16 local_port = 0;
        u32 local_ip = 0;
        const int status = net_tcp_connect((u32)request->arg0, (u16)request->arg1, &handle, &local_port, &local_ip);
        write_net_response(NET_OP_TCP_CONNECT, seq, status, 0, handle, local_port, local_ip);
    } else if (request->op == NET_OP_TCP_WRITE) {
        const u32 payload_len = (u32)request->reserved0;
        u32 written = 0;
        int status = NET_STATUS_OK;
        if (payload_len > NET_PAYLOAD_BYTES || payload_len > NET_TCP_MAX_PAYLOAD) {
            status = NET_STATUS_TOO_BIG;
        } else {
            status = tcp_write(request->arg0, net_request_payload(), payload_len, &written);
        }
        write_net_response(NET_OP_TCP_WRITE, seq, status, 0, written, 0, 0);
    } else if (request->op == NET_OP_TCP_READ) {
        u32 out_len = 0;
        u32 out_cap = (u32)request->reserved0;
        if (out_cap > NET_PAYLOAD_BYTES) out_cap = NET_PAYLOAD_BYTES;
        const int status = tcp_read(request->arg0, net_response_payload(), out_cap, &out_len);
        write_net_response(NET_OP_TCP_READ, seq, status, status == NET_STATUS_OK ? out_len : 0, out_len, 0, 0);
    } else {
        write_net_response(request->op, seq, NET_STATUS_INVALID, 0, 0, 0, 0);
    }
    session->last_completed_seq = seq;
    g_current_session = 0;
    return 1;
}

static int handle_net_requests(void) {
    int processed = 0;
    for (u64 i = 0; i < NET_SESSION_MAX; i++) {
        if (handle_net_request_for_session(&g_sessions[i])) processed = 1;
    }
    return processed;
}

void virtio_net_main(void) {
    user_log("[virtio_net] VirtioNet: started\n");
    wait_for_boot_resources();
    if (!init_virtio()) {
        cfg_write(NET_DRIVER_STATUS_INDEX, NET_STATUS_FAILED);
        if (g_common_base != 0) {
            mmio_write_u8(g_common_base + COMMON_DEVICE_STATUS, (u8)(mmio_read_u8(g_common_base + COMMON_DEVICE_STATUS) | STATUS_FAILED));
        }
        user_log("[virtio_net] VirtioNet: init failed\n");
        for (;;) wait_event(0, 1);
    }
    cfg_write(NET_DRIVER_STATUS_INDEX, NET_STATUS_READY);
    log_mac();
    user_log("[virtio_net] VirtioNet: queue ready\n");
    send_dhcp_discover();

    for (;;) {
        int did_work = 0;
        did_work |= handle_net_requests();
        did_work |= poll_tx_queue();
        did_work |= poll_rx_queue_budget(8);
        did_work |= handle_net_requests();
        if (did_work) {
            g_net_service_work_loops++;
            continue;
        }
        g_net_service_idle_sleeps++;
        const u64 received = wait_event(1, 1);
        if (received >= CAP_TRANSFER_ID_MIN) handle_net_connect_transfer(received);
    }
}

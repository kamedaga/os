static u16 read_net_be16(const u8 *p) {
    return (u16)(((u16)p[0] << 8) | p[1]);
}

static u16 read_net_le16(const u8 *p) {
    return (u16)(((u16)p[1] << 8) | p[0]);
}

struct linux_pollfd {
    i32 fd;
    short events;
    short revents;
};

struct linux_msghdr64 {
    u64 msg_name;
    u32 msg_namelen;
    u32 __pad0;
    u64 msg_iov;
    u64 msg_iovlen;
    u64 msg_control;
    u64 msg_controllen;
    i32 msg_flags;
    u32 __pad1;
};

static u64 socket_write_from_target(u64 fd, u64 src_va, u64 len);
static u64 socket_read_to_target(u64 fd, u64 dst_va, u64 len);
static u64 socket_send_payload(u64 fd, const u8 *payload, u64 len);
static u64 socket_send_iov_from_target(u64 fd, u64 iov_va, u64 iovcnt);
static u8 g_net_io_payload[NET_TCP_READ_BYTES];

enum {
    UDP_NONBLOCK_RECV_GRACE_LOOPS = 256,
    UDP_NONBLOCK_SEND_GRACE_LOOPS = 256,
};

static int fd_is_nonblock(u64 fd) {
    return fd_valid(fd) && (g_fds[fd].fd_flags & O_NONBLOCK) != 0;
}

static void log_socket_error(const char *label, u64 fd, u64 result) {
    if (result == errno_again()) return;
    user_log(label);
    user_log(" fd=");
    user_log_hex_value(fd);
    user_log("LinuxAbiServer: socket result=");
    user_log_hex_value(result);
}

static u32 read_net_be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void write_net_be16(u8 *p, u16 value) {
    p[0] = (u8)(value >> 8);
    p[1] = (u8)value;
}

static void write_net_be32(u8 *p, u32 value) {
    p[0] = (u8)(value >> 24);
    p[1] = (u8)(value >> 16);
    p[2] = (u8)(value >> 8);
    p[3] = (u8)value;
}

static void write_net_le16(u8 *p, u16 value) {
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
}

static u64 make_net_nonce(u64 request_paddr, u64 response_paddr, u64 endpoint_id, u64 process_slot) {
    u64 nonce = request_paddr ^ ((response_paddr << 17) | (response_paddr >> 47)) ^ ((endpoint_id << 7) | (endpoint_id >> 57)) ^ process_slot ^ 0x6e65742d73746174ULL;
    return nonce == 0 ? 1 : nonce;
}

static int install_net_endpoint(void) {
    if (g_net.endpoint_id == 0 || g_net.process_handle == 0) return 0;
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, g_net.endpoint_id, g_net.process_handle) == SYSCALL_OK;
}

static int grant_net_response_page(void) {
    u64 ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, g_net.response_paddr, g_net.endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_net_endpoint()) ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, g_net.response_paddr, g_net.endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE);
    return ret == SYSCALL_OK;
}

static int share_net_request_page(void) {
    u64 ret = syscall2(SYSCALL_SHARE_CAP, g_net.request_paddr, g_net.endpoint_id);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_net_endpoint()) ret = syscall2(SYSCALL_SHARE_CAP, g_net.request_paddr, g_net.endpoint_id);
    return ret == SYSCALL_OK;
}

static u64 signal_net_status(void) {
    u64 ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_net.endpoint_id, 0);
    if (ret == SYSCALL_OK) return ret;
    if (ret == SYSCALL_ERR_ENDPOINT && install_net_endpoint()) ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_net.endpoint_id, 0);
    return ret;
}

static int wait_net_response(u64 expected_seq, u16 expected_op, u64 poll_limit) {
    volatile struct net_response_header *response = (volatile struct net_response_header *)NET_RESPONSE_VA;
    g_prof.net_wait_calls++;
    for (u64 i = 0; poll_limit == 0 || i < poll_limit; i++) {
        if (response->response_seq == expected_seq) {
            g_prof.net_wait_loops += i;
            if (i > 8) g_prof.net_wait_slow++;
            return response->magic == NET_RESPONSE_MAGIC &&
                response->version == NET_PROTOCOL_VERSION &&
                response->op == expected_op;
        }
        wait_without_consuming_ipc();
    }
    g_prof.net_wait_loops += poll_limit;
    g_prof.net_wait_timeouts++;
    return 0;
}

static int connect_net_from_registry(void) {
    struct service_entry entry;
    if (!find_service(SERVICE_KIND_NET, &entry)) return 0;
    g_net.endpoint_id = entry.endpoint_id;
    g_net.process_handle = entry.process_handle;
    const int have_pages = g_net.request_paddr >= 0x1000 && g_net.response_paddr >= 0x1000;
    if (!have_pages) {
        g_net.request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
        g_net.response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
        if (g_net.request_paddr < 0x1000 || g_net.response_paddr < 0x1000) return 0;
        if (syscall3(SYSCALL_MAP_PAGE, NET_REQUEST_VA, g_net.request_paddr, 1) != SYSCALL_OK) return 0;
        if (syscall3(SYSCALL_MAP_PAGE, NET_RESPONSE_VA, g_net.response_paddr, 1) != SYSCALL_OK) return 0;
    }
    if (!grant_net_response_page()) return 0;
    clear_page(NET_REQUEST_VA);
    clear_page(NET_RESPONSE_VA);

    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_HANDLE);
    g_net.session_nonce = make_net_nonce(g_net.request_paddr, g_net.response_paddr, g_net.endpoint_id, self_slot);
    volatile struct net_request_header *request = (volatile struct net_request_header *)NET_REQUEST_VA;
    const u64 connect_seq = g_net.next_seq != 0 ? g_net.next_seq++ : 1;
    request->magic = NET_REQUEST_MAGIC;
    request->version = NET_PROTOCOL_VERSION;
    request->op = NET_OP_CONNECT;
    request->session_nonce = g_net.session_nonce;
    request->arg0 = g_net.response_paddr;
    request->arg1 = self_slot;
    __asm__ volatile("" ::: "memory");
    request->request_seq = connect_seq;
    if (!share_net_request_page()) return 0;
    if (!wait_net_response(connect_seq, NET_OP_CONNECT, 8192)) return 0;
    volatile struct net_response_header *response = (volatile struct net_response_header *)NET_RESPONSE_VA;
    if (response->status != NET_STATUS_OK) return 0;
    if (g_net.next_seq <= connect_seq) g_net.next_seq = connect_seq + 1;
    g_net.active = 1;
    user_log("LinuxAbiServer: net connect ok\n");
    return 1;
}

static int ensure_net_connected(void) {
    if (g_net.active) return 1;
    return connect_net_from_registry();
}

static int net_begin_request(u16 op, u64 arg0, u64 arg1, u64 arg2, u64 reserved0, const u8 *payload, u32 payload_len, u64 *seq_out) {
    if (payload_len > NET_REQUEST_PAYLOAD_BYTES) return 0;
    for (u64 attempt = 0; attempt < 2; attempt++) {
        if (!ensure_net_connected()) return 0;
        g_prof.net_requests++;
        if (op < NET_PROFILE_OP_COUNT) g_prof.net_op_counts[op]++;
        if (op == NET_OP_SEND_TO || op == NET_OP_TCP_WRITE) g_prof.net_payload_tx_bytes += payload_len;
        clear_page(NET_REQUEST_VA);
        clear_page(NET_RESPONSE_VA);
        volatile struct net_request_header *request = (volatile struct net_request_header *)NET_REQUEST_VA;
        const u64 seq = g_net.next_seq++;
        request->magic = NET_REQUEST_MAGIC;
        request->version = NET_PROTOCOL_VERSION;
        request->op = op;
        request->session_nonce = g_net.session_nonce;
        request->arg0 = arg0;
        request->arg1 = arg1;
        request->arg2 = arg2;
        request->reserved0 = reserved0;
        if (payload_len != 0 && payload != 0) {
            volatile u8 *dst = (volatile u8 *)(NET_REQUEST_VA + NET_REQUEST_HEADER_BYTES);
            for (u32 i = 0; i < payload_len; i++) dst[i] = payload[i];
        }
        __asm__ volatile("" ::: "memory");
        request->request_seq = seq;
        const u64 signal_status = signal_net_status();
        if (signal_status == SYSCALL_OK) {
            *seq_out = seq;
            return 1;
        }
        user_log("LinuxAbiServer: net signal failed status=");
        user_log_hex_value(signal_status);
        g_net.active = 0;
        if (!connect_net_from_registry()) return 0;
    }
    return 0;
}

static u64 net_status_to_errno(i32 status) {
    if (status == NET_STATUS_INVALID) return errno_inval();
    if (status == NET_STATUS_NOT_CONNECTED) return errno_netunreach();
    if (status == NET_STATUS_NO_ROUTE) return errno_netunreach();
    if (status == NET_STATUS_PORT_IN_USE) return errno_addrinuse();
    if (status == NET_STATUS_WOULD_BLOCK) return errno_again();
    if (status == NET_STATUS_TOO_BIG) return errno_msgsize();
    if (status == NET_STATUS_BUSY) return errno_again();
    return errno_io();
}

static int net_bind_udp(u16 local_port, u64 *handle_out, u16 *actual_port_out, u32 *local_ip_out) {
    u64 seq = 0;
    if (!net_begin_request(NET_OP_BIND, local_port, 0, 0, 0, 0, 0, &seq)) return 0;
    if (!wait_net_response(seq, NET_OP_BIND, 8192)) return 0;
    volatile struct net_response_header *response = (volatile struct net_response_header *)NET_RESPONSE_VA;
    if (response->status != NET_STATUS_OK) return 0;
    *handle_out = response->arg0;
    if (actual_port_out != 0) *actual_port_out = (u16)response->arg1;
    if (local_ip_out != 0) *local_ip_out = (u32)response->arg2;
    return *handle_out != 0;
}

static u64 net_send_udp(u64 handle, u32 remote_ip, u16 remote_port, const u8 *payload, u32 payload_len) {
    u64 seq = 0;
    if (!net_begin_request(NET_OP_SEND_TO, handle, remote_ip, remote_port, payload_len, payload, payload_len, &seq)) return errno_io();
    if (!wait_net_response(seq, NET_OP_SEND_TO, 8192)) return errno_timedout();
    volatile struct net_response_header *response = (volatile struct net_response_header *)NET_RESPONSE_VA;
    return response->status == NET_STATUS_OK ? (u64)payload_len : net_status_to_errno(response->status);
}

static u64 net_recv_udp(u64 handle, u8 *out, u32 out_cap, u32 *src_ip, u16 *src_port) {
    u64 seq = 0;
    if (!net_begin_request(NET_OP_RECV_FROM, handle, 0, 0, out_cap, 0, 0, &seq)) return errno_io();
    if (!wait_net_response(seq, NET_OP_RECV_FROM, 8192)) return errno_timedout();
    volatile struct net_response_header *response = (volatile struct net_response_header *)NET_RESPONSE_VA;
    if (response->status != NET_STATUS_OK) return net_status_to_errno(response->status);
    if (response->inline_bytes > out_cap || response->inline_bytes > NET_RESPONSE_PAYLOAD_BYTES) return errno_io();
    volatile u8 *src = (volatile u8 *)(NET_RESPONSE_VA + NET_RESPONSE_HEADER_BYTES);
    for (u32 i = 0; i < response->inline_bytes; i++) out[i] = src[i];
    g_prof.net_payload_rx_bytes += response->inline_bytes;
    *src_ip = (u32)response->arg0;
    *src_port = (u16)response->arg1;
    return response->inline_bytes;
}

static u64 net_poll_udp(u64 handle) {
    u64 last_error = errno_io();
    for (u64 attempt = 0; attempt < 8; attempt++) {
        u64 seq = 0;
        if (!net_begin_request(NET_OP_POLL, handle, 0, 0, 0, 0, 0, &seq)) {
            last_error = errno_io();
        } else if (!wait_net_response(seq, NET_OP_POLL, 8192)) {
            last_error = errno_timedout();
        } else {
            volatile struct net_response_header *response = (volatile struct net_response_header *)NET_RESPONSE_VA;
            if (response->status == NET_STATUS_OK) return response->arg0;
            last_error = net_status_to_errno(response->status);
            if (last_error != errno_io() && last_error != errno_timedout()) return last_error;
        }
        wait_without_consuming_ipc();
    }
    user_log("LinuxAbiServer: net poll retry exhausted handle=");
    user_log_hex_value(handle);
    user_log("LinuxAbiServer: net poll last=");
    user_log_hex_value(last_error);
    return last_error;
}

static u64 net_tcp_begin_connect(u32 remote_ip, u16 remote_port, u64 *handle_out, u16 *actual_port_out, u32 *local_ip_out) {
    u64 result = errno_netunreach();
    for (u64 attempt = 0; attempt < 8192; attempt++) {
        g_prof.net_tcp_connect_attempts++;
        u64 seq = 0;
        if (!net_begin_request(NET_OP_TCP_CONNECT, remote_ip, remote_port, 0, 0, 0, 0, &seq)) return errno_io();
        if (!wait_net_response(seq, NET_OP_TCP_CONNECT, 8192)) return errno_timedout();
        volatile struct net_response_header *response = (volatile struct net_response_header *)NET_RESPONSE_VA;
        if (response->status == NET_STATUS_OK) {
            *handle_out = response->arg0;
            if (actual_port_out != 0) *actual_port_out = (u16)response->arg1;
            if (local_ip_out != 0) *local_ip_out = (u32)response->arg2;
            break;
        }
        result = net_status_to_errno(response->status);
        if (response->status != NET_STATUS_BUSY && response->status != NET_STATUS_NO_ROUTE) return result;
        wait_without_consuming_ipc();
    }
    if (*handle_out == 0) return result;
    return 0;
}

static u64 net_tcp_wait_connected(u64 handle) {
    for (u64 attempt = 0; attempt < 8192; attempt++) {
        g_prof.net_tcp_connect_poll_loops++;
        const u64 events = net_poll_udp(handle);
        if ((i64)events < 0) return events;
        if ((events & NET_POLL_WRITABLE) != 0) return 0;
        wait_without_consuming_ipc();
    }
    return errno_timedout();
}

static u64 net_tcp_write(u64 handle, const u8 *payload, u32 payload_len) {
    u64 seq = 0;
    if (!net_begin_request(NET_OP_TCP_WRITE, handle, 0, 0, payload_len, payload, payload_len, &seq)) return errno_io();
    if (!wait_net_response(seq, NET_OP_TCP_WRITE, 8192)) return errno_timedout();
    volatile struct net_response_header *response = (volatile struct net_response_header *)NET_RESPONSE_VA;
    return response->status == NET_STATUS_OK ? response->arg0 : net_status_to_errno(response->status);
}

static u64 net_tcp_read(u64 handle, u8 *out, u32 out_cap) {
    u64 seq = 0;
    if (!net_begin_request(NET_OP_TCP_READ, handle, 0, 0, out_cap, 0, 0, &seq)) return errno_io();
    if (!wait_net_response(seq, NET_OP_TCP_READ, 8192)) return errno_timedout();
    volatile struct net_response_header *response = (volatile struct net_response_header *)NET_RESPONSE_VA;
    if (response->status != NET_STATUS_OK) return net_status_to_errno(response->status);
    if (response->inline_bytes > out_cap || response->inline_bytes > NET_RESPONSE_PAYLOAD_BYTES) return errno_io();
    volatile u8 *src = (volatile u8 *)(NET_RESPONSE_VA + NET_RESPONSE_HEADER_BYTES);
    for (u32 i = 0; i < response->inline_bytes; i++) out[i] = src[i];
    g_prof.net_payload_rx_bytes += response->inline_bytes;
    return response->inline_bytes;
}

static void net_close_udp(u64 handle) {
    if (handle == 0 || !g_net.active) return;
    u64 seq = 0;
    if (!net_begin_request(NET_OP_CLOSE, handle, 0, 0, 0, 0, 0, &seq)) return;
    (void)wait_net_response(seq, NET_OP_CLOSE, 8192);
}

static struct ipc_message handle_socket(const struct trap_request *req) {
    const u64 domain = req->args[0];
    const u64 type = req->args[1];
    const u64 protocol = req->args[2];
    if (domain != AF_INET) return reply(errno_afnosupport(), 0);
    const u64 socket_type = type & SOCK_TYPE_MASK;
    if (socket_type != SOCK_DGRAM && socket_type != SOCK_STREAM) return reply(errno_socktnosupport(), 0);
    if (socket_type == SOCK_DGRAM && protocol != 0 && protocol != IPPROTO_UDP) return reply(errno_protonosupport(), 0);
    if (socket_type == SOCK_STREAM && protocol != 0 && protocol != IPPROTO_TCP) return reply(errno_protonosupport(), 0);
    if (!ensure_net_connected()) return reply(errno_netunreach(), 0);
    const int fd = alloc_fd();
    if (fd < 0) return reply(errno_busy(), 0);
    g_fds[(u64)fd].kind = FD_SOCKET;
    g_fds[(u64)fd].token = 0;
    g_fds[(u64)fd].offset = 0;
    g_fds[(u64)fd].size = 0;
    g_fds[(u64)fd].mode_bits = 0;
    g_fds[(u64)fd].fd_flags = (u32)((type & SOCK_NONBLOCK) != 0 ? O_NONBLOCK : 0);
    g_fds[(u64)fd].desc_flags = (u32)((type & SOCK_CLOEXEC) != 0 ? FD_CLOEXEC : 0);
    g_fds[(u64)fd].object_kind = FS_OBJECT_FILE;
    g_fds[(u64)fd].pipe_id = 0;
    g_fds[(u64)fd].socket_connected = 0;
    g_fds[(u64)fd].socket_type = (u8)socket_type;
    g_fds[(u64)fd].socket_connecting = 0;
    g_fds[(u64)fd].socket_reserved0 = 0;
    g_fds[(u64)fd].socket_local_port = 0;
    g_fds[(u64)fd].socket_remote_port = 0;
    g_fds[(u64)fd].socket_local_ip = 0;
    g_fds[(u64)fd].socket_remote_ip = 0;
    g_fds[(u64)fd].path_len = 0;
    g_fds[(u64)fd].path[0] = 0;
    sync_fd_to_thread_group((u64)fd);
    return reply((u64)fd, 0);
}

static int read_sockaddr_in(u64 addr_va, u64 addr_len, u32 *ip_out, u16 *port_out) {
    if (addr_va == 0 || addr_len < 16) return 0;
    u8 raw[16];
    if (copy_from_target(addr_va, raw, sizeof(raw)) != sizeof(raw)) return 0;
    if (read_net_le16(raw) != AF_INET) return 0;
    *port_out = read_net_be16(raw + 2);
    *ip_out = read_net_be32(raw + 4);
    return 1;
}

static int write_sockaddr_in_to_target(u64 addr_va, u64 addrlen_va, u32 ip, u16 port) {
    if (addr_va == 0 || addrlen_va == 0) return 0;
    u32 addrlen = 0;
    if (copy_from_target(addrlen_va, &addrlen, sizeof(addrlen)) != sizeof(addrlen)) return 0;
    if (addrlen < 16) return 0;
    u8 raw[16];
    for (u64 i = 0; i < sizeof(raw); i++) raw[i] = 0;
    write_net_le16(raw, AF_INET);
    write_net_be16(raw + 2, port);
    write_net_be32(raw + 4, ip);
    addrlen = 16;
    return copy_to_target(addr_va, raw, sizeof(raw)) == sizeof(raw) &&
        copy_to_target(addrlen_va, &addrlen, sizeof(addrlen)) == sizeof(addrlen);
}

static int ensure_socket_bound(u64 fd) {
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return 0;
    if (g_fds[fd].token != 0) return 1;
    if (g_fds[fd].socket_type != SOCK_DGRAM) return 0;
    u64 handle = 0;
    u16 local_port = 0;
    u32 local_ip = 0;
    if (!net_bind_udp(0, &handle, &local_port, &local_ip)) return 0;
    g_fds[fd].token = handle;
    g_fds[fd].socket_local_port = local_port;
    g_fds[fd].socket_local_ip = local_ip;
    sync_fd_to_thread_group(fd);
    return 1;
}

static u64 socket_stream_poll_connect(u64 fd, int *ready_out) {
    *ready_out = 0;
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET || g_fds[fd].socket_type != SOCK_STREAM) return errno_badf();
    if (g_fds[fd].socket_connected) {
        *ready_out = 1;
        return 0;
    }
    if (g_fds[fd].token == 0 || !g_fds[fd].socket_connecting) return 0;
    const u64 events = net_poll_udp(g_fds[fd].token);
    if ((i64)events < 0) return events;
    if ((events & NET_POLL_WRITABLE) == 0) return 0;
    g_fds[fd].socket_connected = 1;
    g_fds[fd].socket_connecting = 0;
    sync_fd_to_thread_group(fd);
    *ready_out = 1;
    return 0;
}

static u64 socket_stream_ensure_connected(u64 fd) {
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET || g_fds[fd].socket_type != SOCK_STREAM) return errno_badf();
    if (g_fds[fd].socket_connected) return 0;
    if (g_fds[fd].token == 0 || !g_fds[fd].socket_connecting) return errno_notconn();
    int ready = 0;
    u64 result = socket_stream_poll_connect(fd, &ready);
    if ((i64)result < 0) return result;
    if (ready) return 0;
    if (fd_is_nonblock(fd)) return errno_again();
    result = net_tcp_wait_connected(g_fds[fd].token);
    if ((i64)result < 0) return result;
    g_fds[fd].socket_connected = 1;
    g_fds[fd].socket_connecting = 0;
    sync_fd_to_thread_group(fd);
    return 0;
}

static struct ipc_message handle_bind_socket(const struct trap_request *req) {
    const u64 fd = req->args[0];
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return reply(errno_badf(), 0);
    if (g_fds[fd].socket_type != SOCK_DGRAM) return reply(errno_nosys(), 0);
    u32 ip = 0;
    u16 port = 0;
    if (!read_sockaddr_in(req->args[1], req->args[2], &ip, &port)) return reply(errno_inval(), 0);
    (void)ip;
    if (g_fds[fd].token != 0) return reply(errno_inval(), 0);
    u64 handle = 0;
    u16 local_port = 0;
    u32 local_ip = 0;
    if (!net_bind_udp(port, &handle, &local_port, &local_ip)) return reply(errno_addrinuse(), 0);
    g_fds[fd].token = handle;
    g_fds[fd].socket_local_port = local_port;
    g_fds[fd].socket_local_ip = local_ip;
    sync_fd_to_thread_group(fd);
    return reply(0, 0);
}

static struct ipc_message handle_connect_socket(const struct trap_request *req) {
    const u64 fd = req->args[0];
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return reply(errno_badf(), 0);
    u32 remote_ip = 0;
    u16 remote_port = 0;
    if (!read_sockaddr_in(req->args[1], req->args[2], &remote_ip, &remote_port)) return reply(errno_inval(), 0);
    if (remote_ip == 0 || remote_port == 0) return reply(errno_inval(), 0);
    if (g_fds[fd].socket_type == SOCK_STREAM) {
        if (g_fds[fd].socket_connected) return reply(errno_isconn(), 0);
        if (g_fds[fd].token != 0) {
            int ready = 0;
            const u64 poll_result = socket_stream_poll_connect(fd, &ready);
            if ((i64)poll_result < 0) return reply(poll_result, 0);
            return reply(ready ? 0 : errno_already(), 0);
        }
        u64 handle = 0;
        u16 local_port = 0;
        u32 local_ip = 0;
        u64 result = net_tcp_begin_connect(remote_ip, remote_port, &handle, &local_port, &local_ip);
        if ((i64)result < 0) return reply(result, 0);
        g_fds[fd].token = handle;
        g_fds[fd].socket_local_port = local_port;
        g_fds[fd].socket_local_ip = local_ip;
        g_fds[fd].socket_connected = 0;
        g_fds[fd].socket_connecting = 1;
        g_fds[fd].socket_remote_ip = remote_ip;
        g_fds[fd].socket_remote_port = remote_port;
        sync_fd_to_thread_group(fd);
        if (fd_is_nonblock(fd)) {
            int ready = 0;
            result = socket_stream_poll_connect(fd, &ready);
            if ((i64)result < 0) return reply(result, 0);
            return reply(ready ? 0 : errno_inprogress(), 0);
        }
        result = net_tcp_wait_connected(handle);
        if ((i64)result < 0) {
            net_close_udp(handle);
            g_fds[fd].token = 0;
            g_fds[fd].socket_connecting = 0;
            sync_fd_to_thread_group(fd);
            return reply(result, 0);
        }
        g_fds[fd].socket_connected = 1;
        g_fds[fd].socket_connecting = 0;
        sync_fd_to_thread_group(fd);
        return reply(0, 0);
    }
    if (!ensure_socket_bound(fd)) return reply(errno_busy(), 0);
    g_fds[fd].socket_connected = 1;
    g_fds[fd].socket_connecting = 0;
    g_fds[fd].socket_remote_ip = remote_ip;
    g_fds[fd].socket_remote_port = remote_port;
    sync_fd_to_thread_group(fd);
    return reply(0, 0);
}

static u64 socket_datagram_send_payload(u64 fd, u32 remote_ip, u16 remote_port, const u8 *payload, u64 len, u64 flags) {
    if (len > NET_UDP_MAX_PAYLOAD) return errno_msgsize();
    if (!ensure_socket_bound(fd)) return errno_busy();

    u64 result = errno_again();
    const u64 limit = (fd_is_nonblock(fd) || (flags & MSG_DONTWAIT) != 0) ? UDP_NONBLOCK_SEND_GRACE_LOOPS : 8192;
    for (u64 i = 0; i <= limit; i++) {
        result = net_send_udp(g_fds[fd].token, remote_ip, remote_port, payload, (u32)len);
        if ((i64)result >= 0 || result != errno_again()) break;
        if (i == limit) break;
        wait_without_consuming_ipc();
    }
    return result;
}

static struct ipc_message handle_sendto(const struct trap_request *req) {
    const u64 fd = req->args[0];
    const u64 src_va = req->args[1];
    const u64 len = req->args[2];
    const u64 flags = req->args[3];
    const u64 addr_va = req->args[4];
    const u64 addr_len = req->args[5];
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return reply(errno_badf(), 0);
    if (g_fds[fd].socket_type == SOCK_STREAM) {
        if (addr_va != 0) return reply(errno_inval(), 0);
        return reply(socket_write_from_target(fd, src_va, len), 0);
    }
    if (len > NET_UDP_MAX_PAYLOAD) return reply(errno_msgsize(), 0);
    u32 remote_ip = 0;
    u16 remote_port = 0;
    if (addr_va != 0) {
        if (!read_sockaddr_in(addr_va, addr_len, &remote_ip, &remote_port)) return reply(errno_inval(), 0);
    } else {
        if (!g_fds[fd].socket_connected) return reply(errno_destaddrreq(), 0);
        remote_ip = g_fds[fd].socket_remote_ip;
        remote_port = g_fds[fd].socket_remote_port;
    }
    u8 payload[NET_UDP_MAX_PAYLOAD];
    if (len != 0 && copy_from_target(src_va, payload, len) != len) return reply(errno_fault(), 0);
    const u64 sent = socket_datagram_send_payload(fd, remote_ip, remote_port, payload, len, flags);
    return reply(sent, 0);
}

static struct ipc_message handle_recvfrom(const struct trap_request *req) {
    const u64 fd = req->args[0];
    const u64 dst_va = req->args[1];
    const u64 len = req->args[2];
    const u64 flags = req->args[3];
    const u64 addr_va = req->args[4];
    const u64 addrlen_va = req->args[5];
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return reply(errno_badf(), 0);
    if (g_fds[fd].socket_type == SOCK_STREAM) return reply(socket_read_to_target(fd, dst_va, len), 0);
    if (!ensure_socket_bound(fd)) return reply(errno_busy(), 0);
    u8 payload[NET_UDP_MAX_PAYLOAD];
    const u32 cap = (u32)min_u64(len, NET_UDP_MAX_PAYLOAD);
    u32 src_ip = 0;
    u16 src_port = 0;
    u64 result = errno_again();
    const u64 limit = (fd_is_nonblock(fd) || (flags & MSG_DONTWAIT) != 0) ? UDP_NONBLOCK_RECV_GRACE_LOOPS : 8192;
    for (u64 i = 0; i <= limit; i++) {
        result = net_recv_udp(g_fds[fd].token, payload, cap, &src_ip, &src_port);
        if ((i64)result >= 0 || result != errno_again()) break;
        if (i == limit) break;
        wait_without_consuming_ipc();
    }
    if ((i64)result < 0) {
        log_socket_error("LinuxAbiServer: recvmsg failed", fd, result);
        return reply(result, 0);
    }
    if (result != 0 && copy_to_target(dst_va, payload, result) != result) return reply(errno_fault(), 0);
    if (addr_va != 0) {
        u8 raw[16];
        for (u64 i = 0; i < sizeof(raw); i++) raw[i] = 0;
        write_net_le16(raw, AF_INET);
        write_net_be16(raw + 2, src_port);
        write_net_be32(raw + 4, src_ip);
        if (copy_to_target(addr_va, raw, sizeof(raw)) != sizeof(raw)) return reply(errno_fault(), 0);
    }
    if (addrlen_va != 0) {
        u32 addrlen = 16;
        if (copy_to_target(addrlen_va, &addrlen, sizeof(addrlen)) != sizeof(addrlen)) return reply(errno_fault(), 0);
    }
    return reply(result, 0);
}

static u64 copy_payload_to_iov(u64 iov_va, u64 iovcnt, const u8 *payload, u64 len) {
    if (iovcnt > 64) return errno_inval();
    u64 copied = 0;
    for (u64 i = 0; i < iovcnt && copied < len; i++) {
        u64 pair[2];
        if (copy_from_target(iov_va + i * 16, pair, sizeof(pair)) != sizeof(pair)) return errno_fault();
        const u64 n = min_u64(pair[1], len - copied);
        if (n != 0 && copy_to_target(pair[0], payload + copied, n) != n) return errno_fault();
        copied += n;
    }
    return copied;
}

static u64 copy_iov_to_payload(u64 iov_va, u64 iovcnt, u8 *payload, u64 payload_cap) {
    if (iovcnt > 64) return errno_inval();
    u64 copied = 0;
    for (u64 i = 0; i < iovcnt; i++) {
        u64 pair[2];
        if (copy_from_target(iov_va + i * 16, pair, sizeof(pair)) != sizeof(pair)) return errno_fault();
        if (pair[1] == 0) continue;
        if (copied + pair[1] > payload_cap) return errno_msgsize();
        if (copy_from_target(pair[0], payload + copied, pair[1]) != pair[1]) return errno_fault();
        copied += pair[1];
    }
    return copied;
}

static u64 socket_stream_send_payload(u64 fd, const u8 *payload, u64 len) {
    const u64 connect_result = socket_stream_ensure_connected(fd);
    if ((i64)connect_result < 0) return connect_result;
    u64 sent = 0;
    while (sent < len) {
        const u64 chunk = min_u64(len - sent, NET_TCP_MAX_PAYLOAD);
        u64 result = errno_again();
        const u64 limit = fd_is_nonblock(fd) ? 0 : 8192;
        for (u64 i = 0; i <= limit; i++) {
            result = net_tcp_write(g_fds[fd].token, payload + sent, (u32)chunk);
            if ((i64)result >= 0 || result != errno_again()) break;
            if (i == limit) break;
            wait_without_consuming_ipc();
        }
        if ((i64)result < 0) return sent != 0 ? sent : result;
        if (result == 0) break;
        sent += result;
        if (result < chunk) break;
    }
    return sent;
}

static u64 socket_stream_write_from_target(u64 fd, u64 src_va, u64 len) {
    u8 payload[NET_TCP_MAX_PAYLOAD];
    u64 sent = 0;
    while (sent < len) {
        const u64 chunk = min_u64(len - sent, NET_TCP_MAX_PAYLOAD);
        if (copy_from_target(src_va + sent, payload, chunk) != chunk) return sent != 0 ? sent : errno_fault();
        const u64 result = socket_stream_send_payload(fd, payload, chunk);
        if ((i64)result < 0) return sent != 0 ? sent : result;
        sent += result;
        if (result < chunk) break;
    }
    return sent;
}

static u64 socket_stream_send_iov_from_target(u64 fd, u64 iov_va, u64 iovcnt) {
    if (iovcnt > 64) return errno_inval();
    u64 total = 0;
    for (u64 i = 0; i < iovcnt; i++) {
        u64 pair[2];
        if (copy_from_target(iov_va + i * 16, pair, sizeof(pair)) != sizeof(pair)) return total != 0 ? total : errno_fault();
        if (pair[1] == 0) continue;
        const u64 result = socket_stream_write_from_target(fd, pair[0], pair[1]);
        if ((i64)result < 0) return total != 0 ? total : result;
        total += result;
        if (result < pair[1]) break;
    }
    return total;
}

static struct ipc_message handle_sendmsg_socket(const struct trap_request *req) {
    const u64 fd = req->args[0];
    const u64 msg_va = req->args[1];
    const u64 flags = req->args[2];
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return reply(errno_badf(), 0);
    if (msg_va == 0) return reply(errno_fault(), 0);

    struct linux_msghdr64 msg;
    if (copy_from_target(msg_va, &msg, sizeof(msg)) != sizeof(msg)) return reply(errno_fault(), 0);
    if (msg.msg_iov == 0 || msg.msg_iovlen == 0) return reply(errno_inval(), 0);

    if (g_fds[fd].socket_type == SOCK_STREAM) {
        if (msg.msg_name != 0) return reply(errno_inval(), 0);
        const u64 sent = socket_stream_send_iov_from_target(fd, msg.msg_iov, msg.msg_iovlen);
        return reply(sent, 0);
    }

    if (g_fds[fd].socket_type == SOCK_DGRAM && msg.msg_name != 0) {
        u8 payload[NET_UDP_MAX_PAYLOAD];
        const u64 copied = copy_iov_to_payload(msg.msg_iov, msg.msg_iovlen, payload, NET_UDP_MAX_PAYLOAD);
        if ((i64)copied < 0) return reply(copied, 0);
        u32 remote_ip = 0;
        u16 remote_port = 0;
        if (!read_sockaddr_in(msg.msg_name, msg.msg_namelen, &remote_ip, &remote_port)) return reply(errno_inval(), 0);
        const u64 sent = socket_datagram_send_payload(fd, remote_ip, remote_port, payload, copied, flags);
        return reply(sent, 0);
    }
    u8 payload[NET_UDP_MAX_PAYLOAD];
    const u64 copied = copy_iov_to_payload(msg.msg_iov, msg.msg_iovlen, payload, NET_UDP_MAX_PAYLOAD);
    if ((i64)copied < 0) return reply(copied, 0);
    return reply(socket_send_payload(fd, payload, copied), 0);
}

static struct ipc_message handle_recvmsg_socket(const struct trap_request *req) {
    const u64 fd = req->args[0];
    const u64 msg_va = req->args[1];
    const u64 flags = req->args[2];
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return reply(errno_badf(), 0);
    if (msg_va == 0) return reply(errno_fault(), 0);

    struct linux_msghdr64 msg;
    if (copy_from_target(msg_va, &msg, sizeof(msg)) != sizeof(msg)) return reply(errno_fault(), 0);
    if (msg.msg_iov == 0 || msg.msg_iovlen == 0) return reply(errno_inval(), 0);

    u64 iov0[2];
    if (copy_from_target(msg.msg_iov, iov0, sizeof(iov0)) != sizeof(iov0)) return reply(errno_fault(), 0);
    const u32 cap = (u32)min_u64(iov0[1], g_fds[fd].socket_type == SOCK_STREAM ? NET_TCP_READ_BYTES : NET_UDP_MAX_PAYLOAD);
    u8 *payload = g_net_io_payload;
    u32 src_ip = 0;
    u16 src_port = 0;
    u64 result = errno_again();

    if (g_fds[fd].socket_type == SOCK_STREAM) {
        const u64 connect_result = socket_stream_ensure_connected(fd);
        if ((i64)connect_result < 0) return reply(connect_result, 0);
        const u64 limit = (fd_is_nonblock(fd) || (flags & MSG_DONTWAIT) != 0) ? 0 : 8192;
        for (u64 i = 0; i <= limit; i++) {
            result = net_tcp_read(g_fds[fd].token, payload, cap);
            if ((i64)result >= 0 || result != errno_again()) break;
            if (i == limit) break;
            wait_without_consuming_ipc();
        }
    } else {
        if (!ensure_socket_bound(fd)) return reply(errno_busy(), 0);
        const u64 limit = (fd_is_nonblock(fd) || (flags & MSG_DONTWAIT) != 0) ? UDP_NONBLOCK_RECV_GRACE_LOOPS : 8192;
        for (u64 i = 0; i <= limit; i++) {
            result = net_recv_udp(g_fds[fd].token, payload, cap, &src_ip, &src_port);
            if ((i64)result >= 0 || result != errno_again()) break;
            if (i == limit) break;
            wait_without_consuming_ipc();
        }
    }
    if ((i64)result < 0) return reply(result, 0);

    const u64 copied = copy_payload_to_iov(msg.msg_iov, msg.msg_iovlen, payload, result);
    if ((i64)copied < 0) return reply(copied, 0);

    if (msg.msg_name != 0 && msg.msg_namelen >= 16 && g_fds[fd].socket_type == SOCK_DGRAM) {
        u8 raw[16] = {0};
        write_net_le16(raw + 0, AF_INET);
        write_net_be16(raw + 2, src_port);
        write_net_be32(raw + 4, src_ip);
        if (copy_to_target(msg.msg_name, raw, sizeof(raw)) != sizeof(raw)) return reply(errno_fault(), 0);
        msg.msg_namelen = sizeof(raw);
    } else if (msg.msg_name != 0) {
        msg.msg_namelen = 0;
    }
    msg.msg_controllen = 0;
    msg.msg_flags = 0;
    if (copy_to_target(msg_va, &msg, sizeof(msg)) != sizeof(msg)) return reply(errno_fault(), 0);
    return reply(copied, 0);
}

static u64 socket_send_payload(u64 fd, const u8 *payload, u64 len) {
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return errno_badf();
    if (g_fds[fd].socket_type == SOCK_STREAM) {
        return socket_stream_send_payload(fd, payload, len);
    }
    if (!g_fds[fd].socket_connected) return errno_destaddrreq();
    if (len > NET_UDP_MAX_PAYLOAD) return errno_msgsize();
    return socket_datagram_send_payload(fd, g_fds[fd].socket_remote_ip, g_fds[fd].socket_remote_port, payload, len, 0);
}

static u64 socket_write_from_target(u64 fd, u64 src_va, u64 len) {
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return errno_badf();
    if (g_fds[fd].socket_type == SOCK_STREAM) {
        return socket_stream_write_from_target(fd, src_va, len);
    }
    u8 payload[NET_UDP_MAX_PAYLOAD];
    if (len > NET_UDP_MAX_PAYLOAD) return errno_msgsize();
    if (len != 0 && copy_from_target(src_va, payload, len) != len) return errno_fault();
    return socket_send_payload(fd, payload, len);
}

static u64 socket_send_iov_from_target(u64 fd, u64 iov_va, u64 iovcnt) {
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return errno_badf();
    if (g_fds[fd].socket_type == SOCK_STREAM) {
        return socket_stream_send_iov_from_target(fd, iov_va, iovcnt);
    }
    if (iovcnt > 64) return errno_inval();
    u8 payload[NET_UDP_MAX_PAYLOAD];
    const u64 copied = copy_iov_to_payload(iov_va, iovcnt, payload, NET_UDP_MAX_PAYLOAD);
    if ((i64)copied < 0) return copied;
    return socket_send_payload(fd, payload, copied);
}

static u64 socket_read_to_target(u64 fd, u64 dst_va, u64 len) {
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return errno_badf();
    if (g_fds[fd].socket_type == SOCK_STREAM) {
        const u64 connect_result = socket_stream_ensure_connected(fd);
        if ((i64)connect_result < 0) return connect_result;
        u8 *payload = g_net_io_payload;
        const u32 cap = (u32)min_u64(len, NET_TCP_READ_BYTES);
        u64 result = errno_again();
        const u64 limit = fd_is_nonblock(fd) ? 0 : 8192;
        for (u64 i = 0; i <= limit; i++) {
            result = net_tcp_read(g_fds[fd].token, payload, cap);
            if ((i64)result >= 0 || result != errno_again()) break;
            if (i == limit) break;
            wait_without_consuming_ipc();
        }
        if ((i64)result < 0) {
            log_socket_error("LinuxAbiServer: socket read failed", fd, result);
            return result;
        }
        if (result != 0 && copy_to_target(dst_va, payload, result) != result) return errno_fault();
        return result;
    }
    if (!ensure_socket_bound(fd)) return errno_busy();
    u8 payload[NET_UDP_MAX_PAYLOAD];
    const u32 cap = (u32)min_u64(len, NET_UDP_MAX_PAYLOAD);
    u32 src_ip = 0;
    u16 src_port = 0;
    u64 result = errno_again();
    const u64 limit = fd_is_nonblock(fd) ? 0 : 8192;
    for (u64 i = 0; i <= limit; i++) {
        result = net_recv_udp(g_fds[fd].token, payload, cap, &src_ip, &src_port);
        if ((i64)result >= 0 || result != errno_again()) break;
        if (i == limit) break;
        wait_without_consuming_ipc();
    }
    if ((i64)result < 0) return result;
    if (result != 0 && copy_to_target(dst_va, payload, result) != result) return errno_fault();
    return result;
}

static struct ipc_message handle_getsockname_socket(const struct trap_request *req) {
    const u64 fd = req->args[0];
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return reply(errno_badf(), 0);
    if (g_fds[fd].socket_type == SOCK_DGRAM && !ensure_socket_bound(fd)) return reply(errno_busy(), 0);
    if (g_fds[fd].socket_type == SOCK_STREAM && g_fds[fd].token == 0) return reply(errno_inval(), 0);
    if (!write_sockaddr_in_to_target(req->args[1], req->args[2], g_fds[fd].socket_local_ip, g_fds[fd].socket_local_port)) return reply(errno_fault(), 0);
    return reply(0, 0);
}

static struct ipc_message handle_getpeername_socket(const struct trap_request *req) {
    const u64 fd = req->args[0];
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return reply(errno_badf(), 0);
    if (g_fds[fd].socket_type == SOCK_STREAM && !g_fds[fd].socket_connected) {
        int ready = 0;
        const u64 poll_result = socket_stream_poll_connect(fd, &ready);
        if ((i64)poll_result < 0) return reply(poll_result, 0);
    }
    if (!g_fds[fd].socket_connected) return reply(errno_notconn(), 0);
    if (!write_sockaddr_in_to_target(req->args[1], req->args[2], g_fds[fd].socket_remote_ip, g_fds[fd].socket_remote_port)) return reply(errno_fault(), 0);
    return reply(0, 0);
}

static struct ipc_message handle_setsockopt_socket(const struct trap_request *req) {
    const u64 fd = req->args[0];
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return reply(errno_badf(), 0);
    return reply(0, 0);
}

static struct ipc_message handle_getsockopt_socket(const struct trap_request *req) {
    const u64 fd = req->args[0];
    const u64 level = req->args[1];
    const u64 optname = req->args[2];
    const u64 optval_va = req->args[3];
    const u64 optlen_va = req->args[4];
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return reply(errno_badf(), 0);
    if (level == SOL_SOCKET && optname == SO_ERROR && optval_va != 0 && optlen_va != 0) {
        u32 optlen = 0;
        if (copy_from_target(optlen_va, &optlen, sizeof(optlen)) != sizeof(optlen)) return reply(errno_fault(), 0);
        if (optlen < sizeof(i32)) return reply(errno_inval(), 0);
        i32 value = 0;
        if (g_fds[fd].socket_type == SOCK_STREAM && g_fds[fd].socket_connecting && !g_fds[fd].socket_connected) {
            int ready = 0;
            const u64 poll_result = socket_stream_poll_connect(fd, &ready);
            if ((i64)poll_result < 0) value = (i32)(-(i64)poll_result);
        }
        optlen = sizeof(i32);
        if (copy_to_target(optval_va, &value, sizeof(value)) != sizeof(value) ||
            copy_to_target(optlen_va, &optlen, sizeof(optlen)) != sizeof(optlen)) return reply(errno_fault(), 0);
        return reply(0, 0);
    }
    if (level == SOL_SOCKET && optname == SO_TYPE && optval_va != 0 && optlen_va != 0) {
        u32 optlen = 0;
        if (copy_from_target(optlen_va, &optlen, sizeof(optlen)) != sizeof(optlen)) return reply(errno_fault(), 0);
        if (optlen < sizeof(i32)) return reply(errno_inval(), 0);
        i32 value = g_fds[fd].socket_type == SOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM;
        optlen = sizeof(i32);
        if (copy_to_target(optval_va, &value, sizeof(value)) != sizeof(value) ||
            copy_to_target(optlen_va, &optlen, sizeof(optlen)) != sizeof(optlen)) return reply(errno_fault(), 0);
        return reply(0, 0);
    }
    return reply(0, 0);
}

static struct ipc_message handle_shutdown_socket(const struct trap_request *req) {
    const u64 fd = req->args[0];
    if (!fd_valid(fd) || g_fds[fd].kind != FD_SOCKET) return reply(errno_badf(), 0);
    return reply(0, 0);
}

static u16 fd_poll_revents(u64 fd, u16 requested) {
    const u16 read_mask = (u16)(POLLIN | POLLRDNORM);
    const u16 write_mask = (u16)(POLLOUT | POLLWRNORM);
    if (!fd_valid(fd)) return POLLNVAL;
    if (requested == 0) return 0;

    struct fd_entry *entry = &g_fds[fd];
    if (entry->kind == FD_SOCKET) {
        u64 events = entry->socket_type == SOCK_DGRAM ? NET_POLL_WRITABLE : 0;
        if (entry->token != 0) {
            events = net_poll_udp(entry->token);
            if ((i64)events < 0) {
                if (entry->socket_type == SOCK_STREAM &&
                    entry->socket_connecting &&
                    (events == errno_io() || events == errno_timedout()))
                {
                    return 0;
                }
                log_socket_error("LinuxAbiServer: net poll failed", fd, events);
                return POLLERR;
            }
            if (entry->socket_type == SOCK_STREAM && entry->socket_connecting && !entry->socket_connected && (events & NET_POLL_WRITABLE) != 0) {
                entry->socket_connected = 1;
                entry->socket_connecting = 0;
                sync_fd_to_thread_group(fd);
            }
        }
        if ((i64)events < 0) {
            if (entry->socket_type == SOCK_STREAM &&
                entry->socket_connecting &&
                (events == errno_io() || events == errno_timedout()))
            {
                return 0;
            }
            log_socket_error("LinuxAbiServer: net poll failed", fd, events);
            return POLLERR;
        }
        u16 revents = 0;
        if ((events & NET_POLL_READABLE) != 0) revents |= (u16)(requested & read_mask);
        if ((events & NET_POLL_WRITABLE) != 0) revents |= (u16)(requested & write_mask);
        return revents;
    }

    if (entry->kind == FD_PIPE_READ) {
        const u8 pipe_id = entry->pipe_id;
        if (pipe_id >= PIPE_MAX || !g_pipes[pipe_id].used) return POLLNVAL;
        if (g_pipes[pipe_id].len != 0 || (g_pipes[pipe_id].write_refs == 0 && !pipe_has_live_writer(pipe_id))) return (u16)(requested & read_mask);
        return 0;
    }

    if (entry->kind == FD_PIPE_WRITE) {
        const u8 pipe_id = entry->pipe_id;
        if (pipe_id >= PIPE_MAX || !g_pipes[pipe_id].used) return POLLNVAL;
        if (g_pipes[pipe_id].read_refs == 0) return POLLERR;
        return g_pipes[pipe_id].len < PIPE_BUFFER_BYTES ? (u16)(requested & write_mask) : 0;
    }

    if (entry->kind == FD_FILE || entry->kind == FD_DIR) return (u16)(requested & (read_mask | write_mask));
    if (entry->kind == FD_RANDOM) return (u16)(requested & read_mask);
    if (entry->kind == FD_STDIO || entry->kind == FD_TTY) return (u16)(requested & (read_mask | write_mask));
    return POLLNVAL;
}

static u64 poll_wait_limit(const struct trap_request *req, int ppoll, int *immediate, int *fault) {
    *immediate = 0;
    *fault = 0;
    if (ppoll) {
        const u64 timespec_va = req->args[2];
        if (timespec_va == 0) return 8192;
        i64 pair[2];
        if (copy_from_target(timespec_va, pair, sizeof(pair)) != sizeof(pair)) { *fault = 1; return 0; }
        if (pair[0] == 0 && pair[1] == 0) { *immediate = 1; return 0; }
        return 8192;
    }
    const i32 timeout_ms = (i32)(u32)req->args[2];
    if (timeout_ms == 0) { *immediate = 1; return 0; }
    if (timeout_ms < 0) return 8192;
    u64 attempts = (u64)(u32)timeout_ms * 4 + 1;
    if (attempts > 8192) attempts = 8192;
    return attempts;
}

static i64 scan_pollfds(u64 fds_va, u64 nfds) {
    if (nfds > 64) return (i64)errno_inval();
    i64 ready = 0;
    for (u64 i = 0; i < nfds; i++) {
        struct linux_pollfd pfd;
        const u64 pfd_va = fds_va + i * sizeof(pfd);
        if (copy_from_target(pfd_va, &pfd, sizeof(pfd)) != sizeof(pfd)) return (i64)errno_fault();
        pfd.revents = 0;
        if (pfd.fd >= 0) {
            pfd.revents = (short)fd_poll_revents((u64)(u32)pfd.fd, (u16)pfd.events);
            if (pfd.revents != 0) ready++;
        }
        if (copy_to_target(pfd_va, &pfd, sizeof(pfd)) != sizeof(pfd)) return (i64)errno_fault();
    }
    return ready;
}

static struct ipc_message handle_poll(const struct trap_request *req, int ppoll) {
    const u64 fds_va = req->args[0];
    const u64 nfds = req->args[1];
    if (nfds != 0 && fds_va == 0) return reply(errno_fault(), 0);
    g_prof.poll_calls++;
    int immediate = 0;
    int fault = 0;
    const u64 wait_limit = poll_wait_limit(req, ppoll, &immediate, &fault);
    if (fault) return reply(errno_fault(), 0);
    for (u64 attempt = 0;; attempt++) {
        const i64 ready = scan_pollfds(fds_va, nfds);
        if (ready < 0) return reply((u64)ready, 0);
        if (ready != 0 || immediate || attempt >= wait_limit) {
            g_prof.poll_wait_loops += attempt;
            return reply((u64)ready, 0);
        }
        wait_without_consuming_ipc();
    }
}

static int select_timeout_is_zero(u64 timeout_va, int *fault) {
    *fault = 0;
    if (timeout_va == 0) return 0;
    i64 pair[2];
    if (copy_from_target(timeout_va, pair, sizeof(pair)) != sizeof(pair)) { *fault = 1; return 1; }
    return pair[0] == 0 && pair[1] == 0;
}

static i64 scan_select_sets(u64 nfds, u64 readfds_va, u64 writefds_va, u64 exceptfds_va, u64 *read_out, u64 *write_out, u64 *except_out) {
    if (nfds > 32) return (i64)errno_inval();
    u64 read_in = 0;
    u64 write_in = 0;
    u64 except_in = 0;
    if (readfds_va != 0 && copy_from_target(readfds_va, &read_in, sizeof(read_in)) != sizeof(read_in)) return (i64)errno_fault();
    if (writefds_va != 0 && copy_from_target(writefds_va, &write_in, sizeof(write_in)) != sizeof(write_in)) return (i64)errno_fault();
    if (exceptfds_va != 0 && copy_from_target(exceptfds_va, &except_in, sizeof(except_in)) != sizeof(except_in)) return (i64)errno_fault();
    (void)except_in;

    *read_out = 0;
    *write_out = 0;
    *except_out = 0;
    i64 ready = 0;
    for (u64 fd = 0; fd < nfds; fd++) {
        const u64 bit = 1ULL << fd;
        const int want_read = (read_in & bit) != 0;
        const int want_write = (write_in & bit) != 0;
        if (!want_read && !want_write) continue;
        if (!fd_valid(fd)) return (i64)errno_badf();
        const u16 requested = (u16)((want_read ? (POLLIN | POLLRDNORM) : 0) | (want_write ? (POLLOUT | POLLWRNORM) : 0));
        const u16 revents = fd_poll_revents(fd, requested);
        if (want_read && (revents & (POLLIN | POLLRDNORM)) != 0) { *read_out |= bit; ready++; }
        if (want_write && (revents & (POLLOUT | POLLWRNORM)) != 0) { *write_out |= bit; ready++; }
    }
    return ready;
}

static struct ipc_message handle_select(const struct trap_request *req, int pselect) {
    const u64 nfds = req->args[0];
    const u64 readfds_va = req->args[1];
    const u64 writefds_va = req->args[2];
    const u64 exceptfds_va = req->args[3];
    const u64 timeout_va = req->args[4];
    (void)pselect;
    g_prof.select_calls++;
    int fault = 0;
    const int immediate = select_timeout_is_zero(timeout_va, &fault);
    if (fault) return reply(errno_fault(), 0);
    const u64 wait_limit = timeout_va == 0 ? 8192 : (immediate ? 0 : 8192);
    for (u64 attempt = 0;; attempt++) {
        u64 read_out = 0;
        u64 write_out = 0;
        u64 except_out = 0;
        const i64 ready = scan_select_sets(nfds, readfds_va, writefds_va, exceptfds_va, &read_out, &write_out, &except_out);
        if (ready < 0) return reply((u64)ready, 0);
        if (ready != 0 || immediate || attempt >= wait_limit) {
            g_prof.select_wait_loops += attempt;
            if (readfds_va != 0 && copy_to_target(readfds_va, &read_out, sizeof(read_out)) != sizeof(read_out)) return reply(errno_fault(), 0);
            if (writefds_va != 0 && copy_to_target(writefds_va, &write_out, sizeof(write_out)) != sizeof(write_out)) return reply(errno_fault(), 0);
            if (exceptfds_va != 0 && copy_to_target(exceptfds_va, &except_out, sizeof(except_out)) != sizeof(except_out)) return reply(errno_fault(), 0);
            return reply((u64)ready, 0);
        }
        wait_without_consuming_ipc();
    }
}

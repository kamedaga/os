static u64 make_console_nonce(u64 request_paddr, u64 response_paddr, u64 endpoint_id, u64 process_slot) {
    u64 nonce = request_paddr ^ ((response_paddr << 17) | (response_paddr >> 47)) ^ ((endpoint_id << 7) | (endpoint_id >> 57)) ^ process_slot ^ 0x434f4e534f4c4555ULL;
    return nonce == 0 ? 1 : nonce;
}

static u64 g_console_read_diag_count = 0;

enum {
    TTY_ATTR_VERSION = 2,
    TTY_IFLAG_ICRNL = 1 << 0,
    TTY_IFLAG_INLCR = 1 << 1,
    TTY_IFLAG_IGNCR = 1 << 2,
    TTY_OFLAG_OPOST = 1 << 0,
    TTY_OFLAG_ONLCR = 1 << 1,
    TTY_LFLAG_ISIG = 1 << 0,
    TTY_LFLAG_ICANON = 1 << 1,
    TTY_LFLAG_ECHO = 1 << 2,
    TTY_LFLAG_ECHOE = 1 << 3,
    TTY_LFLAG_ECHOK = 1 << 4,
    TTY_LFLAG_ECHONL = 1 << 5,
    TTY_LFLAG_IEXTEN = 1 << 6,
    TTY_CC_COUNT = 32,
};

struct tty_attr_payload {
    u32 version;
    u32 iflag;
    u32 oflag;
    u32 lflag;
    u8 cc[TTY_CC_COUNT];
    u16 columns;
    u16 rows;
    u32 reserved0;
};

static void log_console_read_diag(const char *reason, u64 detail) {
    if (g_console_read_diag_count >= 16) return;
    g_console_read_diag_count++;
    user_log("LinuxAbiServer: console read ");
    user_log(reason);
    user_log(" detail=");
    user_log_hex_value(detail);
}

static int install_console_endpoint(void) {
    if (g_console.endpoint_id == 0 || g_console.process_slot == 0) return 0;
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, g_console.endpoint_id, g_console.process_slot) == SYSCALL_OK;
}

static int grant_console_response_page(void) {
    u64 ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, g_console.response_paddr, g_console.endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_console_endpoint()) ret = syscall3(SYSCALL_GRANT_CAP_ON_ENDPOINT, g_console.response_paddr, g_console.endpoint_id, PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE);
    return ret == SYSCALL_OK;
}

static int share_console_request_page(void) {
    u64 ret = syscall2(SYSCALL_SHARE_CAP, g_console.request_paddr, g_console.endpoint_id);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_console_endpoint()) ret = syscall2(SYSCALL_SHARE_CAP, g_console.request_paddr, g_console.endpoint_id);
    return ret == SYSCALL_OK;
}

static int signal_console(void) {
    u64 ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_console.endpoint_id, 0);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_console_endpoint()) ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_console.endpoint_id, 0);
    return ret == SYSCALL_OK;
}

static int wait_console_response(u64 expected_seq, u16 expected_op, u64 poll_limit) {
    volatile struct console_response_header *response = (volatile struct console_response_header *)CONSOLE_RESPONSE_VA;
    for (u64 i = 0; poll_limit == 0 || i < poll_limit; i++) {
        if (response->response_seq == expected_seq) {
            return response->magic == CONSOLE_RESPONSE_MAGIC &&
                response->version == CONSOLE_PROTOCOL_VERSION &&
                response->op == expected_op;
        }
        wait_without_consuming_ipc();
    }
    return 0;
}

static int connect_console_from_registry(void) {
    struct service_entry entry;
    const int using_tty = find_service(SERVICE_KIND_TTY, &entry);
    if (!using_tty && !find_service(SERVICE_KIND_CONSOLE, &entry)) return 0;
    g_console.endpoint_id = entry.endpoint_id;
    g_console.process_slot = entry.process_slot;
    g_console.request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    g_console.response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (g_console.request_paddr < 0x1000 || g_console.response_paddr < 0x1000) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, CONSOLE_REQUEST_VA, g_console.request_paddr, 1) != SYSCALL_OK) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, CONSOLE_RESPONSE_VA, g_console.response_paddr, 1) != SYSCALL_OK) return 0;
    if (!grant_console_response_page()) return 0;
    clear_page(CONSOLE_REQUEST_VA);
    clear_page(CONSOLE_RESPONSE_VA);

    const u64 self_slot = syscall0(SYSCALL_GET_PROCESS_SLOT);
    g_console.session_nonce = make_console_nonce(g_console.request_paddr, g_console.response_paddr, g_console.endpoint_id, self_slot);
    volatile struct console_request_header *request = (volatile struct console_request_header *)CONSOLE_REQUEST_VA;
    request->magic = CONSOLE_REQUEST_MAGIC;
    request->version = CONSOLE_PROTOCOL_VERSION;
    request->op = CONSOLE_OP_CONNECT;
    request->session_nonce = g_console.session_nonce;
    request->arg0 = g_console.response_paddr;
    request->arg1 = self_slot;
    __asm__ volatile("" ::: "memory");
    request->request_seq = 1;
    if (!share_console_request_page()) return 0;
    if (!wait_console_response(1, CONSOLE_OP_CONNECT, 8192)) return 0;
    volatile struct console_response_header *response = (volatile struct console_response_header *)CONSOLE_RESPONSE_VA;
    if (response->status != CONSOLE_STATUS_OK) return 0;
    g_console.next_seq = 2;
    g_console.active = 1;
    g_console.is_tty = using_tty;
    user_log(using_tty ? "LinuxAbiServer: tty connect ok\n" : "LinuxAbiServer: console connect ok\n");
    return 1;
}

static int console_begin_request(u16 op, u32 length, const char *inline_src, u32 inline_bytes, u64 *seq_out) {
    if (!g_console.active) return 0;
    if (inline_bytes > CONSOLE_REQUEST_PAYLOAD_BYTES) return 0;
    clear_page(CONSOLE_REQUEST_VA);
    clear_page(CONSOLE_RESPONSE_VA);
    volatile struct console_request_header *request = (volatile struct console_request_header *)CONSOLE_REQUEST_VA;
    const u64 seq = g_console.next_seq++;
    request->magic = CONSOLE_REQUEST_MAGIC;
    request->version = CONSOLE_PROTOCOL_VERSION;
    request->op = op;
    request->session_nonce = g_console.session_nonce;
    request->length = length;
    request->flags = 0;
    request->arg0 = 0;
    request->arg1 = 0;
    request->arg2 = 0;
    request->reserved0 = 0;
    if (inline_bytes != 0) {
        volatile u8 *payload = (volatile u8 *)(CONSOLE_REQUEST_VA + CONSOLE_REQUEST_HEADER_BYTES);
        for (u32 i = 0; i < inline_bytes; i++) payload[i] = (u8)inline_src[i];
    }
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_console()) return 0;
    *seq_out = seq;
    return 1;
}

static int console_get_tty_attr(struct tty_attr_payload *out) {
    if (!g_console.active) return 0;
    u64 seq = 0;
    if (!console_begin_request(CONSOLE_OP_GET_ATTR, sizeof(*out), 0, 0, &seq)) return 0;
    if (!wait_console_response(seq, CONSOLE_OP_GET_ATTR, 8192)) return 0;
    volatile struct console_response_header *response = (volatile struct console_response_header *)CONSOLE_RESPONSE_VA;
    if (response->status != CONSOLE_STATUS_OK) return 0;
    if (response->inline_bytes < sizeof(*out)) return 0;
    volatile u8 *src = (volatile u8 *)(CONSOLE_RESPONSE_VA + CONSOLE_RESPONSE_HEADER_BYTES);
    u8 *dst = (u8 *)out;
    for (u64 i = 0; i < sizeof(*out); i++) dst[i] = src[i];
    return out->version == TTY_ATTR_VERSION;
}

static int console_set_tty_attr(const struct tty_attr_payload *attr) {
    if (!g_console.active) return 0;
    u64 seq = 0;
    if (!console_begin_request(CONSOLE_OP_SET_ATTR, sizeof(*attr), (const char *)attr, sizeof(*attr), &seq)) return 0;
    if (!wait_console_response(seq, CONSOLE_OP_SET_ATTR, 8192)) return 0;
    volatile struct console_response_header *response = (volatile struct console_response_header *)CONSOLE_RESPONSE_VA;
    return response->status == CONSOLE_STATUS_OK;
}

static int console_take_tty_signal(u64 *signo_out) {
    *signo_out = 0;
    if (!g_console.active || !g_console.is_tty) return 0;
    u64 seq = 0;
    if (!console_begin_request(CONSOLE_OP_GET_SIGNAL, 0, 0, 0, &seq)) return 0;
    if (!wait_console_response(seq, CONSOLE_OP_GET_SIGNAL, 8192)) return 0;
    volatile struct console_response_header *response = (volatile struct console_response_header *)CONSOLE_RESPONSE_VA;
    if (response->status == CONSOLE_STATUS_AGAIN) return 1;
    if (response->status != CONSOLE_STATUS_OK) return 0;
    *signo_out = response->arg0;
    return 1;
}

static int poll_tty_signal_events(void) {
    static u64 next_poll_tick = 0;
    const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
    if (now < next_poll_tick) return 0;
    next_poll_tick = now + 4;
    for (;;) {
        u64 signo = 0;
        if (!console_take_tty_signal(&signo) || signo == 0) return 1;
        next_poll_tick = now;
        deliver_tty_signal(signo);
    }
}

static u64 console_write_bytes(const char *bytes, u64 len) {
    if (!g_console.active) {
        user_log("LinuxAbiServer: console write inactive\n");
        return errno_io();
    }
    u64 done = 0;
    while (done < len) {
        u64 chunk = min_u64(len - done, CONSOLE_REQUEST_PAYLOAD_BYTES);
        u64 seq = 0;
        if (!console_begin_request(CONSOLE_OP_WRITE, (u32)chunk, bytes + done, (u32)chunk, &seq)) {
            user_log("LinuxAbiServer: console write begin failed\n");
            return done != 0 ? done : errno_io();
        }
        if (!wait_console_response(seq, CONSOLE_OP_WRITE, 8192)) {
            user_log("LinuxAbiServer: console write response timeout\n");
            return done != 0 ? done : errno_io();
        }
        volatile struct console_response_header *response = (volatile struct console_response_header *)CONSOLE_RESPONSE_VA;
        if (response->status != CONSOLE_STATUS_OK) {
            user_log("LinuxAbiServer: console write status=");
            user_log_hex_value(response->status);
            return done != 0 ? done : errno_io();
        }
        done += chunk;
    }
    return done;
}

static u64 console_write_from_target(u64 src, u64 len, int *fault) {
    *fault = 0;
    u64 done = 0;
    while (done < len) {
        char buf[128];
        const u64 chunk = min_u64(len - done, sizeof(buf));
        if (copy_from_target(src + done, buf, chunk) != chunk) {
            *fault = 1;
            return done;
        }
        const u64 n = console_write_bytes(buf, chunk);
        if (n != chunk) return done != 0 ? done : n;
        done += chunk;
    }
    return done;
}

static u64 console_read_to_target(u64 dst, u64 len, int *fault) {
    *fault = 0;
    if (!g_console.active) {
        log_console_read_diag("inactive", len);
        return errno_io();
    }
    if (len == 0) return 0;
    const u32 request_len = (u32)min_u64(len, CONSOLE_RESPONSE_PAYLOAD_BYTES);
    for (;;) {
        u64 seq = 0;
        if (!console_begin_request(CONSOLE_OP_READ, request_len, 0, 0, &seq)) {
            log_console_read_diag("begin-failed", request_len);
            return errno_io();
        }
        if (!wait_console_response(seq, CONSOLE_OP_READ, 0)) {
            log_console_read_diag("wait-failed", seq);
            return errno_io();
        }
        volatile struct console_response_header *response = (volatile struct console_response_header *)CONSOLE_RESPONSE_VA;
        if (response->status == CONSOLE_STATUS_AGAIN) {
            wait_without_consuming_ipc();
            continue;
        }
        if (response->status == CONSOLE_STATUS_INTERRUPTED) {
            if (response->arg0 != 0) deliver_tty_signal(response->arg0);
            return errno_intr();
        }
        if (response->status != CONSOLE_STATUS_OK) {
            log_console_read_diag("status", response->status);
            return errno_io();
        }
        const u64 n = min_u64(response->inline_bytes, request_len);
        if (n == 0) return 0;
        if (copy_to_target(dst, (const void *)(CONSOLE_RESPONSE_VA + CONSOLE_RESPONSE_HEADER_BYTES), n) != n) {
            log_console_read_diag("copy-failed", n);
            *fault = 1;
            return 0;
        }
        return n;
    }
}

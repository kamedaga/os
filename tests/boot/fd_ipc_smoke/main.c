#include "pacha/ipc.h"
#include "pacha/syscall.h"

typedef unsigned long long u64;

enum {
    SMOKE_SYSCALL_LOG = 1,
    SMOKE_SYSCALL_OK = 0,
};

static u64 cstr_len(const char *s) {
    u64 n = 0;
    while (s[n] != 0) n++;
    return n;
}

static u64 smoke_syscall2(u64 nr, u64 a0, u64 a1) {
    return (u64)pacha_syscall2(nr, a0, a1);
}

static void log_text(const char *s) {
    (void)smoke_syscall2(SMOKE_SYSCALL_LOG, (u64)s, cstr_len(s));
}

static void log_hex(const char *prefix, u64 value) {
    static const char digits[] = "0123456789abcdef";
    char buf[96];
    u64 n = 0;
    while (prefix[n] != 0 && n + 19 < sizeof(buf)) {
        buf[n] = prefix[n];
        n++;
    }
    buf[n++] = '0';
    buf[n++] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[n++] = digits[(value >> ((u64)i * 4)) & 0xf];
    }
    buf[n++] = '\n';
    (void)smoke_syscall2(SMOKE_SYSCALL_LOG, (u64)buf, n);
}

static void log_hex_inline(u64 value) {
    static const char digits[] = "0123456789abcdef";
    char buf[18];
    u64 n = 0;
    buf[n++] = '0';
    buf[n++] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[n++] = digits[(value >> ((u64)i * 4)) & 0xf];
    }
    (void)smoke_syscall2(SMOKE_SYSCALL_LOG, (u64)buf, n);
}

static int expect_u64(const char *label, u64 got, u64 expected) {
    if (got == expected) return 1;
    log_text(label);
    log_hex(" got=", got);
    log_hex(" expected=", expected);
    return 0;
}

static int expect_ok(const char *label, int status) {
    if (status == 0) return 1;
    log_text(label);
    log_hex(" status=", (u64)(long long)status);
    return 0;
}

static int expect_fd_readable(const char *label, int fd);

static int fast_echo_handler(void *ctx, const struct pacha_ipc_fast_entry *request, struct pacha_ipc_fast_entry *response) {
    (void)ctx;
    pacha_ipc_fast_entry_init(response, request->op + 1, request->offset + 0x1000, request->len * 2, request->flags + 1);
    response->status = 0x1234;
    return 0;
}

static int run_normal_backend_smoke(u64 endpoint_rights) {
    struct pacha_ipc_channel_pair pair = {0, 0};
    if (!expect_ok("[fd_ipc_boot_smoke] normal backend channel create failed\n", pacha_ipc_channel_create(&pair, endpoint_rights, 0))) return 0;
    struct pacha_ipc_fast_channel client = {0};
    struct pacha_ipc_fast_channel server = {0};
    if (!expect_ok("[fd_ipc_boot_smoke] normal client init failed\n", pacha_ipc_fast_channel_init_normal(&client, pair.a))) return 0;
    if (!expect_ok("[fd_ipc_boot_smoke] normal server init failed\n", pacha_ipc_fast_channel_init_normal(&server, pair.b))) return 0;
    struct pacha_ipc_fast_entry request = {0};
    pacha_ipc_fast_entry_init(&request, 0x7171, 0x4000, 32, 3);
    if (!expect_ok("[fd_ipc_boot_smoke] normal send failed\n", pacha_ipc_fast_send(&client, &request))) return 0;
    if (!expect_ok("[fd_ipc_boot_smoke] normal serve failed\n", pacha_ipc_fast_serve_once(&server, fast_echo_handler, 0))) return 0;
    struct pacha_ipc_fast_entry response = {0};
    if (!expect_ok("[fd_ipc_boot_smoke] normal recv failed\n", pacha_ipc_fast_recv(&client, &response))) return 0;
    if (!expect_u64("[fd_ipc_boot_smoke] normal response op mismatch\n", response.op, 0x7172)) return 0;
    if (!expect_u64("[fd_ipc_boot_smoke] normal response len mismatch\n", response.len, 64)) return 0;
    return 1;
}

static int run_process_thread_create_smoke(void) {
    const u64 process_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_SPAWN |
        PACHA_FD_RIGHT_KILL |
        PACHA_FD_RIGHT_SET_CONTEXT |
        PACHA_FD_RIGHT_MAP_INTO;
    const u64 thread_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_START |
        PACHA_FD_RIGHT_KILL |
        PACHA_FD_RIGHT_SET_CONTEXT;

    const int process_fd = pacha_process_create(process_rights, 0);
    if (process_fd < 16) {
        log_hex("[fd_ipc_boot_smoke] process_create failed=", (u64)(long long)process_fd);
        return 0;
    }
    struct pacha_fd_info process_info = {0};
    if (!expect_ok("[fd_ipc_boot_smoke] process fd_get_info failed\n", pacha_fd_get_info(process_fd, &process_info))) return 0;
    if (!expect_u64("[fd_ipc_boot_smoke] process fd kind mismatch\n", process_info.kind, PACHA_FD_KIND_PROCESS)) return 0;

    const int thread_fd = pacha_thread_create(process_fd, 0x400000, 0x800000, 0, 0, thread_rights);
    if (thread_fd < 16) {
        log_hex("[fd_ipc_boot_smoke] thread_create failed=", (u64)(long long)thread_fd);
        return 0;
    }
    struct pacha_fd_info thread_info = {0};
    if (!expect_ok("[fd_ipc_boot_smoke] thread fd_get_info failed\n", pacha_fd_get_info(thread_fd, &thread_info))) return 0;
    if (!expect_u64("[fd_ipc_boot_smoke] thread fd kind mismatch\n", thread_info.kind, PACHA_FD_KIND_THREAD)) return 0;
    log_text("[fd_ipc_boot_smoke] process/thread create OK\n");
    return 1;
}

static int run_timerfd_wait_smoke(void) {
    const u64 rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_READ;
    const int timer_fd = pacha_timerfd_create(1000000ull, 0, rights, 0);
    if (timer_fd < 16) {
        log_hex("[fd_ipc_boot_smoke] timerfd_create failed=", (u64)(long long)timer_fd);
        return 0;
    }
    struct pacha_fd_info timer_info = {0};
    if (!expect_ok("[fd_ipc_boot_smoke] timer fd_get_info failed\n", pacha_fd_get_info(timer_fd, &timer_info))) return 0;
    if (!expect_u64("[fd_ipc_boot_smoke] timer fd kind mismatch\n", timer_info.kind, PACHA_FD_KIND_TIMER)) return 0;

    struct pacha_pollfd pfd = {
        .fd = timer_fd,
        .events = PACHA_FD_EVENT_READABLE,
    };
    const long wait_ready = pacha_fd_wait_many(&pfd, 1, PACHA_FD_WAIT_FOREVER);
    if (wait_ready < 1) {
        log_hex("[fd_ipc_boot_smoke] timer wait failed=", (u64)(long long)wait_ready);
        return 0;
    }
    if ((pfd.revents & PACHA_FD_EVENT_READABLE) == 0) {
        log_hex("[fd_ipc_boot_smoke] timer revents=", pfd.revents);
        return 0;
    }
    u64 expirations = 0;
    const long read_len = pacha_fd_read(timer_fd, &expirations, sizeof(expirations));
    if (read_len != 8 || expirations == 0) {
        log_hex("[fd_ipc_boot_smoke] timer read_len=", (u64)(long long)read_len);
        log_hex("[fd_ipc_boot_smoke] timer expirations=", expirations);
        return 0;
    }
    (void)pacha_fd_close(timer_fd);
    log_text("[fd_ipc_boot_smoke] timerfd wait OK\n");
    return 1;
}

static int run_eventfd_io_smoke(void) {
    const u64 rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_READ |
        PACHA_FD_RIGHT_WRITE |
        PACHA_FD_RIGHT_SET_FLAGS;
    const int event_fd = pacha_eventfd_create(0, rights, 0);
    if (event_fd < 16) {
        log_hex("[fd_ipc_boot_smoke] eventfd_create failed=", (u64)(long long)event_fd);
        return 0;
    }
    struct pacha_fd_info event_info = {0};
    if (!expect_ok("[fd_ipc_boot_smoke] event fd_get_info failed\n", pacha_fd_get_info(event_fd, &event_info))) return 0;
    if (!expect_u64("[fd_ipc_boot_smoke] event fd kind mismatch\n", event_info.kind, PACHA_FD_KIND_EVENT)) return 0;
    if (!expect_u64("[fd_ipc_boot_smoke] event fcntl flags mismatch\n", (u64)pacha_fd_fcntl(event_fd, PACHA_FD_FCNTL_GET_FLAGS, 0, 0), 0)) return 0;

    u64 value = 3;
    const long write_len = pacha_fd_write(event_fd, &value, sizeof(value));
    if (write_len != 8) {
        log_hex("[fd_ipc_boot_smoke] event write_len=", (u64)(long long)write_len);
        return 0;
    }
    if (!expect_fd_readable("[fd_ipc_boot_smoke] event fd_poll not readable\n", event_fd)) return 0;

    u64 read_value = 0;
    const long read_len = pacha_fd_read(event_fd, &read_value, sizeof(read_value));
    if (read_len != 8 || read_value != 3) {
        log_hex("[fd_ipc_boot_smoke] event read_len=", (u64)(long long)read_len);
        log_hex("[fd_ipc_boot_smoke] event read_value=", read_value);
        return 0;
    }

    value = 5;
    const struct pacha_iovec write_iov = {
        .base = &value,
        .len = sizeof(value),
    };
    const long writev_len = pacha_fd_writev(event_fd, &write_iov, 1);
    if (writev_len != 8) {
        log_hex("[fd_ipc_boot_smoke] event writev_len=", (u64)(long long)writev_len);
        return 0;
    }
    read_value = 0;
    const struct pacha_iovec read_iov = {
        .base = &read_value,
        .len = sizeof(read_value),
    };
    const long readv_len = pacha_fd_readv(event_fd, &read_iov, 1);
    if (readv_len != 8 || read_value != 5) {
        log_hex("[fd_ipc_boot_smoke] event readv_len=", (u64)(long long)readv_len);
        log_hex("[fd_ipc_boot_smoke] event readv_value=", read_value);
        return 0;
    }
    (void)pacha_fd_close(event_fd);
    log_text("[fd_ipc_boot_smoke] eventfd IO OK\n");
    return 1;
}

static int run_anonymous_mmap_smoke(void) {
    unsigned char *page = (unsigned char *)pacha_mmap_anonymous(
        4096,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_PRIVATE
    );
    if (!page) {
        log_text("[fd_ipc_boot_smoke] anonymous mmap failed\n");
        return 0;
    }

    page[0] = 0x5a;
    page[4095] = 0xa5;
    if (page[0] != 0x5a || page[4095] != 0xa5) {
        log_hex("[fd_ipc_boot_smoke] anonymous mmap byte0=", page[0]);
        log_hex("[fd_ipc_boot_smoke] anonymous mmap byte4095=", page[4095]);
        return 0;
    }
    if (!expect_ok("[fd_ipc_boot_smoke] anonymous munmap failed\n", pacha_munmap(page, 4096))) return 0;
    log_text("[fd_ipc_boot_smoke] anonymous mmap OK\n");
    return 1;
}

static int expect_fd_readable(const char *label, int fd) {
    struct pacha_pollfd pfd = {
        .fd = fd,
        .events = PACHA_FD_EVENT_READABLE,
    };
    const long ready = pacha_fd_poll(&pfd, 1);
    if (ready != 1 || (pfd.revents & PACHA_FD_EVENT_READABLE) == 0) {
        log_text(label);
        log_hex(" ready=", (u64)(long long)ready);
        log_hex(" revents=", pfd.revents);
        return 0;
    }
    return 1;
}

void fd_ipc_boot_smoke_main(void) {
    log_text("[fd_ipc_boot_smoke] start\n");

    if (!run_process_thread_create_smoke()) return;
    if (!run_timerfd_wait_smoke()) return;
    if (!run_eventfd_io_smoke()) return;
    if (!run_anonymous_mmap_smoke()) return;

    const u64 endpoint_rights =
        PACHA_FD_RIGHT_SEND |
        PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_CALL |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_CLOSE;
    const int endpoint = pacha_ipc_endpoint_create(endpoint_rights, 0);
    if (endpoint < 16) {
        log_hex("[fd_ipc_boot_smoke] endpoint_create failed=", (u64)(long long)endpoint);
        return;
    }

    struct pacha_ipc_msg send_msg = {
        .word0 = 11,
        .word1 = 22,
        .word2 = 33,
        .word3 = 44,
    };
    if (!expect_ok("[fd_ipc_boot_smoke] endpoint send failed\n", pacha_ipc_send(endpoint, &send_msg))) return;
    if (!expect_fd_readable("[fd_ipc_boot_smoke] endpoint fd_poll not readable\n", endpoint)) return;

    struct pacha_ipc_msg recv_msg = {
        .fd_capacity = 0,
    };
    if (!expect_ok("[fd_ipc_boot_smoke] endpoint recv failed\n", pacha_ipc_recv(endpoint, &recv_msg))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] endpoint word0 mismatch\n", recv_msg.word0, 11)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] endpoint word3 mismatch\n", recv_msg.word3, 44)) return;

    struct pacha_ipc_channel_pair pair = {0, 0};
    if (!expect_ok("[fd_ipc_boot_smoke] channel create failed\n", pacha_ipc_channel_create(&pair, endpoint_rights, 0))) return;
    send_msg.word0 = 55;
    send_msg.word3 = 88;
    if (!expect_ok("[fd_ipc_boot_smoke] channel send failed\n", pacha_ipc_send(pair.a, &send_msg))) return;
    if (!expect_fd_readable("[fd_ipc_boot_smoke] channel fd_poll not readable\n", pair.b)) return;
    recv_msg = (struct pacha_ipc_msg){0};
    if (!expect_ok("[fd_ipc_boot_smoke] channel recv failed\n", pacha_ipc_recv(pair.b, &recv_msg))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] channel word0 mismatch\n", recv_msg.word0, 55)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] channel word3 mismatch\n", recv_msg.word3, 88)) return;

    send_msg = (struct pacha_ipc_msg){
        .word0 = 101,
        .word1 = 202,
    };
    const int client_reply = pacha_ipc_call(endpoint, &send_msg);
    if (client_reply < 16) {
        log_hex("[fd_ipc_boot_smoke] call failed=", (u64)(long long)client_reply);
        return;
    }

    struct pacha_ipc_fd recv_fds[1] = {0};
    recv_msg = (struct pacha_ipc_msg){
        .fds = recv_fds,
        .fd_capacity = 1,
    };
    if (!expect_ok("[fd_ipc_boot_smoke] call recv failed\n", pacha_ipc_recv(endpoint, &recv_msg))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] call word0 mismatch\n", recv_msg.word0, 101)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] call fd_count mismatch\n", recv_msg.fd_count, 1)) return;

    const int server_reply = (int)recv_fds[0].fd;
    send_msg = (struct pacha_ipc_msg){
        .word0 = 303,
        .word1 = 404,
    };
    if (!expect_ok("[fd_ipc_boot_smoke] reply failed\n", pacha_ipc_reply(server_reply, &send_msg))) return;

    recv_msg = (struct pacha_ipc_msg){0};
    if (!expect_ok("[fd_ipc_boot_smoke] reply recv failed\n", pacha_ipc_recv(client_reply, &recv_msg))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] reply word0 mismatch\n", recv_msg.word0, 303)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] reply word1 mismatch\n", recv_msg.word1, 404)) return;

    struct pacha_ipc_fast_channel fast = {0};
    if (!expect_ok("[fd_ipc_boot_smoke] fast init failed\n", pacha_ipc_fast_channel_init_local(&fast, endpoint, PACHA_IPC_FAST_F_PREFER_PKEY, 1))) return;
    log_text("[fd_ipc_boot_smoke] fast backend=");
    log_text(pacha_ipc_fast_backend_name(fast.backend));
    log_text(" reason=");
    log_text(pacha_ipc_fast_fallback_reason_name(fast.fallback_reason));
    log_text(" err=");
    log_hex_inline((u64)(unsigned long long)(long long)fast.last_error);
    log_text(" pku=");
    log_hex_inline((u64)(unsigned)pacha_ipc_pkey_supported());
    log_text(" ospke=");
    log_hex_inline((u64)(unsigned)pacha_ipc_pkey_enabled());
    log_text("\n");

    struct pacha_ipc_channel_pair fast_pair = {0, 0};
    if (!expect_ok("[fd_ipc_boot_smoke] fast setup channel create failed\n", pacha_ipc_channel_create(&fast_pair, endpoint_rights, 0))) return;
    struct pacha_ipc_fast_channel client_fast = {0};
    struct pacha_ipc_fast_channel server_fast = {0};
    if (!expect_ok("[fd_ipc_boot_smoke] fast offer failed\n", pacha_ipc_fast_channel_offer(&client_fast, fast_pair.a, PACHA_IPC_FAST_F_PREFER_PKEY, 1))) return;
    if (!expect_ok("[fd_ipc_boot_smoke] fast accept failed\n", pacha_ipc_fast_channel_accept(&server_fast, fast_pair.b, PACHA_IPC_FAST_F_PREFER_PKEY, 1))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] fast client not ready\n", (u64)(unsigned)pacha_ipc_fast_channel_ready(&client_fast), 1)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] fast server not ready\n", (u64)(unsigned)pacha_ipc_fast_channel_ready(&server_fast), 1)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] fast client ring backend missing\n", (u64)(unsigned)pacha_ipc_fast_channel_uses_ring(&client_fast), 1)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] fast server ring backend missing\n", (u64)(unsigned)pacha_ipc_fast_channel_uses_ring(&server_fast), 1)) return;
    log_text("[fd_ipc_boot_smoke] fast setup client=");
    log_text(pacha_ipc_fast_backend_name(client_fast.backend));
    log_text(" server=");
    log_text(pacha_ipc_fast_backend_name(server_fast.backend));
    log_text(" reason=");
    log_text(pacha_ipc_fast_fallback_reason_name(server_fast.fallback_reason));
    log_text(" err=");
    log_hex_inline((u64)(unsigned long long)(long long)server_fast.last_error);
    log_text("\n");

    struct pacha_ipc_fast_entry fast_request = {0};
    pacha_ipc_fast_entry_init(&fast_request, 0x5151, 0x2000, 128, 9);
    if (!expect_ok("[fd_ipc_boot_smoke] fast request send failed\n", pacha_ipc_fast_send(&client_fast, &fast_request))) return;
    if (!expect_ok("[fd_ipc_boot_smoke] fast serve_once failed\n", pacha_ipc_fast_serve_once(&server_fast, fast_echo_handler, 0))) return;
    struct pacha_ipc_fast_entry fast_response_recv = {0};
    if (!expect_ok("[fd_ipc_boot_smoke] fast response recv failed\n", pacha_ipc_fast_recv(&client_fast, &fast_response_recv))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] fast response op mismatch\n", fast_response_recv.op, 0x5152)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] fast response len mismatch\n", fast_response_recv.len, 256)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] fast response status mismatch\n", fast_response_recv.status, 0x1234)) return;

    if (!run_normal_backend_smoke(endpoint_rights)) return;

    log_text("[fd_ipc_boot_smoke] OK\n");
}

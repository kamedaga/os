typedef unsigned long long u64;

enum {
    SYSCALL_LOG = 0x9,
    SYSCALL_IPC_ENDPOINT_CREATE = 0x140,
    SYSCALL_IPC_CHANNEL_CREATE = 0x141,
    SYSCALL_IPC_SEND = 0x142,
    SYSCALL_IPC_RECV = 0x143,
    SYSCALL_IPC_CALL = 0x144,
    SYSCALL_IPC_REPLY = 0x145,

    FD_RIGHT_TRANSFER = 1ull << 2,
    FD_RIGHT_CLOSE = 1ull << 6,
    FD_RIGHT_SEND = 1ull << 7,
    FD_RIGHT_RECV = 1ull << 8,
    FD_RIGHT_CALL = 1ull << 9,
};

struct ipc_fd {
    u64 fd;
    u64 rights;
    u64 flags;
    u64 transfer_flags;
};

struct ipc_msg {
    u64 word0;
    u64 word1;
    u64 word2;
    u64 word3;
    struct ipc_fd *fds;
    u64 fd_count;
    u64 fd_capacity;
    u64 flags;
};

static u64 cstr_len(const char *s) {
    u64 n = 0;
    while (s[n] != 0) n++;
    return n;
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

static u64 syscall3(u64 nr, u64 a0, u64 a1, u64 a2) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static void log_text(const char *s) {
    (void)syscall2(SYSCALL_LOG, (u64)s, cstr_len(s));
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
    (void)syscall2(SYSCALL_LOG, (u64)buf, n);
}

static int expect_u64(const char *label, u64 got, u64 expected) {
    if (got == expected) return 1;
    log_text(label);
    log_hex(" got=", got);
    log_hex(" expected=", expected);
    return 0;
}

static int expect_ok(const char *label, u64 status) {
    if (status == 0) return 1;
    log_text(label);
    log_hex(" status=", status);
    return 0;
}

void fd_ipc_boot_smoke_main(void) {
    log_text("[fd_ipc_boot_smoke] start\n");

    const u64 endpoint_rights =
        FD_RIGHT_SEND | FD_RIGHT_RECV | FD_RIGHT_CALL | FD_RIGHT_TRANSFER | FD_RIGHT_CLOSE;
    const u64 endpoint = syscall2(SYSCALL_IPC_ENDPOINT_CREATE, endpoint_rights, 0);
    if (endpoint < 16) {
        log_hex("[fd_ipc_boot_smoke] endpoint_create failed=", endpoint);
        return;
    }

    struct ipc_msg send_msg = {
        .word0 = 11,
        .word1 = 22,
        .word2 = 33,
        .word3 = 44,
    };
    if (!expect_ok("[fd_ipc_boot_smoke] endpoint send failed\n", syscall2(SYSCALL_IPC_SEND, endpoint, (u64)&send_msg))) return;

    struct ipc_msg recv_msg = {
        .fd_capacity = 0,
    };
    if (!expect_ok("[fd_ipc_boot_smoke] endpoint recv failed\n", syscall2(SYSCALL_IPC_RECV, endpoint, (u64)&recv_msg))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] endpoint word0 mismatch\n", recv_msg.word0, 11)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] endpoint word3 mismatch\n", recv_msg.word3, 44)) return;

    u64 pair[2] = {0, 0};
    if (!expect_ok("[fd_ipc_boot_smoke] channel create failed\n", syscall3(SYSCALL_IPC_CHANNEL_CREATE, (u64)pair, endpoint_rights, 0))) return;
    send_msg.word0 = 55;
    send_msg.word3 = 88;
    if (!expect_ok("[fd_ipc_boot_smoke] channel send failed\n", syscall2(SYSCALL_IPC_SEND, pair[0], (u64)&send_msg))) return;
    recv_msg = (struct ipc_msg){0};
    if (!expect_ok("[fd_ipc_boot_smoke] channel recv failed\n", syscall2(SYSCALL_IPC_RECV, pair[1], (u64)&recv_msg))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] channel word0 mismatch\n", recv_msg.word0, 55)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] channel word3 mismatch\n", recv_msg.word3, 88)) return;

    send_msg = (struct ipc_msg){
        .word0 = 101,
        .word1 = 202,
    };
    const u64 client_reply = syscall2(SYSCALL_IPC_CALL, endpoint, (u64)&send_msg);
    if (client_reply < 16) {
        log_hex("[fd_ipc_boot_smoke] call failed=", client_reply);
        return;
    }

    struct ipc_fd recv_fds[1] = {0};
    recv_msg = (struct ipc_msg){
        .fds = recv_fds,
        .fd_capacity = 1,
    };
    if (!expect_ok("[fd_ipc_boot_smoke] call recv failed\n", syscall2(SYSCALL_IPC_RECV, endpoint, (u64)&recv_msg))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] call word0 mismatch\n", recv_msg.word0, 101)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] call fd_count mismatch\n", recv_msg.fd_count, 1)) return;

    const u64 server_reply = recv_fds[0].fd;
    send_msg = (struct ipc_msg){
        .word0 = 303,
        .word1 = 404,
    };
    if (!expect_ok("[fd_ipc_boot_smoke] reply failed\n", syscall2(SYSCALL_IPC_REPLY, server_reply, (u64)&send_msg))) return;

    recv_msg = (struct ipc_msg){0};
    if (!expect_ok("[fd_ipc_boot_smoke] reply recv failed\n", syscall2(SYSCALL_IPC_RECV, client_reply, (u64)&recv_msg))) return;
    if (!expect_u64("[fd_ipc_boot_smoke] reply word0 mismatch\n", recv_msg.word0, 303)) return;
    if (!expect_u64("[fd_ipc_boot_smoke] reply word1 mismatch\n", recv_msg.word1, 404)) return;

    log_text("[fd_ipc_boot_smoke] OK\n");
}

#include "pacha/ipc.h"

typedef unsigned long long u64;

enum {
    SYSCALL_LOG = 0x9,
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

static int expect_ok(const char *label, int status) {
    if (status == 0) return 1;
    log_text(label);
    log_hex(" status=", (u64)(long long)status);
    return 0;
}

void fd_ipc_boot_smoke_main(void) {
    log_text("[fd_ipc_boot_smoke] start\n");

    const u64 endpoint_rights =
        PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE;
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

    log_text("[fd_ipc_boot_smoke] OK\n");
}

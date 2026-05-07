#include "exec_service_client.h"

typedef unsigned long long u64;

enum {
    SYSCALL_LOG = 0x9,
    SYSCALL_PROCESS_EXIT = 0x34,
    DASH_SHIM_REQUEST_VA = 0x27300000ULL,
    DASH_SHIM_RESPONSE_VA = 0x27301000ULL,
    DASH_SHIM_WAIT_TICKS = 120000,
};

static u64 syscall1(u64 n, u64 a0) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a0) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall2(u64 n, u64 a0, u64 a1) {
    u64 ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a0), "S"(a1) : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 cstr_len(const char *s) {
    u64 n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static void user_log(const char *s) {
    (void)syscall2(SYSCALL_LOG, (u64)s, cstr_len(s));
}

static void process_exit(u64 code) {
    (void)syscall1(SYSCALL_PROCESS_EXIT, code);
    for (;;) __asm__ volatile("pause");
}

void dash_shim_main(void) {
    user_log("DashShim: started\n");
    static const char *argv[] = {
        "/cmd/dash_interactive.elf",
        "-i",
    };
    static const char *envp[] = {
        "PATH=/bin:/cmd",
        "HOME=/",
        "SHELL=/bin/dash",
        "TERM=virtio-console",
        "PS1=# ",
        "CAPABILITYOS=1",
    };
    struct exec_service_spawn_result result;
    const struct exec_service_spawn_options options = {
        .path = "/cmd/dash_interactive.elf",
        .argv = argv,
        .argv_count = 2,
        .envp = envp,
        .envp_count = 6,
        .request_va = DASH_SHIM_REQUEST_VA,
        .response_va = DASH_SHIM_RESPONSE_VA,
        .wait_ticks = DASH_SHIM_WAIT_TICKS,
    };

    if (!exec_service_spawn_linux(&options, &result)) {
        user_log("DashShim: spawn failed\n");
        process_exit(1);
    }
    user_log("DashShim: dash spawned\n");
    process_exit(0);
}

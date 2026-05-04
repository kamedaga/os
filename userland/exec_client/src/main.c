#include "exec_service_client.h"

typedef unsigned long long u64;

enum {
    SYSCALL_LOG = 0x9,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_GET_PROCESS_STATUS = 0x30,
    SYSCALL_PROCESS_EXIT = 0x34,
    PROCESS_STATUS_INACTIVE = 0,
    PROCESS_STATUS_ACTIVE = 1,
    PROCESS_STATUS_FAULTED = 2,
    EXEC_CLIENT_REQUEST_VA = 0x27000000ULL,
    EXEC_CLIENT_RESPONSE_VA = 0x27001000ULL,
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

void exec_client_main(void) {
    user_log("ExecClient: started\n");

    static const char *argv[] = {
        "/cmd/dash.elf",
        "-c",
        "echo basic-ok; "
        "x=var-ok; echo $x; "
        "for x in for-a for-b; do echo $x; done; "
        "case word in word) echo case-ok;; *) echo case-bad;; esac; "
        "f(){ echo func-ok; }; f; "
        "cd /cmd && pwd; "
        "y=$(echo subst-ok); "
        "if [ \"$y\" = subst-ok ]; then echo subst-ok; else echo subst-bad; fi; "
        "true && echo and-ok || echo and-bad; "
        "false || echo or-ok; "
        "[ x = x ] && echo test-ok || echo test-bad; "
        "(echo subshell-ok); "
        "gok=0; for g in /cmd/*.elf; do case $g in /cmd/dash.elf) gok=1;; esac; done; "
        "[ \"$gok\" = 1 ] && echo glob-ok || echo glob-bad; "
        "echo hidden > /dev/null && echo redirect-ok || echo redirect-bad; "
        "echo file-ok > /tmp/ds; "
        "read z < /tmp/ds; "
        "[ \"$z\" = file-ok ] && echo file-rw-ok || echo file-rw-bad; "
        "cd /tmp && [ \"$(pwd)\" = /tmp ] && echo cwd-ok || echo cwd-bad; "
        "echo rel-ok > r; "
        "read rz < r; "
        "[ \"$rz\" = rel-ok ] && echo rel-rw-ok || echo rel-rw-bad; "
        "/cmd/busybox.elf cat /tmp/ds > /tmp/bc; "
        "echo busybox-cat-cmd-returned; "
        "read bbcat < /tmp/bc; "
        "[ \"$bbcat\" = file-ok ] && echo busybox-cat-ok || echo busybox-cat-bad; "
        "cd /cmd; "
        "echo fat-ok > /share/f; "
        "read fz < /share/f; "
        "[ \"$fz\" = fat-ok ] && echo fat-rw-ok || echo fat-rw-bad; "
        "/cmd/busybox.elf cat /share/f > /tmp/bf; "
        "read bbfat < /tmp/bf; "
        "[ \"$bbfat\" = fat-ok ] && echo busybox-fat-cat-ok || echo busybox-fat-cat-bad; "
        "/cmd/busybox.elf true && echo busybox-true-ok || echo busybox-true-bad; "
        "/cmd/busybox.elf false || echo busybox-false-ok; "
        "/cmd/musl_smoke.elf argv-smoke && echo musl-smoke-ok || echo musl-smoke-bad; "
        "echo pipe-ok | while read x; do echo $x; done; "
        "echo dash-smoke-done"
    };
    static const char *envp[] = { "PATH=/bin:/cmd", "CAPABILITYOS=1" };
    struct exec_service_spawn_result result;
    const struct exec_service_spawn_options options = {
        .path = "/cmd/dash.elf",
        .argv = argv,
        .argv_count = 3,
        .envp = envp,
        .envp_count = 2,
        .request_va = EXEC_CLIENT_REQUEST_VA,
        .response_va = EXEC_CLIENT_RESPONSE_VA,
        .wait_ticks = 20000,
    };

    if (!exec_service_spawn_linux(&options, &result)) {
        user_log("ExecClient: spawn failed\n");
        process_exit(1);
    }
    user_log("ExecClient: spawn ok\n");

    while (1) {
        const u64 status = syscall1(SYSCALL_GET_PROCESS_STATUS, result.linux_abi_process_slot);
        const u64 status_kind = status & 0xff;
        if (status_kind == PROCESS_STATUS_INACTIVE) {
            user_log("ExecClient: child done\n");
            process_exit(0);
        }
        if (status_kind == PROCESS_STATUS_FAULTED) {
            user_log("ExecClient: child faulted\n");
            process_exit(1);
        }
        if (status_kind != PROCESS_STATUS_ACTIVE) {
            user_log("ExecClient: child status invalid\n");
            process_exit(1);
        }
        (void)syscall2(SYSCALL_WAIT_EVENT, 0, 1);
    }
}

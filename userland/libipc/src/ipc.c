#include "pacha/ipc.h"

static long pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1) {
    uint64_t ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return (long)ret;
}

static long pacha_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    uint64_t ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r8", "r9", "r10", "r11", "memory");
    return (long)ret;
}

static int pacha_status_to_int(long status) {
    return status == 0 ? 0 : -(int)status;
}

static int pacha_fd_result_to_int(long result) {
    return result >= 16 ? (int)result : -(int)result;
}

int pacha_ipc_endpoint_create(uint64_t rights, uint32_t flags) {
    return pacha_fd_result_to_int(pacha_syscall2(PACHA_IPC_SYSCALL_ENDPOINT_CREATE, rights, flags));
}

int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *out, uint64_t rights, uint32_t flags) {
    if (!out) return -1;
    uint64_t pair[2] = {0, 0};
    const long status = pacha_syscall3(PACHA_IPC_SYSCALL_CHANNEL_CREATE, (uint64_t)(uintptr_t)pair, rights, flags);
    if (status != 0) return -(int)status;
    out->a = (int)pair[0];
    out->b = (int)pair[1];
    return 0;
}

int pacha_ipc_send(int fd, const struct pacha_ipc_msg *msg) {
    if (!msg) return -1;
    return pacha_status_to_int(pacha_syscall2(PACHA_IPC_SYSCALL_SEND, (uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)msg));
}

int pacha_ipc_recv(int fd, struct pacha_ipc_msg *msg) {
    if (!msg) return -1;
    return pacha_status_to_int(pacha_syscall2(PACHA_IPC_SYSCALL_RECV, (uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)msg));
}

int pacha_ipc_call(int fd, const struct pacha_ipc_msg *msg) {
    if (!msg) return -1;
    return pacha_fd_result_to_int(pacha_syscall2(PACHA_IPC_SYSCALL_CALL, (uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)msg));
}

int pacha_ipc_reply(int reply_fd, const struct pacha_ipc_msg *msg) {
    if (!msg) return -1;
    return pacha_status_to_int(pacha_syscall2(PACHA_IPC_SYSCALL_REPLY, (uint64_t)(uint32_t)reply_fd, (uint64_t)(uintptr_t)msg));
}

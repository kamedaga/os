#ifndef PACHA_IPC_H
#define PACHA_IPC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PACHA_IPC_SYSCALL_ENDPOINT_CREATE = 0x140,
    PACHA_IPC_SYSCALL_CHANNEL_CREATE = 0x141,
    PACHA_IPC_SYSCALL_SEND = 0x142,
    PACHA_IPC_SYSCALL_RECV = 0x143,
    PACHA_IPC_SYSCALL_CALL = 0x144,
    PACHA_IPC_SYSCALL_REPLY = 0x145,

    PACHA_IPC_MAX_TRANSFER_FDS = 8,

    PACHA_IPC_TRANSFER_MOVE = 1u << 0,
    PACHA_IPC_TRANSFER_CLOEXEC = 1u << 1,
    PACHA_IPC_TRANSFER_NONBLOCK = 1u << 2,
    PACHA_IPC_TRANSFER_INHERIT = 1u << 3,
    PACHA_IPC_TRANSFER_PRIVATE = 1u << 4,

    PACHA_FD_RIGHT_INSPECT = 1ull << 0,
    PACHA_FD_RIGHT_DUP = 1ull << 1,
    PACHA_FD_RIGHT_TRANSFER = 1ull << 2,
    PACHA_FD_RIGHT_WAIT = 1ull << 3,
    PACHA_FD_RIGHT_POLL = 1ull << 4,
    PACHA_FD_RIGHT_SET_FLAGS = 1ull << 5,
    PACHA_FD_RIGHT_CLOSE = 1ull << 6,
    PACHA_FD_RIGHT_SEND = 1ull << 7,
    PACHA_FD_RIGHT_RECV = 1ull << 8,
    PACHA_FD_RIGHT_CALL = 1ull << 9,
    PACHA_FD_RIGHT_ACCEPT = 1ull << 10,
    PACHA_FD_RIGHT_BIND = 1ull << 11,
    PACHA_FD_RIGHT_ENDPOINT_SIGNAL = 1ull << 12,
};

struct pacha_ipc_fd {
    uint64_t fd;
    uint64_t rights;
    uint64_t flags;
    uint64_t transfer_flags;
};

struct pacha_ipc_msg {
    uint64_t word0;
    uint64_t word1;
    uint64_t word2;
    uint64_t word3;
    struct pacha_ipc_fd *fds;
    uint64_t fd_count;
    uint64_t fd_capacity;
    uint64_t flags;
};

struct pacha_ipc_channel_pair {
    int a;
    int b;
};

int pacha_ipc_endpoint_create(uint64_t rights, uint32_t flags);
int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *out, uint64_t rights, uint32_t flags);
int pacha_ipc_send(int fd, const struct pacha_ipc_msg *msg);
int pacha_ipc_recv(int fd, struct pacha_ipc_msg *msg);
int pacha_ipc_call(int fd, const struct pacha_ipc_msg *msg);
int pacha_ipc_reply(int reply_fd, const struct pacha_ipc_msg *msg);

#ifdef __cplusplus
}
#endif

#endif

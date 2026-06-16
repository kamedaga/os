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
    PACHA_FD_SYSCALL_VMO_CREATE = 0x107,
    PACHA_FD_SYSCALL_MMAP = 0x109,

    PACHA_IPC_MAX_TRANSFER_FDS = 8,
    PACHA_IPC_FAST_RING_ENTRIES = 64,

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
    PACHA_FD_RIGHT_MAP_READ = 1ull << 13,
    PACHA_FD_RIGHT_MAP_WRITE = 1ull << 14,
    PACHA_FD_RIGHT_MAP_EXEC = 1ull << 15,

    PACHA_MMAP_SHARED = 1ull << 3,
    PACHA_MMAP_PKEY_SHIFT = 8,
    PACHA_MMAP_PKEY_MASK = 0xfull << PACHA_MMAP_PKEY_SHIFT,
    PACHA_PROT_READ = 1ull << 0,
    PACHA_PROT_WRITE = 1ull << 1,
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

enum pacha_ipc_fast_backend {
    PACHA_IPC_BACKEND_NORMAL = 0,
    PACHA_IPC_BACKEND_SHARED_VMO_RING = 1,
    PACHA_IPC_BACKEND_PKEY_RING = 2,
};

enum pacha_ipc_fast_fallback_reason {
    PACHA_IPC_FAST_FALLBACK_NONE = 0,
    PACHA_IPC_FAST_FALLBACK_PKEY_NOT_REQUESTED = 1,
    PACHA_IPC_FAST_FALLBACK_PKEY_INVALID = 2,
    PACHA_IPC_FAST_FALLBACK_PKEY_UNAVAILABLE = 3,
    PACHA_IPC_FAST_FALLBACK_VMO_CREATE_FAILED = 4,
    PACHA_IPC_FAST_FALLBACK_PKEY_MMAP_FAILED = 5,
    PACHA_IPC_FAST_FALLBACK_RING_MMAP_FAILED = 6,
};

enum {
    PACHA_IPC_FAST_F_PREFER_PKEY = 1u << 0,
    PACHA_IPC_FAST_F_REQUIRE_PKEY = 1u << 1,
};

struct pacha_ipc_fast_entry {
    uint64_t op;
    uint64_t seq;
    uint64_t generation;
    uint64_t offset;
    uint64_t len;
    uint64_t flags;
    uint64_t status;
    uint64_t reserved;
};

struct pacha_ipc_fast_ring {
    uint64_t magic;
    uint64_t generation;
    uint64_t capacity;
    volatile uint64_t producer;
    volatile uint64_t consumer;
    struct pacha_ipc_fast_entry entries[PACHA_IPC_FAST_RING_ENTRIES];
};

struct pacha_ipc_fast_channel {
    int channel_fd;
    int request_vmo_fd;
    int completion_vmo_fd;
    enum pacha_ipc_fast_backend backend;
    enum pacha_ipc_fast_fallback_reason fallback_reason;
    uint32_t pkey;
    uint32_t flags;
    struct pacha_ipc_fast_ring *request;
    struct pacha_ipc_fast_ring *completion;
    struct pacha_ipc_fast_ring *tx;
    struct pacha_ipc_fast_ring *rx;
};

typedef int (*pacha_ipc_fast_handler_fn)(
    void *ctx,
    const struct pacha_ipc_fast_entry *request,
    struct pacha_ipc_fast_entry *response
);

int pacha_ipc_endpoint_create(uint64_t rights, uint32_t flags);
int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *out, uint64_t rights, uint32_t flags);
int pacha_ipc_send(int fd, const struct pacha_ipc_msg *msg);
int pacha_ipc_recv(int fd, struct pacha_ipc_msg *msg);
int pacha_ipc_call(int fd, const struct pacha_ipc_msg *msg);
int pacha_ipc_reply(int reply_fd, const struct pacha_ipc_msg *msg);

int pacha_vmo_create(uint64_t size, uint64_t rights, uint32_t flags);
void *pacha_mmap(int fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset);
uint64_t pacha_mmap_pkey_flags(uint64_t flags, uint32_t pkey);

int pacha_ipc_pkey_supported(void);
int pacha_ipc_pkey_enabled(void);
uint32_t pacha_ipc_pkru_read(void);
void pacha_ipc_pkru_write(uint32_t pkru);
uint32_t pacha_ipc_pkey_disable_mask(uint32_t pkey);
void pacha_ipc_pkey_open(uint32_t pkey, uint32_t *saved_pkru);
void pacha_ipc_pkey_close(uint32_t pkey, uint32_t saved_pkru);

int pacha_ipc_fast_channel_init_local(struct pacha_ipc_fast_channel *fast, int channel_fd, uint32_t flags, uint32_t pkey);
int pacha_ipc_fast_channel_init_normal(struct pacha_ipc_fast_channel *fast, int channel_fd);
int pacha_ipc_fast_channel_offer(struct pacha_ipc_fast_channel *fast, int control_fd, uint32_t flags, uint32_t pkey);
int pacha_ipc_fast_channel_accept(struct pacha_ipc_fast_channel *fast, int control_fd, uint32_t flags, uint32_t pkey);
int pacha_ipc_fast_channel_ready(const struct pacha_ipc_fast_channel *fast);
int pacha_ipc_fast_channel_uses_ring(const struct pacha_ipc_fast_channel *fast);
void pacha_ipc_fast_entry_init(struct pacha_ipc_fast_entry *entry, uint64_t op, uint64_t offset, uint64_t len, uint64_t flags);
int pacha_ipc_fast_send(struct pacha_ipc_fast_channel *fast, const struct pacha_ipc_fast_entry *entry);
int pacha_ipc_fast_recv(struct pacha_ipc_fast_channel *fast, struct pacha_ipc_fast_entry *out);
int pacha_ipc_fast_call(struct pacha_ipc_fast_channel *fast, const struct pacha_ipc_fast_entry *request, struct pacha_ipc_fast_entry *response);
int pacha_ipc_fast_serve_once(struct pacha_ipc_fast_channel *fast, pacha_ipc_fast_handler_fn handler, void *ctx);
const char *pacha_ipc_fast_backend_name(enum pacha_ipc_fast_backend backend);
const char *pacha_ipc_fast_fallback_reason_name(enum pacha_ipc_fast_fallback_reason reason);

#ifdef __cplusplus
}
#endif

#endif

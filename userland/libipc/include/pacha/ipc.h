#ifndef PACHA_IPC_H
#define PACHA_IPC_H

#include "pacha/abi.h"
#include "pacha/syscall.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PACHA_IPC_MAX_TRANSFER_FDS = 19,
    PACHA_IPC_FAST_RING_ENTRIES = 64,

    PACHA_IPC_TRANSFER_MOVE = 1u << 0,
    PACHA_IPC_TRANSFER_CLOEXEC = 1u << 1,
    PACHA_IPC_TRANSFER_NONBLOCK = 1u << 2,
    PACHA_IPC_TRANSFER_INHERIT = 1u << 3,
    PACHA_IPC_TRANSFER_PRIVATE = 1u << 4,
};

struct pacha_ipc_fd {
    uint64_t fd;
    uint64_t rights;
    uint64_t flags;
    uint64_t transfer_flags;
};

/* Userspace convenience representation, never passed as a kernel header.
 * Initialize omitted words to zero; the machine ABI always transfers four. */
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

static inline long pacha_ipc_message_syscall(uint64_t nr, uint64_t fd,
    const struct pacha_ipc_msg *send, struct pacha_ipc_msg *receive,
    uint64_t timeout, uint64_t flags, int *committed) {
    uint64_t words[4] = {0, 0, 0, 0};
    struct pacha_ipc_extension extension = {0};
    if (send) {
        words[0] = send->word0; words[1] = send->word1;
        words[2] = send->word2; words[3] = send->word3;
        extension.send_array = (uintptr_t)send->fds;
        extension.send_count = send->fd_count;
    }
    if (receive) {
        extension.receive_array = (uintptr_t)receive->fds;
        extension.receive_capacity = receive->fd_capacity;
    }
    long status = pacha_ipc_registers(nr, fd, flags, timeout, words,
        (extension.send_count || extension.receive_capacity) ? &extension : 0, committed);
    if (receive) {
        receive->word0 = words[0]; receive->word1 = words[1];
        receive->word2 = words[2]; receive->word3 = words[3];
        receive->fd_count = extension.received_count;
    }
    return status;
}

struct pacha_ipc_channel_pair {
    pacha_fd_t a;
    pacha_fd_t b;
};

struct pacha_fd_info {
    uint64_t kind;
    uint64_t rights;
    uint64_t flags;
    uint64_t size;
    uint64_t extra;
};

struct pacha_iovec {
    void *base;
    uint64_t len;
};

#include "pacha/poll.h"

enum { PACHA_SERVICE_WAIT_MAX_FDS = 1024 };

struct pacha_service_wait_set {
    struct pacha_pollfd fds[PACHA_SERVICE_WAIT_MAX_FDS];
    uint64_t count;
};

enum pacha_ipc_fast_backend {
    PACHA_IPC_BACKEND_NORMAL = 0,
    PACHA_IPC_BACKEND_PKEY_RING = 2,
};

enum pacha_ipc_fast_fallback_reason {
    PACHA_IPC_FAST_FALLBACK_NONE = 0,
    PACHA_IPC_FAST_FALLBACK_PKEY_NOT_REQUESTED = 1,
    PACHA_IPC_FAST_FALLBACK_PKEY_INVALID = 2,
    PACHA_IPC_FAST_FALLBACK_PKEY_UNAVAILABLE = 3,
    PACHA_IPC_FAST_FALLBACK_VMO_CREATE_FAILED = 4,
    PACHA_IPC_FAST_FALLBACK_PKEY_MMAP_FAILED = 5,
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
    pacha_fd_t channel_fd;
    pacha_fd_t request_vmo_fd;
    pacha_fd_t completion_vmo_fd;
    enum pacha_ipc_fast_backend backend;
    enum pacha_ipc_fast_fallback_reason fallback_reason;
    uint32_t pkey;
    uint32_t flags;
    int last_error;
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

pacha_fd_t pacha_ipc_endpoint_create(uint64_t rights, uint32_t flags);
int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *out, uint64_t rights, uint32_t flags);
int pacha_ipc_send(pacha_fd_t fd, const struct pacha_ipc_msg *msg);
int pacha_ipc_recv(pacha_fd_t fd, struct pacha_ipc_msg *msg);
int pacha_ipc_recv_wait(pacha_fd_t fd, struct pacha_ipc_msg *msg, uint64_t timeout_ticks);
/* sent/replied records commitment even when the following wait fails. */
#define PACHA_IPC_REPLY_IMPLICIT ((pacha_fd_t)-1)
int pacha_ipc_call(pacha_fd_t fd, const struct pacha_ipc_msg *msg,
    struct pacha_ipc_msg *reply, uint64_t timeout_ticks, int *sent);
int pacha_ipc_reply_recv(pacha_fd_t fd, const struct pacha_ipc_msg *reply,
    struct pacha_ipc_msg *next, uint64_t timeout_ticks, int *replied);
pacha_fd_t pacha_ipc_reply_extract(void);
int pacha_ipc_reply(pacha_fd_t reply_fd, const struct pacha_ipc_msg *msg);

/* Explicit copy grants only. Empty list means an empty child FD table;
 * INHERIT is not consulted. Sources must carry TRANSFER, rights may only
 * shrink, and flags describe the child descriptor, not the source. */
#define PACHA_PROCESS_CREATE_MAX_GRANTS 64u
struct pacha_process_fd_grant {
    uint64_t source_fd, target_fd, rights, flags;
};
pacha_fd_t pacha_process_create(uint64_t rights, uint32_t flags,
    const struct pacha_process_fd_grant *grants, uint64_t count);
pacha_fd_t pacha_process_clone(uint64_t rights, uint32_t flags);
pacha_fd_t pacha_thread_create(pacha_fd_t process_fd, uint64_t entry_rip, uint64_t stack_rsp, uint64_t flags, uint64_t fs_base, uint64_t rights);
int pacha_thread_start(pacha_fd_t thread_fd);
struct pacha_thread_registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};
struct pacha_thread_context {
    uint64_t size, xstate_features, fs_base, gs_base, pkru, reserved[3];
    struct pacha_thread_registers registers;
    unsigned char xstate[832];
};
/* CPU-quiescent suspended threads only. Active waits/faults return NOT_READY.
 * GET requires INSPECT; SET requires SET_CONTEXT. Neither resumes the thread. */
int pacha_thread_get_context(pacha_fd_t thread_fd, struct pacha_thread_context *context);
int pacha_thread_set_context(pacha_fd_t thread_fd, const struct pacha_thread_context *context);
int pacha_thread_signal(pacha_fd_t thread_fd, unsigned int notification);
int pacha_thread_register_fault(uint64_t entry_rip, uint64_t stack_base, uint64_t stack_size);
/* Trampolines receive this frame in RDI on a 16-byte aligned runtime stack.
 * Register order follows the native x86-64 trap frame, not Linux pt_regs.
 * Fault frames use signo=0 and append vector/error/address metadata. error_code
 * includes PACHA_FAULT_ERROR_SOFTWARE_MASK; mask it off for the raw CPU code;
 * return both kinds through PROCESS_SIGNAL_CTL_RETURN. */
struct pacha_native_signal_frame {
    uint64_t magic, size, signo, xstate_features;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
    unsigned char xstate[832];
};
struct pacha_native_fault_frame {
    struct pacha_native_signal_frame signal;
    uint64_t vector, error_code, address;
};
int pacha_thread_set_gs_base(uint64_t gs_base);
long pacha_process_map(pacha_fd_t process_fd, pacha_fd_t vmo_fd, uint64_t target_va, uint64_t size, uint64_t prot, uint64_t vmo_offset);
/* MAP_INTO authority; returns only after cross-CPU range invalidation. */
int pacha_process_unmap(pacha_fd_t process_fd, uint64_t target_va, uint64_t size, uint64_t flags);
struct pacha_process_map_batch_entry {
    uint64_t vmo_fd;
    uint64_t target_va;
    uint64_t size;
    uint64_t prot;
    uint64_t vmo_offset;
    uint64_t flags;
};
long pacha_process_map_flags(
    pacha_fd_t process_fd,
    pacha_fd_t vmo_fd,
    uint64_t target_va,
    uint64_t size,
    uint64_t prot,
    uint64_t vmo_offset,
    uint64_t flags);
int pacha_process_map_batch(
    pacha_fd_t process_fd,
    const struct pacha_process_map_batch_entry *entries,
    uint64_t entry_count);
/* Returns bytes written or negative native status (INVALID=-1, MAP=-4).
 * Native flags must be zero; one request is limited to 4096 bytes. */
long pacha_getrandom(void *buf, uint64_t len, uint64_t flags);
struct pacha_fd_table_info {
    uint64_t capacity;
    uint64_t maximum;
    uint64_t free_slots;
};
/* Zero queries; nonzero ensures a minimum capacity. Free slots are a snapshot,
 * not a reservation. Growth never changes existing descriptor numbers. */
int pacha_fd_table(uint64_t minimum_capacity, struct pacha_fd_table_info *out);
int pacha_fd_get_info(pacha_fd_t fd, struct pacha_fd_info *out);
/* Caller-owned, dynamically sized array; preserves native wait batch bounds. */
long pacha_fd_wait_many_batched(struct pacha_pollfd *fds, uint64_t count,
    uint64_t timeout_ticks);
int pacha_fd_close(pacha_fd_t fd);
long pacha_fd_read(pacha_fd_t fd, void *buf, uint64_t len);
long pacha_fd_write(pacha_fd_t fd, const void *buf, uint64_t len);
long pacha_fd_readv(pacha_fd_t fd, const struct pacha_iovec *iov, uint64_t iov_count);
long pacha_fd_writev(pacha_fd_t fd, const struct pacha_iovec *iov, uint64_t iov_count);
long pacha_fd_fcntl(pacha_fd_t fd, uint64_t cmd, uint64_t arg0, uint64_t arg1);
long pacha_fd_poll(struct pacha_pollfd *fds, uint64_t count);
long pacha_fd_wait_many(struct pacha_pollfd *fds, uint64_t count, uint64_t timeout_ticks);
int pacha_service_wait_init(struct pacha_service_wait_set *set, pacha_fd_t endpoint_fd);
int pacha_service_wait_add(struct pacha_service_wait_set *set, pacha_fd_t fd, uint32_t events);
long pacha_service_wait(struct pacha_service_wait_set *set, uint64_t timeout_ticks);
uint64_t pacha_service_wait_revents(
    const struct pacha_service_wait_set *set,
    pacha_fd_t fd);
pacha_fd_t pacha_eventfd_create(uint64_t initial_value, uint64_t rights, uint32_t fd_flags);
pacha_fd_t pacha_timerfd_create(uint64_t initial_ns, uint64_t interval_ns, uint64_t rights, uint32_t fd_flags);
int pacha_timerfd_settime(pacha_fd_t fd, uint64_t initial_ns, uint64_t interval_ns, uint64_t flags);

pacha_fd_t pacha_vmo_create(uint64_t size, uint64_t rights, uint32_t flags);
/* Grow page-aligned capacity while preserving live aliases; requires RESIZE. */
int pacha_vmo_grow(pacha_fd_t fd, uint64_t new_capacity);
pacha_fd_t pacha_vmo_create_contiguous(uint64_t size, uint64_t rights, uint32_t flags);
pacha_fd_t pacha_vmo_create_page_view(pacha_fd_t parent_fd, const uint64_t *page_indices,
    uint64_t page_count, uint64_t rights, uint32_t flags);
int pacha_vmo_revoke(pacha_fd_t fd);
void *pacha_mmap(pacha_fd_t fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset);
void *pacha_mmap_anonymous(uint64_t size, uint64_t prot, uint64_t flags);
int pacha_munmap(void *addr, uint64_t size);
uint64_t pacha_mmap_pkey_flags(uint64_t flags, uint32_t pkey);

int pacha_ipc_pkey_supported(void);
int pacha_ipc_pkey_enabled(void);
uint32_t pacha_ipc_pkru_read(void);
void pacha_ipc_pkru_write(uint32_t pkru);
uint32_t pacha_ipc_pkey_disable_mask(uint32_t pkey);
void pacha_ipc_pkey_open(uint32_t pkey, uint32_t *saved_pkru);
void pacha_ipc_pkey_close(uint32_t pkey, uint32_t saved_pkru);

int pacha_ipc_fast_channel_init_local(struct pacha_ipc_fast_channel *fast, pacha_fd_t channel_fd, uint32_t flags, uint32_t pkey);
int pacha_ipc_fast_channel_init_normal(struct pacha_ipc_fast_channel *fast, pacha_fd_t channel_fd);
int pacha_ipc_fast_channel_offer(struct pacha_ipc_fast_channel *fast, pacha_fd_t control_fd, uint32_t flags, uint32_t pkey);
int pacha_ipc_fast_channel_accept(struct pacha_ipc_fast_channel *fast, pacha_fd_t control_fd, uint32_t flags, uint32_t pkey);
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

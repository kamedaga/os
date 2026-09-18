#include "lpr_socket.h"

#include "lpr_filed_internal.h"
#include "lpr_gui_detail.h"
#include "lpr_unix/poll.h"

#include "support/string.h"
#include "support/syscall.h"
#include <pacha/ipc.h>
#include <pacha/trace.h>
#include <pachaos/abi.h>
#include <personality/lpr_client_abi.h>
#include <personality/linux_lpr.h>
#include <stddef.h>
#include <stdint.h>

#define LPR_LINUX_AF_UNIX 1ull
#define LPR_LINUX_AF_INET 2ull
#define LPR_LINUX_AF_NETLINK 16ull
#define LPR_LINUX_SOCK_STREAM 1ull
#define LPR_LINUX_SOCK_DGRAM 2ull
#define LPR_LINUX_SOCK_RAW 3ull
#define LPR_LINUX_SOCK_NONBLOCK 00004000ull
#define LPR_LINUX_SOCK_CLOEXEC 02000000ull
#define LPR_LINUX_IPPROTO_TCP 6ull
#define LPR_LINUX_IPPROTO_UDP 17ull
#define LPR_LINUX_IPPROTO_IP 0ull
#define LPR_LINUX_IPPROTO_IPV6 41ull
#define LPR_LINUX_NETLINK_KOBJECT_UEVENT 15ull
#define LPR_LINUX_SOL_SOCKET 1ull
#define LPR_LINUX_SO_ERROR 4ull
#define LPR_LINUX_SO_REUSEADDR 2ull
#define LPR_LINUX_SO_KEEPALIVE 9ull
#define LPR_LINUX_SO_SNDBUF 7ull
#define LPR_LINUX_SO_RCVBUF 8ull
#define LPR_LINUX_SO_SNDTIMEO 21ull
#define LPR_LINUX_SO_RCVTIMEO 20ull
#define LPR_LINUX_SO_TYPE 3ull
#define LPR_LINUX_SO_DOMAIN 39ull
#define LPR_LINUX_SO_PROTOCOL 38ull
#define LPR_LINUX_TCP_NODELAY 1ull
#define LPR_LINUX_IP_TOS 1ull
#define LPR_LINUX_IP_TTL 2ull
#define LPR_LINUX_IPV6_V6ONLY 26ull
#define LPR_LINUX_F_DUPFD 0ull
#define LPR_LINUX_F_GETFD 1ull
#define LPR_LINUX_F_SETFD 2ull
#define LPR_LINUX_F_GETFL 3ull
#define LPR_LINUX_F_SETFL 4ull
#define LPR_LINUX_F_DUPFD_CLOEXEC 1030ull
#define LPR_LINUX_FD_CLOEXEC 1ull
#define LPR_LINUX_O_RDWR 00000002ull
#define LPR_LINUX_O_NONBLOCK 00004000ull
#define LPR_LINUX_O_CLOEXEC 02000000ull
#define LPR_LINUX_POLLIN 0x0001u
#define LPR_LINUX_POLLOUT 0x0004u
#define LPR_LINUX_POLLERR 0x0008u
#define LPR_LINUX_POLLHUP 0x0010u
#define LPR_LINUX_POLLNVAL 0x0020u
#define LPR_LINUX_MSG_PEEK 0x0002ull
#define LPR_LINUX_MSG_CTRUNC 0x0008u
#define LPR_LINUX_MSG_TRUNC 0x0020u
#define LPR_LINUX_MSG_DONTWAIT 0x0040ull
#define LPR_LINUX_MSG_NOSIGNAL 0x4000ull
#define LPR_LINUX_MSG_CMSG_CLOEXEC 0x40000000ull
#define LPR_LINUX_SCM_RIGHTS 1ull
#define LPR_LINUX_SO_PEERCRED 17ull
#define LPR_LINUX_SO_PEERPIDFD 77ull
#define LPR_LINUX_S_IFSOCK 0140000ull
#define LPR_LINUX_UIO_MAXIOV 1024u
#define LPR_LINUX_EMSGSIZE 90
#define LPR_NETD_DEFAULT_ADDR_BE 0x0f02000au
#define LPR_NETD_EPHEMERAL_PORT_BASE 49152u

#define LPR_SOCKET_HINT_PENDING 0x01u
#define LPR_SOCKET_HINT_READABLE 0x02u
#define LPR_SOCKET_HINT_HANGUP 0x04u
#define LPR_SOCKET_HINT_WRITABLE 0x08u

#define LPR_USER_LOW_GUARD_END 4096ull
#define LPR_USER_CANONICAL_END 0x0000800000000000ull

#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
#define LPR_NETD_PROFILE_OPS 16u

typedef struct lpr_netd_profile_metric {
    uint64_t count;
    uint64_t total_cycles;
    uint64_t max_cycles;
} lpr_netd_profile_metric_t;

enum lpr_netd_profile_stage {
    LPR_NETD_PROFILE_LOCK = 1,
    LPR_NETD_PROFILE_CALL = 2,
    LPR_NETD_PROFILE_RECV = 3,
    LPR_NETD_PROFILE_CLOSE = 4,
    LPR_NETD_PROFILE_TOTAL = 5,
};

static lpr_netd_profile_metric_t lpr_netd_profile_lock;
static lpr_netd_profile_metric_t
    lpr_netd_profile_stages[4][LPR_NETD_PROFILE_OPS];

static void lpr_netd_profile_record(
    lpr_netd_profile_metric_t *metric,
    uint64_t start)
{
    const uint64_t end = pacha_trace_read_tsc();
    if (metric == 0 || end < start) return;
    const uint64_t elapsed = end - start;
    __atomic_fetch_add(&metric->count, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&metric->total_cycles, elapsed, __ATOMIC_RELAXED);
    uint64_t maximum = __atomic_load_n(
        &metric->max_cycles, __ATOMIC_RELAXED);
    while (elapsed > maximum && !__atomic_compare_exchange_n(
        &metric->max_cycles,
        &maximum,
        elapsed,
        0,
        __ATOMIC_RELAXED,
        __ATOMIC_RELAXED))
    {
    }
}

static lpr_netd_profile_metric_t *lpr_netd_profile_stage_metric(
    enum lpr_netd_profile_stage stage,
    uint64_t op)
{
    if (stage < LPR_NETD_PROFILE_CALL ||
        stage > LPR_NETD_PROFILE_TOTAL ||
        op >= LPR_NETD_PROFILE_OPS)
        return 0;
    return &lpr_netd_profile_stages[stage - LPR_NETD_PROFILE_CALL][op];
}

static void lpr_netd_profile_record_stage(
    enum lpr_netd_profile_stage stage,
    uint64_t op,
    uint64_t start)
{
    lpr_netd_profile_record(
        lpr_netd_profile_stage_metric(stage, op), start);
}

void lpr_netd_profile_dump(void)
{
    pacha_trace5(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_NETD_CALL,
        PACHA_TRACE_CLASS_METRIC,
        LPR_NETD_PROFILE_LOCK,
        0,
        __atomic_load_n(&lpr_netd_profile_lock.count, __ATOMIC_RELAXED),
        __atomic_load_n(
            &lpr_netd_profile_lock.total_cycles, __ATOMIC_RELAXED),
        __atomic_load_n(
            &lpr_netd_profile_lock.max_cycles, __ATOMIC_RELAXED));
    for (uint64_t op = 0; op < LPR_NETD_PROFILE_OPS; ++op) {
        for (uint64_t stage = LPR_NETD_PROFILE_CALL;
             stage <= LPR_NETD_PROFILE_TOTAL;
             ++stage)
        {
            lpr_netd_profile_metric_t *metric =
                lpr_netd_profile_stage_metric(
                    (enum lpr_netd_profile_stage)stage, op);
            const uint64_t count = __atomic_load_n(
                &metric->count, __ATOMIC_RELAXED);
            if (count == 0) continue;
            pacha_trace5(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_NETD_CALL,
                PACHA_TRACE_CLASS_METRIC,
                stage,
                op,
                count,
                __atomic_load_n(
                    &metric->total_cycles, __ATOMIC_RELAXED),
                __atomic_load_n(
                    &metric->max_cycles, __ATOMIC_RELAXED));
        }
    }
}
#else
void lpr_netd_profile_dump(void)
{
}
#endif

int lpr_user_range_plausible(uint64_t ptr, uint64_t bytes)
{
    if (bytes == 0) {
        return 1;
    }
    if (ptr < LPR_USER_LOW_GUARD_END) {
        return 0;
    }
    if (ptr > UINT64_MAX - (bytes - 1u)) {
        return 0;
    }
    const uint64_t end = ptr + bytes - 1u;
    return ptr < LPR_USER_CANONICAL_END && end < LPR_USER_CANONICAL_END;
}

typedef struct lpr_linux_sockaddr_in {
    uint16_t family;
    uint16_t port_be;
    uint32_t addr_be;
    uint8_t zero[8];
} lpr_linux_sockaddr_in_t;

typedef struct lpr_linux_sockaddr_un {
    uint16_t family;
    char path[108];
} lpr_linux_sockaddr_un_t;



typedef struct lpr_linux_sockaddr_nl {
    uint16_t family;
    uint16_t pad;
    uint32_t pid;
    uint32_t groups;
} lpr_linux_sockaddr_nl_t;



typedef struct lpr_linux_msghdr {
    uint64_t msg_name;
    uint32_t msg_namelen;
    uint32_t pad0;
    uint64_t msg_iov;
    uint64_t msg_iovlen;
    uint64_t msg_control;
    uint64_t msg_controllen;
    uint32_t msg_flags;
    uint32_t pad1;
} lpr_linux_msghdr_t;

typedef struct lpr_linux_mmsghdr {
    lpr_linux_msghdr_t msg_hdr;
    uint32_t msg_len;
    uint32_t pad0;
} lpr_linux_mmsghdr_t;

typedef struct lpr_linux_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
} lpr_linux_pollfd_t;

typedef struct lpr_linux_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
} lpr_linux_timeval_t;

typedef struct lpr_linux_pselect_sigmask {
    uint64_t sigmask;
    uint64_t sigsetsize;
} lpr_linux_pselect_sigmask_t;

static int64_t lpr_linux_socket_wait_events(uint64_t fd, uint32_t events, int32_t timeout_ms);
static int64_t lpr_linux_socket_wait_events_until(
    uint64_t fd,
    uint32_t events,
    lpr_wait_deadline_t *deadline);

int lpr_fd_slot_alloc_from(uint64_t min_fd);
int lpr_control_install_fd(uint64_t fd, uint8_t ops_id, uint64_t linux_flags, uint64_t backend_id, uint64_t offset);
void lpr_control_close_fd(uint64_t fd);
int lpr_control_set_fd_flags(uint64_t fd, uint64_t flags);
int lpr_control_set_status_flags(uint64_t fd, uint64_t flags);
int64_t lpr_control_get_fd_flags(uint64_t fd);
int64_t lpr_control_get_status_flags(uint64_t fd, uint32_t access_mode);
static int64_t lpr_negative_status(int64_t status)
{
    return status < 0 ? status : -LPR_LINUX_EIO;
}

static void lpr_socket_debug_connect(const char *phase, uint32_t addr_be, uint16_t port_be, int64_t status)
{
    pacha_trace4(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_SOCKET_CONNECT,
        PACHA_TRACE_CLASS_DEBUG,
        pacha_trace_name_id(phase),
        addr_be,
        port_be,
        (uint64_t)status);
}

static void lpr_netd_debug_call(const char *phase, uint64_t op, uint64_t request_id, int64_t status, uint64_t result)
{
    if (op != NETD_OP_POLL && op != NETD_OP_CONNECT) {
        return;
    }
    pacha_trace5(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_NETD_CALL,
        PACHA_TRACE_CLASS_DEBUG,
        pacha_trace_name_id(phase),
        op,
        request_id,
        (uint64_t)status,
        result);
}

static int lpr_socket_connect_target_supported(uint32_t addr_be)
{
    const uint8_t *addr = (const uint8_t *)&addr_be;
    if (addr[0] == 10 && !(addr[1] == 0 && addr[2] == 2)) {
        return 0;
    }
    return 1;
}

static int lpr_socket_op_nonblocking(uint64_t fd, uint64_t flags)
{
    return (flags & LPR_LINUX_MSG_DONTWAIT) != 0 ||
           (lpr_socket_backend(fd)->flags & LPR_LINUX_O_NONBLOCK) != 0;
}

static int64_t lpr_socket_timeval_to_ms(const lpr_linux_timeval_t *tv, int64_t *out_ms)
{
    if (tv == 0 || out_ms == 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000ll) {
        return -LPR_LINUX_EINVAL;
    }
    if (tv->tv_sec > 9223372036854ll) {
        *out_ms = 9223372036854775807ll;
    } else {
        *out_ms = tv->tv_sec * 1000ll + (tv->tv_usec + 999ll) / 1000ll;
    }
    return 0;
}

static int64_t lpr_socket_timespec_to_ms(const lpr_linux_timespec_t *ts, int64_t *out_ms)
{
    if (ts == 0 || out_ms == 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000ll) {
        return -LPR_LINUX_EINVAL;
    }
    if (ts->tv_sec > 9223372036854775ll) {
        *out_ms = 9223372036854775807ll;
    } else {
        *out_ms = ts->tv_sec * 1000ll + (ts->tv_nsec + 999999ll) / 1000000ll;
    }
    return 0;
}

static uint64_t lpr_fdset_word_count(uint64_t nfds)
{
    return (nfds + 63u) / 64u;
}

static int lpr_fdset_test(const uint64_t *set, uint64_t fd)
{
    return (set[fd / 64u] & (1ull << (fd % 64u))) != 0;
}

static void lpr_fdset_clear(uint64_t *set, uint64_t fd)
{
    set[fd / 64u] &= ~(1ull << (fd % 64u));
}

static void lpr_socket_trace_socket(uint64_t domain, uint64_t type, uint64_t protocol, int64_t result)
{
    pacha_trace4(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_SOCKET_CREATE,
        PACHA_TRACE_CLASS_DEBUG,
        domain,
        type,
        protocol,
        (uint64_t)result);
}

static int lpr_netd_endpoint_ready(void)
{
    if (__atomic_load_n(
            &lpr_netd_endpoint_checked, __ATOMIC_ACQUIRE) != 0)
        return 0;
    struct pacha_fd_info info;
    lpr_memset(&info, 0, sizeof(info));
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_FD_GET_INFO,
        LPR_NETD_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&info);
    if (status != 0 || info.kind != PACHA_FD_KIND_ENDPOINT) {
        return -LPR_LINUX_ENETDOWN;
    }
    __atomic_store_n(
        &lpr_netd_endpoint_checked, 1u, __ATOMIC_RELEASE);
    return 0;
}

static void lpr_netd_page_pool_wake(void)
{
    __atomic_add_fetch(
        &lpr_state.netd_rpc.page_pool_epoch, 1u, __ATOMIC_RELEASE);
    (void)lpr_pacha_syscall2(
        PACHA_RUNTIME_SYSCALL_FUTEX_WAKE,
        (uint64_t)(uintptr_t)&lpr_state.netd_rpc.page_pool_epoch,
        1);
}

static void lpr_netd_page_pool_wait(uint32_t epoch)
{
    (void)lpr_pacha_syscall3(
        PACHA_RUNTIME_SYSCALL_FUTEX_WAIT,
        (uint64_t)(uintptr_t)&lpr_state.netd_rpc.page_pool_epoch,
        epoch,
        0);
}

static void lpr_netd_page_pool_lock(void)
{
    lpr_state_lock(&lpr_state.netd_rpc.lock_word);
}

static void lpr_netd_page_pool_unlock(void)
{
    lpr_state_unlock(&lpr_state.netd_rpc.lock_word);
}

static void lpr_netd_page_slot_release(lpr_netd_page_slot_t *slot)
{
    if (slot == 0) return;
    lpr_netd_page_pool_lock();
    slot->busy = 0;
    lpr_netd_page_pool_unlock();
    lpr_netd_page_pool_wake();
}

static lpr_netd_page_slot_t *lpr_netd_page_slot_owned(int page_fd)
{
    lpr_netd_page_slot_t *found = 0;
    lpr_netd_page_pool_lock();
    for (uint32_t i = 0; i < LPR_NETD_PAGE_POOL_SLOTS; ++i) {
        lpr_netd_page_slot_t *slot = &lpr_state.netd_rpc.page_slots[i];
        if (slot->busy && slot->page_fd == page_fd && slot->page != 0) {
            found = slot;
            break;
        }
    }
    lpr_netd_page_pool_unlock();
    return found;
}

static int lpr_netd_create_page(void **out_page)
{
    if (out_page == 0) return -LPR_LINUX_EINVAL;

    lpr_netd_page_slot_t *reserved = 0;
    for (;;) {
#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
        const uint64_t lock_start = pacha_trace_read_tsc();
#endif
        lpr_netd_page_pool_lock();
#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
        lpr_netd_profile_record(&lpr_netd_profile_lock, lock_start);
#endif
        for (uint32_t i = 0; i < LPR_NETD_PAGE_POOL_SLOTS; ++i) {
            lpr_netd_page_slot_t *slot = &lpr_state.netd_rpc.page_slots[i];
            if (!slot->busy && slot->page_fd >= 16 && slot->page != 0) {
                slot->busy = 1;
                *out_page = slot->page;
                const int page_fd = slot->page_fd;
                lpr_netd_page_pool_unlock();
                return page_fd;
            }
        }
        for (uint32_t i = 0; i < LPR_NETD_PAGE_POOL_SLOTS; ++i) {
            lpr_netd_page_slot_t *slot = &lpr_state.netd_rpc.page_slots[i];
            if (!slot->busy && slot->page_fd < 16 && slot->page == 0) {
                slot->busy = 1;
                slot->lease_fd = -1;
                reserved = slot;
                break;
            }
        }
        if (reserved != 0) {
            lpr_netd_page_pool_unlock();
            break;
        }
        const uint32_t epoch = __atomic_load_n(
            &lpr_state.netd_rpc.page_pool_epoch, __ATOMIC_ACQUIRE);
        lpr_netd_page_pool_unlock();
        lpr_netd_page_pool_wait(epoch);
    }

    const uint64_t rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        NETD_PAGE_BYTES,
        rights,
        PACHA_FD_FLAG_CLOEXEC);
    if (fd < 16) {
        lpr_netd_page_slot_release(reserved);
        return (int)lpr_negative_status(fd);
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)fd,
        0,
        NETD_PAGE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
        lpr_netd_page_slot_release(reserved);
        return (int)lpr_negative_status(mapped);
    }
    reserved->page_fd = (int)fd;
    reserved->page = (void *)(uintptr_t)mapped;
    *out_page = reserved->page;
    return (int)fd;
}

static void lpr_netd_destroy_page(int fd, void *page)
{
    lpr_netd_page_slot_t *slot = lpr_netd_page_slot_owned(fd);
    if (slot != 0 && slot->page == page) {
        lpr_netd_page_slot_release(slot);
        return;
    }
    if (page != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page,
            NETD_PAGE_BYTES);
    }
    if (fd >= 16) {
        (void)lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
    }
}

void lpr_netd_page_pool_after_fork_child(void)
{
    for (uint32_t i = 0; i < LPR_NETD_PAGE_POOL_SLOTS; ++i) {
        lpr_netd_page_slot_t *slot = &lpr_state.netd_rpc.page_slots[i];
        if (slot->lease_fd >= 16) {
            (void)lpr_pacha_syscall1(
                PACHAOS_SYSCALL_FD_CLOSE,
                (uint64_t)(uint32_t)slot->lease_fd);
        }
        if (slot->page != 0) {
            (void)lpr_pacha_syscall2(
                PACHAOS_SYSCALL_MUNMAP,
                (uint64_t)(uintptr_t)slot->page,
                NETD_PAGE_BYTES);
        }
        if (slot->page_fd >= 16) {
            (void)lpr_pacha_syscall1(
                PACHAOS_SYSCALL_FD_CLOSE,
                (uint64_t)(uint32_t)slot->page_fd);
        }
        lpr_memset(slot, 0, sizeof(*slot));
    }
    __atomic_store_n(
        &lpr_state.netd_rpc.page_pool_epoch, 0u, __ATOMIC_RELEASE);
}

typedef struct lpr_netd_fd_options {
    const int *transfer_fds;
    uint32_t transfer_count;
    int move_transfer;
    int *out_received_fds;
    uint32_t received_capacity;
    uint32_t *out_received_count;
} lpr_netd_fd_options_t;

static int64_t lpr_netd_exchange(
    uint64_t op,
    uint64_t word2,
    struct pacha_ipc_fd *fd_items,
    uint64_t fd_count,
    uint64_t *out_result,
    int *out_received_fds,
    uint32_t received_capacity,
    uint32_t *out_received_count)
{
#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
    const uint64_t total_start = pacha_trace_read_tsc();
#endif
    const uint64_t request_id = lpr_next_request_id(&lpr_netd_request_id);
    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = op,
        .word2 = word2,
        .word3 = request_id,
        .fds = fd_count != 0 ? fd_items : 0,
        .fd_count = fd_count,
    };
    lpr_netd_debug_call("call_begin", op, request_id, 0, 0);
#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
    const uint64_t call_start = pacha_trace_read_tsc();
#endif
    /* Endpoint backpressure is a transport condition, not the EAGAIN result
     * of the requested socket operation.  In particular, returning it after
     * the caller has moved a Unix doorbell acknowledgement into the request
     * loses the only copy of that acknowledgement and can suppress every
     * later edge. */
    const int64_t reply_fd = lpr_native_ipc_call_wait(
        LPR_NETD_ENDPOINT_FD, &request);
#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
    lpr_netd_profile_record_stage(
        LPR_NETD_PROFILE_CALL, op, call_start);
#endif
    lpr_netd_debug_call("call_end", op, request_id, reply_fd, 0);
    if (reply_fd < 16) {
#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
        lpr_netd_profile_record_stage(
            LPR_NETD_PROFILE_TOTAL, op, total_start);
#endif
        return lpr_negative_status(reply_fd);
    }

    struct pacha_ipc_msg reply;
    struct pacha_ipc_fd reply_fd_items[NETD_TRANSFER_MAX_CAPABILITIES];
    lpr_memset(&reply, 0, sizeof(reply));
    lpr_memset(reply_fd_items, 0, sizeof(reply_fd_items));
    if (out_received_count != 0) *out_received_count = 0;
    for (uint32_t i = 0; i < received_capacity; ++i) out_received_fds[i] = -1;
    if (received_capacity != 0) {
        reply.fds = reply_fd_items;
        reply.fd_capacity = received_capacity;
    }
    lpr_netd_debug_call("recv_begin", op, request_id, reply_fd, 0);
#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
    const uint64_t recv_start = pacha_trace_read_tsc();
#endif
    const int64_t recv_status = lpr_native_ipc_recv_wait(
        (uint64_t)(uint32_t)reply_fd,
        &reply);
#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
    lpr_netd_profile_record_stage(
        LPR_NETD_PROFILE_RECV, op, recv_start);
#endif
    lpr_netd_debug_call("recv_end", op, request_id, recv_status, reply.word2);
#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
    const uint64_t close_start = pacha_trace_read_tsc();
#endif
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
    lpr_netd_profile_record_stage(
        LPR_NETD_PROFILE_CLOSE, op, close_start);
    lpr_netd_profile_record_stage(
        LPR_NETD_PROFILE_TOTAL, op, total_start);
#endif
    if (recv_status != 0) {
        return lpr_negative_status(recv_status);
    }
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC || reply.word3 != request_id) {
        for (uint64_t i = 0; i < reply.fd_count && i < received_capacity; ++i)
            if (reply_fd_items[i].fd >= 16)
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, reply_fd_items[i].fd);
        return -LPR_LINUX_EIO;
    }
    if (reply.fd_count > received_capacity) return -LPR_LINUX_EIO;
    for (uint64_t i = 0; i < reply.fd_count; ++i) {
        if (reply_fd_items[i].fd < 16) {
            for (uint64_t j = 0; j < i; ++j)
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, reply_fd_items[j].fd);
            return -LPR_LINUX_EIO;
        }
        out_received_fds[i] = (int)(uint32_t)reply_fd_items[i].fd;
    }
    if (out_received_count != 0) *out_received_count = (uint32_t)reply.fd_count;
    if (out_result != 0) {
        *out_result = reply.word2;
    }
    return (int64_t)reply.word1;
}

static void lpr_netd_page_attachment_drop(lpr_netd_page_slot_t *slot)
{
    if (slot == 0) return;
    if (slot->lease_fd >= 16) {
        (void)lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE,
            (uint64_t)(uint32_t)slot->lease_fd);
    }
    slot->lease_fd = -1;
    slot->attachment_id = 0;
}

static int64_t lpr_netd_page_attach(int page_fd)
{
    lpr_netd_page_slot_t *slot = lpr_netd_page_slot_owned(page_fd);
    if (slot == 0) return -LPR_LINUX_EINVAL;
    if (slot->attachment_id != 0 && slot->lease_fd >= 16)
        return 0;
    lpr_netd_page_attachment_drop(slot);

    const uint64_t channel_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_SEND |
        PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_TRANSFER;
    uint64_t pair[2] = {0, 0};
    const int64_t pair_status = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_IPC_CHANNEL_CREATE,
        (uint64_t)(uintptr_t)pair,
        channel_rights,
        PACHA_FD_FLAG_CLOEXEC);
    if (pair_status != 0 || pair[0] < 16 || pair[1] < 16 ||
        pair[0] > INT32_MAX || pair[1] > INT32_MAX)
    {
        if (pair[0] >= 16)
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        if (pair[1] >= 16)
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        return pair_status != 0 ? lpr_negative_status(pair_status) :
            -LPR_LINUX_EMFILE;
    }

    struct pacha_ipc_fd attach_fds[2];
    lpr_memset(attach_fds, 0, sizeof(attach_fds));
    attach_fds[0].fd = (uint64_t)(uint32_t)page_fd;
    attach_fds[0].rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    attach_fds[1].fd = pair[1];
    attach_fds[1].rights = channel_rights;
    /* Retain this sender FD until the close below. MOVE would release its
     * number during IPC_CALL, allowing another thread to reuse it for a
     * reply or socket before attachment setup closes it again.
     */
    attach_fds[1].transfer_flags = 0;

    uint64_t attachment_id = 0;
    const int64_t status = lpr_netd_exchange(
        NETD_OP_PAGE_ATTACH,
        0,
        attach_fds,
        2,
        &attachment_id,
        0,
        0,
        0);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
    if (status != 0 || attachment_id == 0) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        return status != 0 ? status : -LPR_LINUX_EIO;
    }
    slot->lease_fd = (int)(uint32_t)pair[0];
    slot->attachment_id = attachment_id;
    return 0;
}

static int64_t lpr_netd_call_with_fd(
    uint64_t op,
    int page_fd,
    uint64_t word2,
    uint64_t *out_result,
    const lpr_netd_fd_options_t *fd_options)
{
    const int *const transfer_fds = fd_options != 0 ? fd_options->transfer_fds : 0;
    const uint32_t transfer_count = fd_options != 0 ? fd_options->transfer_count : 0;
    const int move_transfer = fd_options != 0 ? fd_options->move_transfer : 0;
    int *const out_received_fds = fd_options != 0 ? fd_options->out_received_fds : 0;
    const uint32_t received_capacity = fd_options != 0 ? fd_options->received_capacity : 0;
    uint32_t *const out_received_count = fd_options != 0 ? fd_options->out_received_count : 0;
    if (transfer_count > NETD_TRANSFER_MAX_CAPABILITIES ||
        received_capacity > NETD_TRANSFER_MAX_CAPABILITIES ||
        (transfer_count != 0 && transfer_fds == 0) ||
        (received_capacity != 0 && (out_received_fds == 0 || out_received_count == 0)))
        return -LPR_LINUX_EINVAL;
    const int ready = lpr_netd_endpoint_ready();
    if (ready != 0) return ready;

    struct pacha_ipc_fd fd_items[NETD_TRANSFER_MAX_CAPABILITIES];
    lpr_memset(fd_items, 0, sizeof(fd_items));
    for (uint32_t i = 0; i < transfer_count; ++i) {
        if (transfer_fds[i] < 16) return -LPR_LINUX_EBADF;
        struct pacha_fd_info info;
        lpr_memset(&info, 0, sizeof(info));
        if (!lpr_native_fd_info((uint64_t)(uint32_t)transfer_fds[i], &info) ||
            (info.rights & PACHA_FD_RIGHT_TRANSFER) == 0)
            return -LPR_LINUX_EBADF;
        fd_items[i].fd = (uint64_t)(uint32_t)transfer_fds[i];
        fd_items[i].rights = info.rights;
        fd_items[i].transfer_flags = move_transfer ? PACHA_IPC_TRANSFER_MOVE : 0;
    }

    for (unsigned attempt = 0; attempt < 2u; ++attempt) {
        uint64_t request_word2 = word2;
        lpr_netd_page_slot_t *page_slot = 0;
        if (page_fd >= 16) {
            page_slot = lpr_netd_page_slot_owned(page_fd);
            if (page_slot == 0) return -LPR_LINUX_EINVAL;
            const int64_t attach_status = lpr_netd_page_attach(page_fd);
            if (attach_status != 0) return attach_status;
            request_word2 = page_slot->attachment_id;
            __atomic_thread_fence(__ATOMIC_RELEASE);
        }
        const int64_t status = lpr_netd_exchange(
            op,
            request_word2,
            transfer_count != 0 ? fd_items : 0,
            transfer_count,
            out_result,
            out_received_fds,
            received_capacity,
            out_received_count);
        if (page_fd < 16) return status;

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (status != NETD_STATUS_STALE_ATTACHMENT) return status;
        lpr_netd_page_attachment_drop(page_slot);

        /* A stale attachment is rejected before dispatch, so the page still
         * contains the exact network operation. Retry it after
         * attaching the page again.  MOVE transfers cannot be replayed because
         * their sender-side descriptors were consumed by the first IPC_CALL. */
        if (move_transfer || attempt != 0u) return status;
    }
    return NETD_STATUS_STALE_ATTACHMENT;
}

static int64_t lpr_netd_call(uint64_t op, int page_fd, uint64_t word2, uint64_t *out_result)
{
    return lpr_netd_call_with_fd(op, page_fd, word2, out_result, 0);
}

int64_t lpr_netd_dup_handle(uint64_t handle)
{
    return handle != 0 ? lpr_netd_call(NETD_OP_DUP, -1, handle, 0) :
        -LPR_LINUX_EBADF;
}


/* Network OFD handoff across exec; AF_UNIX SCM uses unixd instead. */
int64_t lpr_netd_transfer_dup_handle(uint64_t handle, int lease_fd)
{
    if (handle == 0 || lease_fd < 16) return -LPR_LINUX_EINVAL;
    const lpr_netd_fd_options_t options = {
        .transfer_fds = &lease_fd, .transfer_count = 1, .move_transfer = 1 };
    return lpr_netd_call_with_fd(NETD_OP_DUP, -1, handle, 0, &options);
}

int64_t lpr_netd_close_handle(uint64_t handle)
{
    return handle != 0 ? lpr_netd_call(NETD_OP_CLOSE, -1, handle, 0) :
        -LPR_LINUX_EBADF;
}

int lpr_linux_socket_fd_active(uint64_t fd)
{
    lpr_socket_backend_t *socket = lpr_socket_backend(fd);
    return socket != 0 && socket->active;
}

int lpr_linux_socket_fd_cloexec(uint64_t fd)
{
    return lpr_linux_socket_fd_active(fd) &&
        (lpr_control_get_fd_flags(fd) & LPR_LINUX_FD_CLOEXEC) != 0;
}

int lpr_linux_socket_native_wait_fd(uint64_t fd)
{
    lpr_socket_backend_t *socket = lpr_socket_backend(fd);
    return socket != 0 && socket->active ? socket->wait_fd.raw : -1;
}

void lpr_linux_socket_mark_readable(uint64_t fd)
{
    lpr_socket_backend_t *socket = lpr_socket_backend(fd);
    if (socket != 0 && socket->active)
        __atomic_fetch_or(
            &socket->readable_hint,
            LPR_SOCKET_HINT_PENDING,
            __ATOMIC_RELEASE);
}







static int lpr_socket_alloc_fd(void)
{
    return lpr_fd_slot_alloc_from(3);
}

static uint16_t lpr_socket_htons(uint16_t value)
{
    return (uint16_t)((value << 8u) | (value >> 8u));
}

static uint16_t lpr_socket_next_port_be(void)
{
    uint16_t port = lpr_next_ephemeral_port++;
    if (lpr_next_ephemeral_port < LPR_NETD_EPHEMERAL_PORT_BASE) {
        lpr_next_ephemeral_port = LPR_NETD_EPHEMERAL_PORT_BASE;
    }
    return lpr_socket_htons(port);
}

static int64_t lpr_socket_install_endpoint(
    uint64_t domain,
    uint64_t type,
    uint64_t protocol,
    uint64_t flags,
    uint64_t handle,
    int native_wait_fd,
    int connected,
    int32_t peer_pid,
    uint32_t peer_uid,
    uint32_t peer_gid)
{
    const int fd = lpr_socket_alloc_fd();
    if (fd < 0) {
        (void)lpr_netd_call(NETD_OP_CLOSE, -1, handle, 0);
        if (native_wait_fd >= 16)
            (void)lpr_pacha_syscall1(
                PACHAOS_SYSCALL_FD_CLOSE,
                (uint64_t)(uint32_t)native_wait_fd);
        return fd;
    }
    const uint64_t linux_flags =
        LPR_LINUX_O_RDWR |
        ((flags & LPR_LINUX_SOCK_NONBLOCK) != 0 ? LPR_LINUX_O_NONBLOCK : 0) |
        ((flags & LPR_LINUX_SOCK_CLOEXEC) != 0 ? LPR_LINUX_O_CLOEXEC : 0);
    const int install_status = lpr_control_install_fd(
        (uint64_t)(uint32_t)fd,
        LPR_FD_OPS_SOCKET,
        linux_flags,
        handle,
        0);
    if (install_status != 0) {
        (void)lpr_netd_call(NETD_OP_CLOSE, -1, handle, 0);
        if (native_wait_fd >= 16)
            (void)lpr_pacha_syscall1(
                PACHAOS_SYSCALL_FD_CLOSE,
                (uint64_t)(uint32_t)native_wait_fd);
        return install_status;
    }
    lpr_socket_backend_t *socket = lpr_socket_backend((uint64_t)(uint32_t)fd);
    if (socket == 0) {
        lpr_control_close_fd((uint64_t)(uint32_t)fd);
        if (native_wait_fd >= 16)
            (void)lpr_pacha_syscall1(
                PACHAOS_SYSCALL_FD_CLOSE,
                (uint64_t)(uint32_t)native_wait_fd);
        return -LPR_LINUX_EIO;
    }
    socket->type = (uint8_t)type;
    socket->readable_hint = 0;
    socket->write_blocked = 0;
    socket->connected = connected != 0;
    socket->connecting = 0;
    socket->domain = (uint8_t)domain;
    socket->protocol = (uint16_t)protocol;
    socket->flags =
        LPR_LINUX_O_RDWR |
        ((flags & LPR_LINUX_SOCK_NONBLOCK) != 0 ? LPR_LINUX_O_NONBLOCK : 0);
    socket->sndbuf = 256u * 1024u;
    socket->rcvbuf = 256u * 1024u;
    socket->reuseaddr = 0;
    socket->keepalive = 0;
    socket->tcp_nodelay = 0;
    socket->sndtimeo_ms = 0;
    socket->rcvtimeo_ms = 0;
    socket->last_error = 0;
    socket->wait_fd.raw = native_wait_fd;
    socket->local_addr_be = 0;
    socket->local_port_be = lpr_socket_next_port_be();
    socket->peer_addr_be = 0;
    socket->peer_port_be = 0;
    socket->peer_pid = peer_pid;
    socket->peer_uid = peer_uid;
    socket->peer_gid = peer_gid;
    return fd;
}

static int64_t lpr_socket_copy_sockaddr(uint64_t addr_raw, uint64_t addrlen_raw, uint32_t addr_be, uint16_t port_be)
{
    if (addr_raw == 0 || addrlen_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    uint32_t *addrlen = (uint32_t *)(uintptr_t)addrlen_raw;
    const uint32_t requested = *addrlen;
    if (requested < sizeof(lpr_linux_sockaddr_in_t)) {
        *addrlen = sizeof(lpr_linux_sockaddr_in_t);
        return -LPR_LINUX_EINVAL;
    }
    lpr_linux_sockaddr_in_t *addr = (lpr_linux_sockaddr_in_t *)(uintptr_t)addr_raw;
    lpr_memset(addr, 0, sizeof(*addr));
    addr->family = LPR_LINUX_AF_INET;
    addr->port_be = port_be;
    addr->addr_be = addr_be;
    *addrlen = sizeof(*addr);
    return 0;
}





int64_t lpr_linux_socket(uint64_t domain, uint64_t type, uint64_t protocol)
{
    if (domain == LPR_LINUX_AF_UNIX) return lpr_unix_socket_create(type, protocol);
    if (domain != LPR_LINUX_AF_INET &&
        domain != LPR_LINUX_AF_NETLINK) {
        const int64_t result = -LPR_LINUX_EAFNOSUPPORT;
        lpr_socket_trace_socket(domain, type, protocol, result);
        return result;
    }
    const uint64_t flags = type & (LPR_LINUX_SOCK_NONBLOCK | LPR_LINUX_SOCK_CLOEXEC);
    type &= ~(LPR_LINUX_SOCK_NONBLOCK | LPR_LINUX_SOCK_CLOEXEC);
    uint64_t netd_type = 0;
    if (type == LPR_LINUX_SOCK_STREAM) {
        netd_type = NETD_SOCK_STREAM;
        if (domain == LPR_LINUX_AF_INET && protocol == 0) {
            protocol = LPR_LINUX_IPPROTO_TCP;
        }
    } else if (type == LPR_LINUX_SOCK_DGRAM ||
        (type == LPR_LINUX_SOCK_RAW && domain == LPR_LINUX_AF_NETLINK)) {
        netd_type = type == LPR_LINUX_SOCK_RAW ? NETD_SOCK_RAW : NETD_SOCK_DGRAM;
        if (protocol == 0 && domain == LPR_LINUX_AF_INET) {
            protocol = LPR_LINUX_IPPROTO_UDP;
        }
    } else {
        const int64_t result = -LPR_LINUX_ESOCKTNOSUPPORT;
        lpr_socket_trace_socket(domain, type, protocol, result);
        return result;
    }

    void *page = 0;
    const int page_fd = lpr_netd_create_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memset(page, 0, sizeof(netd_socket_t));
    netd_socket_t *req = (netd_socket_t *)page;
    req->domain = domain == LPR_LINUX_AF_NETLINK ? NETD_AF_NETLINK : NETD_AF_INET;
    req->type = netd_type;
    req->protocol = protocol;
    req->flags = flags;
    int native_wait_fd = -1;
    int remote_wait_fd = -1;
    const int notification_status = lpr_native_wait_pair(
        &native_wait_fd, &remote_wait_fd);
    if (notification_status != 0) {
        lpr_netd_destroy_page(page_fd, page);
        return notification_status;
    }
    uint64_t handle = 0;
    const lpr_netd_fd_options_t fd_options = {
        .transfer_fds = &remote_wait_fd,
        .transfer_count = remote_wait_fd >= 16 ? 1u : 0u,
        .move_transfer = 1,
    };
    const int64_t status = lpr_netd_call_with_fd(
        NETD_OP_SOCKET, page_fd, 0, &handle, &fd_options);
    lpr_netd_destroy_page(page_fd, page);
    if (status != 0) {
        if (native_wait_fd >= 16)
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)native_wait_fd);
        if (remote_wait_fd >= 16)
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)remote_wait_fd);
        lpr_socket_trace_socket(domain, type, protocol, status);
        return status;
    }

    const int64_t fd = lpr_socket_install_endpoint(
        domain, type, protocol, flags, handle, native_wait_fd,
        0, 0, 0, 0);
    lpr_socket_trace_socket(domain, type, protocol, fd);
    return fd;
}

int64_t lpr_linux_socketpair(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t sockets_raw)
{
    return domain == LPR_LINUX_AF_UNIX ? lpr_unix_socket_pair(type, protocol, sockets_raw) : -LPR_LINUX_EAFNOSUPPORT;
}

int64_t lpr_linux_socket_close(uint64_t fd)
{
    return lpr_linux_close(fd);
}

int64_t lpr_socket_close_backend(void *state)
{
    const lpr_socket_backend_t *socket = state;
    const int transfer_lease =
        (socket->reserved1 & LPR_BACKEND_TRANSFER_LEASE) != 0;
    const int64_t status = !transfer_lease && socket->handle != 0 ?
        lpr_netd_call(NETD_OP_CLOSE, -1, socket->handle, 0) : 0;
    if (socket->wait_fd.raw >= 16) {
        (void)lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE,
            (uint64_t)(uint32_t)socket->wait_fd.raw);
    }
    const int64_t lease_status = socket->lease_fd.raw >= 16 ?
        lpr_close_native_fd_if_open(
            (uint64_t)(uint32_t)socket->lease_fd.raw) : 0;
    return status != 0 ? status : lease_status;
}

int64_t lpr_linux_connect(uint64_t fd, uint64_t addr_raw, uint64_t addrlen)
{
    if (lpr_unix_socket_active(fd)) return lpr_unix_socket_address(fd, UNIX_OP_CONNECT, addr_raw, addrlen);
    if (!lpr_linux_socket_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (addr_raw == 0 || addrlen < sizeof(uint16_t)) {
        return -LPR_LINUX_EINVAL;
    }

    if (lpr_socket_backend(fd)->domain == LPR_LINUX_AF_NETLINK)
        return -LPR_LINUX_EOPNOTSUPP;
    if (addrlen < sizeof(lpr_linux_sockaddr_in_t)) return -LPR_LINUX_EINVAL;
    const lpr_linux_sockaddr_in_t *addr = (const lpr_linux_sockaddr_in_t *)(uintptr_t)addr_raw;
    if (addr->family != LPR_LINUX_AF_INET) {
        return -LPR_LINUX_EAFNOSUPPORT;
    }
    if (!lpr_socket_connect_target_supported(addr->addr_be)) {
        lpr_socket_debug_connect("reject", addr->addr_be, addr->port_be, -LPR_LINUX_ENETUNREACH);
        lpr_socket_backend(fd)->connected = 0;
        lpr_socket_backend(fd)->connecting = 0;
        lpr_socket_backend(fd)->last_error = LPR_LINUX_ENETUNREACH;
        return -LPR_LINUX_ENETUNREACH;
    }
    void *page = 0;
    const int page_fd = lpr_netd_create_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memset(page, 0, sizeof(netd_connect_t));
    netd_connect_t *req = (netd_connect_t *)page;
    req->handle = lpr_socket_backend(fd)->handle;
    req->addr.addr_be = addr->addr_be;
    req->addr.port_be = addr->port_be;
    lpr_socket_debug_connect("begin", addr->addr_be, addr->port_be, 0);
    const int64_t status = lpr_netd_call(NETD_OP_CONNECT, page_fd, 0, 0);
    lpr_socket_debug_connect("end", addr->addr_be, addr->port_be, status);
    lpr_netd_destroy_page(page_fd, page);
    if (status == 0) {
        lpr_socket_backend(fd)->connected = 1;
        lpr_socket_backend(fd)->connecting = 0;
        lpr_socket_backend(fd)->last_error = 0;
        lpr_socket_backend(fd)->local_addr_be = LPR_NETD_DEFAULT_ADDR_BE;
        lpr_socket_backend(fd)->peer_addr_be = addr->addr_be;
        lpr_socket_backend(fd)->peer_port_be = addr->port_be;
    } else if (status == -LPR_LINUX_EINPROGRESS || status == -LPR_LINUX_EALREADY) {
        lpr_socket_backend(fd)->connected = 0;
        lpr_socket_backend(fd)->connecting = 1;
        lpr_socket_backend(fd)->last_error = 0;
        lpr_socket_backend(fd)->local_addr_be = LPR_NETD_DEFAULT_ADDR_BE;
        lpr_socket_backend(fd)->peer_addr_be = addr->addr_be;
        lpr_socket_backend(fd)->peer_port_be = addr->port_be;
        if ((lpr_socket_backend(fd)->flags & LPR_LINUX_O_NONBLOCK) == 0) {
            const int64_t wait_status = lpr_linux_socket_wait_events(fd, LPR_LINUX_POLLOUT, lpr_socket_backend(fd)->sndtimeo_ms);
            if (wait_status != 0) {
                return wait_status;
            }
            if (lpr_socket_backend(fd)->last_error != 0) {
                const int32_t error = lpr_socket_backend(fd)->last_error;
                lpr_socket_backend(fd)->last_error = 0;
                return -error;
            }
            lpr_socket_backend(fd)->connected = 1;
            lpr_socket_backend(fd)->connecting = 0;
            return 0;
        }
    } else {
        lpr_socket_backend(fd)->connecting = 0;
        lpr_socket_backend(fd)->last_error = (int32_t)-status;
    }
    return status;
}

int64_t lpr_linux_bind(uint64_t fd, uint64_t addr_raw, uint64_t addrlen)
{
    if (lpr_unix_socket_active(fd)) return lpr_unix_socket_address(fd, UNIX_OP_BIND, addr_raw, addrlen);
    if (!lpr_linux_socket_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (addr_raw == 0 || addrlen < sizeof(uint16_t)) {
        return -LPR_LINUX_EINVAL;
    }

    if (lpr_socket_backend(fd)->domain == LPR_LINUX_AF_NETLINK) {
        if (addrlen < sizeof(lpr_linux_sockaddr_nl_t)) return -LPR_LINUX_EINVAL;
        const lpr_linux_sockaddr_nl_t *addr = (const lpr_linux_sockaddr_nl_t *)(uintptr_t)addr_raw;
        if (addr->family != LPR_LINUX_AF_NETLINK) return -LPR_LINUX_EAFNOSUPPORT;
        const uint32_t pid = addr->pid != 0 ? addr->pid : (uint32_t)lpr_linux_getpid();
        const uint32_t groups = addr->groups;
        void *page = 0;
        const int page_fd = lpr_netd_create_page(&page);
        if (page_fd < 0) return page_fd;
        lpr_memset(page, 0, sizeof(netd_netlink_bind_t));
        netd_netlink_bind_t *request = page;
        request->handle = lpr_socket_backend(fd)->handle;
        request->pid = pid;
        request->groups = groups;
        const int64_t status = lpr_netd_call(NETD_OP_BIND, page_fd, 0, 0);
        lpr_netd_destroy_page(page_fd, page);
        if (status == 0) {
            lpr_socket_backend(fd)->local_addr_be = pid;
            lpr_socket_backend(fd)->peer_addr_be = groups;
        }
        return status;
    }
    if (addrlen < sizeof(lpr_linux_sockaddr_in_t)) return -LPR_LINUX_EINVAL;
    const lpr_linux_sockaddr_in_t *addr = (const lpr_linux_sockaddr_in_t *)(uintptr_t)addr_raw;
    if (addr->family != LPR_LINUX_AF_INET) {
        return -LPR_LINUX_EAFNOSUPPORT;
    }
    lpr_socket_backend(fd)->local_addr_be = addr->addr_be;
    lpr_socket_backend(fd)->local_port_be = addr->port_be != 0 ? addr->port_be : lpr_socket_next_port_be();
    return 0;
}

int64_t lpr_linux_listen(uint64_t fd, uint64_t backlog)
{
    return lpr_unix_socket_listen(fd, backlog);
}

int64_t lpr_linux_accept(uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t flags)
{
    return lpr_unix_socket_accept(fd, addr, addrlen, flags);
}

int64_t lpr_linux_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t dest_addr, uint64_t addrlen)
{
    if (lpr_unix_socket_active(fd)) return
        lpr_unix_socket_address_io(fd, buf, len, 1, flags, dest_addr, addrlen);
    if (!lpr_linux_socket_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (buf == 0 && len != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (len == 0) {
        return 0;
    }
    if (dest_addr != 0 && addrlen < sizeof(lpr_linux_sockaddr_in_t)) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t sent_total = 0;
    const uint8_t *src = (const uint8_t *)(uintptr_t)buf;
    while (sent_total < len) {
        uint64_t chunk = len - sent_total;
        if (chunk > NETD_IO_BYTES) {
            chunk = NETD_IO_BYTES;
        }
        void *page = 0;
        const int page_fd = lpr_netd_create_page(&page);
        if (page_fd < 0) {
            return sent_total != 0 ? (int64_t)sent_total : page_fd;
        }
        lpr_memset(page, 0, offsetof(netd_io_t, data));
        netd_io_t *req = (netd_io_t *)page;
        req->handle = lpr_socket_backend(fd)->handle;
        req->length = chunk;
        req->flags = flags;
        if (dest_addr != 0) {
            const lpr_linux_sockaddr_in_t *addr = (const lpr_linux_sockaddr_in_t *)(uintptr_t)dest_addr;
            if (addr->family != LPR_LINUX_AF_INET) {
                lpr_netd_destroy_page(page_fd, page);
                return sent_total != 0 ? (int64_t)sent_total : -LPR_LINUX_EAFNOSUPPORT;
            }
            req->addr.addr_be = addr->addr_be;
            req->addr.port_be = addr->port_be;
            lpr_socket_backend(fd)->peer_addr_be = addr->addr_be;
            lpr_socket_backend(fd)->peer_port_be = addr->port_be;
        }
        lpr_memcpy(req->data, src + sent_total, (size_t)chunk);
        uint64_t sent = 0;
        const int64_t status = lpr_netd_call(NETD_OP_SEND, page_fd, 0, &sent);
        lpr_netd_destroy_page(page_fd, page);
        if (status != 0) {
            if (status == -LPR_LINUX_EAGAIN && !lpr_socket_op_nonblocking(fd, flags)) {
                const int64_t wait_status = lpr_linux_socket_wait_events(fd, LPR_LINUX_POLLOUT, lpr_socket_backend(fd)->sndtimeo_ms);
                if (wait_status == 0) {
                    continue;
                }
                return sent_total != 0 ? (int64_t)sent_total : wait_status;
            }
            return sent_total != 0 ? (int64_t)sent_total : status;
        }
        if (sent == 0) {
            return sent_total != 0 ? (int64_t)sent_total : -LPR_LINUX_EAGAIN;
        }
        sent_total += sent;
    }
    return (int64_t)sent_total;
}

int64_t lpr_linux_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t src_addr, uint64_t addrlen_raw)
{
    if (lpr_unix_socket_active(fd)) return
        lpr_unix_socket_address_io(fd, buf, len, 0, flags, src_addr, addrlen_raw);
    if (!lpr_linux_socket_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (buf == 0 && len != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (len == 0) {
        return 0;
    }
    lpr_wait_deadline_t receive_deadline;
    int receive_deadline_initialized = 0;
recvfrom_retry:
    ;
    void *page = 0;
    const int page_fd = lpr_netd_create_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memset(page, 0, offsetof(netd_io_t, data));
    netd_io_t *req = (netd_io_t *)page;
    req->handle = lpr_socket_backend(fd)->handle;
    req->length = len < NETD_IO_BYTES ? len : NETD_IO_BYTES;
    req->flags = flags;
    uint64_t received = 0;
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    if (__atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u) {
        lpr_glycin_diag_event(
            "recvfrom.enter", fd, req->handle, req->length, (int64_t)flags);
    }
#endif
    const int64_t status = lpr_netd_call(NETD_OP_RECV, page_fd, 0, &received);
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    if (__atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u) {
        lpr_glycin_diag_event(
            "recvfrom.exit", fd, req->handle, received, status);
    }
#endif
    if (status == 0 &&
        (((flags & LPR_LINUX_MSG_PEEK) != 0) || received == 0))
        lpr_linux_socket_mark_readable(fd);
    if (status == 0 && received != 0) {
        lpr_memcpy((void *)(uintptr_t)buf, req->data, (size_t)received);
        if (src_addr != 0 && addrlen_raw != 0) {
            uint32_t *addrlen = (uint32_t *)(uintptr_t)addrlen_raw;
            if (lpr_socket_backend(fd)->domain == LPR_LINUX_AF_NETLINK &&
                *addrlen >= sizeof(lpr_linux_sockaddr_nl_t))
            {
                lpr_linux_sockaddr_nl_t *addr =
                    (lpr_linux_sockaddr_nl_t *)(uintptr_t)src_addr;
                lpr_memset(addr, 0, sizeof(*addr));
                addr->family = LPR_LINUX_AF_NETLINK;
                addr->pid = 0;
                addr->groups = lpr_socket_backend(fd)->peer_addr_be;
                *addrlen = sizeof(*addr);
            } else if (*addrlen >= sizeof(lpr_linux_sockaddr_in_t)) {
                lpr_linux_sockaddr_in_t *addr = (lpr_linux_sockaddr_in_t *)(uintptr_t)src_addr;
                lpr_memset(addr, 0, sizeof(*addr));
                addr->family = LPR_LINUX_AF_INET;
                addr->port_be = req->addr.port_be != 0 ? req->addr.port_be : lpr_socket_backend(fd)->peer_port_be;
                addr->addr_be = req->addr.addr_be != 0 ? req->addr.addr_be : lpr_socket_backend(fd)->peer_addr_be;
                *addrlen = sizeof(*addr);
            }
        }
    }
    lpr_netd_destroy_page(page_fd, page);
    if (status == -LPR_LINUX_EAGAIN && !lpr_socket_op_nonblocking(fd, flags)) {
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
        if (__atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u) {
            lpr_glycin_diag_event(
                "recvfrom.wait.enter",
                fd,
                lpr_socket_backend(fd)->handle,
                lpr_socket_backend(fd)->rcvtimeo_ms,
                status);
        }
#endif
        int64_t wait_status = 0;
        if (!receive_deadline_initialized) {
            wait_status = lpr_wait_deadline_init(
                &receive_deadline,
                lpr_socket_backend(fd)->rcvtimeo_ms == 0 ? -1 :
                    lpr_socket_backend(fd)->rcvtimeo_ms);
            if (wait_status == 0) receive_deadline_initialized = 1;
        }
        if (wait_status == 0) {
            wait_status = lpr_linux_socket_wait_events_until(
                fd, LPR_LINUX_POLLIN, &receive_deadline);
        }
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
        if (__atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u) {
            lpr_glycin_diag_event(
                "recvfrom.wait.exit",
                fd,
                lpr_socket_backend(fd)->handle,
                lpr_socket_backend(fd)->rcvtimeo_ms,
                wait_status);
        }
#endif
        if (wait_status == 0) {
            goto recvfrom_retry;
        }
        return wait_status;
    }
    return status == 0 ? (int64_t)received : status;
}

int64_t lpr_linux_socket_read(uint64_t fd, uint64_t buf, uint64_t count)
{
    return lpr_linux_recvfrom(fd, buf, count, 0, 0, 0);
}

int64_t lpr_linux_socket_write(uint64_t fd, uint64_t buf, uint64_t count)
{
    return lpr_linux_sendto(fd, buf, count, 0, 0, 0);
}

int64_t lpr_linux_socket_readv(uint64_t fd, uint64_t iov_raw, uint64_t iov_count)
{
    if (iov_raw == 0 && iov_count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
    int64_t total = 0;
    for (uint64_t i = 0; i < iov_count; i += 1) {
        const int64_t n = lpr_linux_socket_read(fd, iov[i].base, iov[i].len);
        if (n < 0) {
            return total != 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < iov[i].len) {
            break;
        }
    }
    return total;
}

int64_t lpr_linux_socket_writev(uint64_t fd, uint64_t iov_raw, uint64_t iov_count)
{
    if (iov_raw == 0 && iov_count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
    int64_t total = 0;
    for (uint64_t i = 0; i < iov_count; i += 1) {
        const int64_t n = lpr_linux_socket_write(fd, iov[i].base, iov[i].len);
        if (n < 0) {
            return total != 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < iov[i].len) {
            break;
        }
    }
    return total;
}

int64_t lpr_linux_sendmsg(uint64_t fd, uint64_t msg_raw, uint64_t flags)
{
    if (lpr_unix_socket_active(fd)) return lpr_unix_socket_message(fd, msg_raw, flags, 1);
    if (msg_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const lpr_linux_msghdr_t *msg =
        (const lpr_linux_msghdr_t *)(uintptr_t)msg_raw;
    const lpr_linux_iovec_t *iov =
        (const lpr_linux_iovec_t *)(uintptr_t)msg->msg_iov;

    lpr_fd_pin_t socket_pin;
    if (fd > UINT32_MAX ||
        lpr_fd_table_pin(
            &lpr_control_fd_table,
            (uint32_t)fd,
            &socket_pin) != 0)
    {
        return -LPR_LINUX_EBADF;
    }
    lpr_socket_backend_t *socket =
        socket_pin.ops_id == LPR_FD_OPS_SOCKET ?
        (lpr_socket_backend_t *)socket_pin.state : 0;
    if (socket == 0 || !socket->active) {
        lpr_fd_unpin(&socket_pin);
        return -LPR_LINUX_EBADF;
    }
    lpr_fd_unpin(&socket_pin);

    int64_t total = 0;
    for (uint64_t i = 0; i < msg->msg_iovlen; i += 1) {
        const int64_t n = lpr_linux_sendto(fd, iov[i].base, iov[i].len, flags, msg->msg_name, msg->msg_namelen);
        if (n < 0) {
            return total != 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < iov[i].len) {
            break;
        }
    }
    return total;
}

int64_t lpr_linux_recvmsg(uint64_t fd, uint64_t msg_raw, uint64_t flags)
{
    if (lpr_unix_socket_active(fd)) return lpr_unix_socket_message(fd, msg_raw, flags, 0);
    if (msg_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (!lpr_linux_socket_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    lpr_linux_msghdr_t *msg = (lpr_linux_msghdr_t *)(uintptr_t)msg_raw;
    if (msg->msg_iov == 0 && msg->msg_iovlen != 0) {
        return -LPR_LINUX_EFAULT;
    }
    const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)msg->msg_iov;
    int64_t total = 0;
    for (uint64_t i = 0; i < msg->msg_iovlen; i += 1) {
        uint32_t namelen = msg->msg_namelen;
        const int64_t n = lpr_linux_recvfrom(
            fd,
            iov[i].base,
            iov[i].len,
            flags,
            msg->msg_name,
            msg->msg_name != 0 ? (uint64_t)(uintptr_t)&namelen : 0);
        if (n < 0) {
            return total != 0 ? total : n;
        }
        if (msg->msg_name != 0) {
            msg->msg_namelen = namelen;
        }
        total += n;
        if (lpr_socket_backend(fd)->type == LPR_LINUX_SOCK_DGRAM) {
            break;
        }
        if ((uint64_t)n < iov[i].len) {
            break;
        }
    }
    msg->msg_flags = 0;
    return total;
}

int64_t lpr_linux_sendmmsg(uint64_t fd, uint64_t msgvec_raw, uint64_t vlen, uint64_t flags)
{
    if (msgvec_raw == 0 && vlen != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (vlen > LPR_LINUX_UIO_MAXIOV) {
        return -LPR_LINUX_EINVAL;
    }
    lpr_linux_mmsghdr_t *msgvec = (lpr_linux_mmsghdr_t *)(uintptr_t)msgvec_raw;
    uint64_t sent = 0;
    for (uint64_t i = 0; i < vlen; i += 1) {
        const int64_t n = lpr_linux_sendmsg(fd, (uint64_t)(uintptr_t)&msgvec[i].msg_hdr, flags);
        if (n < 0) {
            return sent != 0 ? (int64_t)sent : n;
        }
        msgvec[i].msg_len = (uint32_t)n;
        sent += 1;
    }
    return (int64_t)sent;
}

int64_t lpr_linux_recvmmsg(uint64_t fd, uint64_t msgvec_raw, uint64_t vlen, uint64_t flags, uint64_t timeout)
{
    (void)timeout;
    if (msgvec_raw == 0 && vlen != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (vlen > LPR_LINUX_UIO_MAXIOV) {
        return -LPR_LINUX_EINVAL;
    }
    lpr_linux_mmsghdr_t *msgvec = (lpr_linux_mmsghdr_t *)(uintptr_t)msgvec_raw;
    uint64_t received = 0;
    for (uint64_t i = 0; i < vlen; i += 1) {
        const int64_t n = lpr_linux_recvmsg(fd, (uint64_t)(uintptr_t)&msgvec[i].msg_hdr, flags);
        if (n < 0) {
            return received != 0 ? (int64_t)received : n;
        }
        msgvec[i].msg_len = (uint32_t)n;
        received += 1;
        if (n == 0) {
            break;
        }
    }
    return (int64_t)received;
}

int64_t lpr_linux_getsockname(uint64_t fd, uint64_t addr, uint64_t addrlen)
{
    if (lpr_unix_socket_active(fd)) return lpr_unix_socket_name(fd, addr, addrlen, 0);
    if (!lpr_linux_socket_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (lpr_socket_backend(fd)->domain == LPR_LINUX_AF_NETLINK) {
        if (addr == 0 || addrlen == 0) return -LPR_LINUX_EFAULT;
        uint32_t *length = (uint32_t *)(uintptr_t)addrlen;
        if (*length < sizeof(lpr_linux_sockaddr_nl_t)) {
            *length = sizeof(lpr_linux_sockaddr_nl_t);
            return -LPR_LINUX_EINVAL;
        }
        lpr_linux_sockaddr_nl_t *name = (lpr_linux_sockaddr_nl_t *)(uintptr_t)addr;
        lpr_memset(name, 0, sizeof(*name));
        name->family = LPR_LINUX_AF_NETLINK;
        name->pid = lpr_socket_backend(fd)->local_addr_be;
        name->groups = lpr_socket_backend(fd)->peer_addr_be;
        *length = sizeof(*name);
        return 0;
    }
    return lpr_socket_copy_sockaddr(
        addr,
        addrlen,
        lpr_socket_backend(fd)->local_addr_be,
        lpr_socket_backend(fd)->local_port_be);
}

int64_t lpr_linux_getpeername(uint64_t fd, uint64_t addr, uint64_t addrlen)
{
    if (lpr_unix_socket_active(fd)) return lpr_unix_socket_name(fd, addr, addrlen, 1);
    if (!lpr_linux_socket_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (!lpr_socket_backend(fd)->connected && lpr_socket_backend(fd)->peer_addr_be == 0 && lpr_socket_backend(fd)->peer_port_be == 0) {
        return -LPR_LINUX_ENOTCONN;
    }
    return lpr_socket_copy_sockaddr(
        addr,
        addrlen,
        lpr_socket_backend(fd)->peer_addr_be,
        lpr_socket_backend(fd)->peer_port_be);
}

int64_t lpr_linux_shutdown(uint64_t fd, uint64_t how)
{
    if (lpr_unix_socket_active(fd)) return lpr_unix_socket_shutdown(fd, how);
    (void)how;
    if (!lpr_linux_socket_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    return 0;
}

int64_t lpr_linux_setsockopt(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen)
{
    if (lpr_unix_socket_active(fd)) return lpr_unix_socket_option(fd, level, optname, optval, optlen, 1);
    if (!lpr_linux_socket_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (optval == 0 && optlen != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (level == LPR_LINUX_SOL_SOCKET) {
        if (optname == LPR_LINUX_SO_SNDTIMEO || optname == LPR_LINUX_SO_RCVTIMEO) {
            if (optlen < sizeof(lpr_linux_timeval_t)) {
                return -LPR_LINUX_EINVAL;
            }
            int64_t timeout_ms = 0;
            const int64_t status = lpr_socket_timeval_to_ms((const lpr_linux_timeval_t *)(uintptr_t)optval, &timeout_ms);
            if (status != 0) {
                return status;
            }
            if (timeout_ms > 2147483647ll) {
                timeout_ms = 2147483647ll;
            }
            if (optname == LPR_LINUX_SO_SNDTIMEO) {
                lpr_socket_backend(fd)->sndtimeo_ms = (int32_t)timeout_ms;
            } else {
                lpr_socket_backend(fd)->rcvtimeo_ms = (int32_t)timeout_ms;
            }
            return 0;
        }
        if (optlen < sizeof(int32_t)) {
            return -LPR_LINUX_EINVAL;
        }
        const int32_t value = *(const int32_t *)(uintptr_t)optval;
        switch (optname) {
        case LPR_LINUX_SO_REUSEADDR:
            lpr_socket_backend(fd)->reuseaddr = value != 0;
            return 0;
        case LPR_LINUX_SO_KEEPALIVE:
            lpr_socket_backend(fd)->keepalive = value != 0;
            return 0;
        case LPR_LINUX_SO_SNDBUF:
            lpr_socket_backend(fd)->sndbuf = value > 0 ? (uint32_t)value : lpr_socket_backend(fd)->sndbuf;
            return 0;
        case LPR_LINUX_SO_RCVBUF:
            lpr_socket_backend(fd)->rcvbuf = value > 0 ? (uint32_t)value : lpr_socket_backend(fd)->rcvbuf;
            return 0;
        default:
            return 0;
        }
    }
    if (level == LPR_LINUX_IPPROTO_TCP && optname == LPR_LINUX_TCP_NODELAY) {
        if (optlen < sizeof(int32_t)) {
            return -LPR_LINUX_EINVAL;
        }
        lpr_socket_backend(fd)->tcp_nodelay = *(const int32_t *)(uintptr_t)optval != 0;
        return 0;
    }
    if ((level == LPR_LINUX_IPPROTO_IP && (optname == LPR_LINUX_IP_TOS || optname == LPR_LINUX_IP_TTL)) ||
        (level == LPR_LINUX_IPPROTO_IPV6 && optname == LPR_LINUX_IPV6_V6ONLY)) {
        return 0;
    }
    return 0;
}

int64_t lpr_linux_getsockopt(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen_raw)
{
    if (lpr_unix_socket_active(fd)) return lpr_unix_socket_option(fd, level, optname, optval, optlen_raw, 0);
    if (!lpr_linux_socket_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (optval == 0 || optlen_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    uint32_t *optlen = (uint32_t *)(uintptr_t)optlen_raw;
    if (level == LPR_LINUX_SOL_SOCKET) {
        if (optname == LPR_LINUX_SO_PEERPIDFD) {
            /* LPR has no pidfd object.  A successful zero-valued result would
             * falsely identify stdin as the peer pidfd; glib/zbus treat
             * ENOPROTOOPT as the normal older-kernel fallback. */
            return -LPR_LINUX_ENOPROTOOPT;
        }
        if (optname == LPR_LINUX_SO_PEERCRED) {
            if (*optlen < 12u) return -LPR_LINUX_EINVAL;
            int32_t *cred = (int32_t *)(uintptr_t)optval;
            cred[0] = lpr_socket_backend(fd)->peer_pid;
            cred[1] = (int32_t)lpr_socket_backend(fd)->peer_uid;
            cred[2] = (int32_t)lpr_socket_backend(fd)->peer_gid;
            *optlen = 12u;
            return 0;
        }
        if (optname == LPR_LINUX_SO_SNDTIMEO || optname == LPR_LINUX_SO_RCVTIMEO) {
            if (*optlen < sizeof(lpr_linux_timeval_t)) {
                return -LPR_LINUX_EINVAL;
            }
            const int32_t timeout_ms =
                optname == LPR_LINUX_SO_SNDTIMEO ? lpr_socket_backend(fd)->sndtimeo_ms : lpr_socket_backend(fd)->rcvtimeo_ms;
            lpr_linux_timeval_t *tv = (lpr_linux_timeval_t *)(uintptr_t)optval;
            tv->tv_sec = timeout_ms / 1000;
            tv->tv_usec = (timeout_ms % 1000) * 1000;
            *optlen = sizeof(*tv);
            return 0;
        }
        if (*optlen < sizeof(int32_t)) {
            return -LPR_LINUX_EINVAL;
        }
        int32_t value = 0;
        switch (optname) {
        case LPR_LINUX_SO_ERROR:
            value = lpr_socket_backend(fd)->last_error;
            lpr_socket_backend(fd)->last_error = 0;
            break;
        case LPR_LINUX_SO_TYPE:
            value = lpr_socket_backend(fd)->type;
            break;
        case LPR_LINUX_SO_DOMAIN:
            value = (int32_t)lpr_socket_backend(fd)->domain;
            break;
        case LPR_LINUX_SO_PROTOCOL:
            value = lpr_socket_backend(fd)->protocol;
            break;
        case LPR_LINUX_SO_REUSEADDR:
            value = lpr_socket_backend(fd)->reuseaddr;
            break;
        case LPR_LINUX_SO_KEEPALIVE:
            value = lpr_socket_backend(fd)->keepalive;
            break;
        case LPR_LINUX_SO_SNDBUF:
            value = (int32_t)lpr_socket_backend(fd)->sndbuf;
            break;
        case LPR_LINUX_SO_RCVBUF:
            value = (int32_t)lpr_socket_backend(fd)->rcvbuf;
            break;
        default:
            value = 0;
            break;
        }
        *(int32_t *)(uintptr_t)optval = value;
        *optlen = sizeof(int32_t);
        return 0;
    }
    if (level == LPR_LINUX_IPPROTO_TCP && optname == LPR_LINUX_TCP_NODELAY) {
        if (*optlen < sizeof(int32_t)) {
            return -LPR_LINUX_EINVAL;
        }
        *(int32_t *)(uintptr_t)optval = lpr_socket_backend(fd)->tcp_nodelay;
        *optlen = sizeof(int32_t);
        return 0;
    }
    if (*optlen >= sizeof(int32_t)) {
        *(int32_t *)(uintptr_t)optval = 0;
        *optlen = sizeof(int32_t);
        return 0;
    }
    return -LPR_LINUX_EINVAL;
}

int64_t lpr_linux_socket_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg)
{
    if (!lpr_linux_socket_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    switch (cmd) {
    case LPR_LINUX_F_GETFD:
        return lpr_control_get_fd_flags(fd);
    case LPR_LINUX_F_SETFD:
        return lpr_control_set_fd_flags(fd, arg);
    case LPR_LINUX_F_GETFL:
        return lpr_control_get_status_flags(fd, lpr_socket_backend(fd)->flags & LPR_LINUX_O_RDWR);
    case LPR_LINUX_F_SETFL:
        lpr_socket_backend(fd)->flags = (uint32_t)((lpr_socket_backend(fd)->flags & ~LPR_LINUX_O_NONBLOCK) | (arg & LPR_LINUX_O_NONBLOCK));
        return lpr_control_set_status_flags(fd, arg);
    case LPR_LINUX_F_DUPFD:
    case LPR_LINUX_F_DUPFD_CLOEXEC:
        return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
    default:
        return -LPR_LINUX_EINVAL;
    }
}

int64_t lpr_linux_socket_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
    if (!lpr_linux_socket_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (request == LPR_LINUX_FIONBIO) {
        if (arg == 0) {
            return -LPR_LINUX_EFAULT;
        }
        const int enabled = *(const int *)(uintptr_t)arg != 0;
        if (enabled) {
            lpr_socket_backend(fd)->flags |= LPR_LINUX_O_NONBLOCK;
        } else {
            lpr_socket_backend(fd)->flags &= ~LPR_LINUX_O_NONBLOCK;
        }
        return lpr_control_set_status_flags(
            fd, lpr_socket_backend(fd)->flags);
    }
    if (request == LPR_LINUX_FIONREAD) {
        if (arg == 0) {
            return -LPR_LINUX_EFAULT;
        }
        void *page = 0;
        const int page_fd = lpr_netd_create_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        lpr_memset(page, 0, offsetof(netd_io_t, data));
        netd_io_t *req = (netd_io_t *)page;
        req->handle = lpr_socket_backend(fd)->handle;
        req->length = NETD_IO_BYTES;
        req->flags = LPR_LINUX_MSG_PEEK | LPR_LINUX_MSG_DONTWAIT;
        uint64_t received = 0;
        const int64_t status = lpr_netd_call(NETD_OP_RECV, page_fd, 0, &received);
        lpr_netd_destroy_page(page_fd, page);
        if (status != 0 && status != -LPR_LINUX_EAGAIN) {
            return status;
        }
        *(int *)(uintptr_t)arg = status == 0 ? (int)received : 0;
        return 0;
    }
    return -LPR_LINUX_ENOTTY;
}

int64_t lpr_linux_socket_fstat(uint64_t fd, uint64_t statbuf)
{
    if (!lpr_linux_socket_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (statbuf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_linux_stat_t *st = (lpr_linux_stat_t *)(uintptr_t)statbuf;
    lpr_memset(st, 0, sizeof(*st));
    st->st_ino = fd + 1u;
    st->st_nlink = 1;
    st->st_mode = LPR_LINUX_S_IFSOCK | 0777u;
    st->st_blksize = 4096;
    return 0;
}

static int64_t lpr_linux_socket_poll_one(uint64_t fd, uint32_t events, uint32_t *out_revents)
{
    if (out_revents == 0) {
        return -LPR_LINUX_EINVAL;
    }
    *out_revents = 0;
    if (!lpr_linux_socket_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    void *page = 0;
    const int page_fd = lpr_netd_create_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memset(page, 0, sizeof(netd_poll_t));
    netd_poll_t *req = (netd_poll_t *)page;
    req->handle = lpr_socket_backend(fd)->handle;
    req->events = events &
        (NETD_POLLIN | NETD_POLLOUT | NETD_POLLERR | NETD_POLLHUP);
    uint64_t revents = 0;
    const int64_t status = lpr_netd_call(NETD_OP_POLL, page_fd, 0, &revents);
    if (status == 0) {
        const int32_t socket_error = req->error;
        *out_revents = req->revents != 0 ? req->revents : (uint32_t)revents;
        if (socket_error != 0) {
            lpr_socket_backend(fd)->last_error = socket_error;
            lpr_socket_backend(fd)->connecting = 0;
            lpr_socket_backend(fd)->connected = 0;
        } else if (lpr_socket_backend(fd)->connecting && (*out_revents & NETD_POLLOUT) != 0) {
            lpr_socket_backend(fd)->last_error = 0;
            lpr_socket_backend(fd)->connecting = 0;
            lpr_socket_backend(fd)->connected = 1;
        }
    }
    lpr_netd_destroy_page(page_fd, page);
    return status;
}


static int64_t lpr_linux_socket_wait_events_until(
    uint64_t fd,
    uint32_t events,
    lpr_wait_deadline_t *deadline)
{
    if (deadline == 0) return -LPR_LINUX_EINVAL;
    int64_t status = 0;
    for (;;) {
        uint32_t revents = 0;
        status = lpr_linux_socket_poll_one(
            fd, events | LPR_LINUX_POLLERR, &revents);
        if (status != 0) {
            return status;
        }
        if ((revents & LPR_LINUX_POLLERR) != 0) {
            return lpr_socket_backend(fd)->last_error != 0 ? -lpr_socket_backend(fd)->last_error : -LPR_LINUX_EIO;
        }
        if ((revents & LPR_LINUX_POLLHUP) != 0) {
            return (events & LPR_LINUX_POLLIN) != 0 ? 0 : -LPR_LINUX_EPIPE;
        }
        if ((revents & events) != 0) {
            return 0;
        }
        int expired = 0;
        status = lpr_wait_deadline_expired(deadline, &expired);
        if (status != 0) return status;
        if (expired) return -LPR_LINUX_EAGAIN;
        lpr_wait_graph_t graph;
        lpr_wait_graph_init(&graph);
        status = lpr_wait_graph_add_fd(&graph, fd, events);
        if (status != 0) return status;
        status = lpr_wait_graph_block(&graph, deadline);
        if (status != 0) return status;
    }
}


static int64_t lpr_linux_socket_wait_events(
    uint64_t fd,
    uint32_t events,
    int32_t timeout_ms)
{
    lpr_wait_deadline_t deadline;
    const int64_t status = lpr_wait_deadline_init(
        &deadline, timeout_ms == 0 ? -1 : timeout_ms);
    if (status != 0) return status;
    return lpr_linux_socket_wait_events_until(fd, events, &deadline);
}

static int64_t lpr_linux_poll_scan(
    lpr_linux_pollfd_t *fds,
    uint64_t nfds,
    int cached_sockets)
{
    (void)cached_sockets;
    int64_t ready = 0;
    for (uint64_t i = 0; i < nfds; i += 1) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) {
            continue;
        }
        const uint64_t fd = (uint64_t)(uint32_t)fds[i].fd;
        if (lpr_linux_inotify_active(fd)) {
            fds[i].revents = (int16_t)lpr_linux_inotify_poll_events(
                fd, (uint32_t)fds[i].events);
            if (fds[i].revents != 0) {
                ready++;
            }
            continue;
        }
        if (lpr_linux_timerfd_active(fd)) {
            fds[i].revents = (int16_t)lpr_linux_timerfd_poll_events(
                fd, (uint32_t)fds[i].events);
            if (fds[i].revents != 0) {
                ready++;
            }
            continue;
        }
        if (lpr_linux_signalfd_active(fd)) {
            fds[i].revents = (int16_t)lpr_linux_signalfd_poll_events(
                fd, (uint32_t)fds[i].events);
            if (fds[i].revents != 0) {
                ready++;
            }
            continue;
        }
        if (lpr_linux_eventfd_active(fd)) {
            fds[i].revents = (int16_t)lpr_linux_eventfd_poll_events(
                fd, (uint32_t)fds[i].events);
            if (fds[i].revents != 0) {
                ready++;
            }
            continue;
        }
        if (lpr_linux_sync_file_fd_active(fd)) {
            fds[i].revents = (int16_t)lpr_sync_file_poll_events(
                fd, (uint32_t)(uint16_t)fds[i].events);
            if (fds[i].revents != 0) ready++;
            continue;
        }
        if (lpr_linux_tty_fd_active(fd)) {
            fds[i].revents = (int16_t)lpr_linux_tty_poll_events(fd, (uint32_t)fds[i].events);
            if (fds[i].revents != 0) {
                ready++;
            }
            continue;
        }
        if (lpr_linux_drm_fd_active(fd)) {
            const int64_t drm_events = lpr_drm_poll_events(fd, (uint32_t)fds[i].events);
            if (drm_events < 0) {
                fds[i].revents = drm_events == -LPR_LINUX_EBADF ?
                    LPR_LINUX_POLLNVAL : LPR_LINUX_POLLERR;
            } else {
                fds[i].revents = (int16_t)drm_events;
            }
            if (fds[i].revents != 0) ready++;
            continue;
        }
        if (lpr_linux_input_fd_active(fd)) {
            const int64_t input_events =
                lpr_input_poll_events(fd, (uint32_t)fds[i].events);
            if (input_events < 0) {
                fds[i].revents = input_events == -LPR_LINUX_EBADF ?
                    LPR_LINUX_POLLNVAL : LPR_LINUX_POLLERR;
            } else {
                fds[i].revents = (int16_t)input_events;
            }
            if (fds[i].revents != 0) ready++;
            continue;
        }
        if (lpr_linux_pipe_fd_active(fd)) {
            fds[i].revents = (int16_t)lpr_linux_pipe_poll_events(fd, (uint32_t)fds[i].events);
            if (fds[i].revents != 0) {
                ready++;
            }
            continue;
        }
        if (lpr_linux_filed_fd_active(fd)) {
            fds[i].revents = (int16_t)((uint32_t)fds[i].events &
                (LPR_LINUX_POLLIN | LPR_LINUX_POLLOUT));
            if (fds[i].revents != 0) {
                ready++;
            }
            continue;
        }
        if (lpr_linux_device_fd_active(fd)) {
            fds[i].revents = (int16_t)((uint32_t)fds[i].events &
                (LPR_LINUX_POLLIN | LPR_LINUX_POLLOUT));
            if (fds[i].revents != 0) ready++;
            continue;
        }
        if (lpr_linux_epoll_fd_active(fd)) {
            const int64_t epoll_events = lpr_epoll_poll_events(
                fd, (uint32_t)(uint16_t)fds[i].events);
            if (epoll_events < 0) {
                fds[i].revents = epoll_events == -LPR_LINUX_EBADF ?
                    LPR_LINUX_POLLNVAL : LPR_LINUX_POLLERR;
            } else {
                fds[i].revents = (int16_t)epoll_events;
            }
            if (fds[i].revents != 0) ready++;
            continue;
        }
        if (lpr_unix_socket_active(fd)) {
            const int64_t result = lpr_unix_socket_poll(fd, (uint16_t)fds[i].events);
            fds[i].revents = result < 0 ? (result == -LPR_LINUX_EBADF ?
                LPR_LINUX_POLLNVAL : LPR_LINUX_POLLERR) : (int16_t)result;
            if (fds[i].revents) ready++;
            continue;
        }
        if (!lpr_linux_socket_fd_active(fd)) {
            fds[i].revents = LPR_LINUX_POLLNVAL;
            ready++;
            continue;
        }
        uint32_t revents = 0;
        const int64_t status = lpr_linux_socket_poll_one(
            fd, (uint32_t)fds[i].events, &revents);
        if (status == -LPR_LINUX_EBADF) {
            fds[i].revents = LPR_LINUX_POLLNVAL;
            ready++;
            continue;
        }
        if (status != 0) {
            fds[i].revents = LPR_LINUX_POLLERR;
            ready++;
            continue;
        }
        fds[i].revents |= (int16_t)(revents &
            (LPR_LINUX_POLLIN | LPR_LINUX_POLLOUT |
             LPR_LINUX_POLLERR | LPR_LINUX_POLLHUP));
        if ((fds[i].events & LPR_LINUX_POLLERR) != 0 && lpr_socket_backend(fd)->last_error != 0) {
            fds[i].revents |= LPR_LINUX_POLLERR;
        }
        if (fds[i].revents != 0) {
            ready++;
        }
    }
    return ready;
}

static int64_t lpr_linux_poll_wait(lpr_linux_pollfd_t *fds, uint64_t nfds, int64_t timeout_ms)
{
#if defined(LPR_GUI_PROFILE) && LPR_GUI_PROFILE
    const int gui = lpr_gui_profile_current_thread();
    uint64_t gui_start;
#endif
    lpr_wait_deadline_t deadline;
    int64_t status = lpr_wait_deadline_init(&deadline, timeout_ms);
    if (status != 0) return status;
    for (;;) {
#if defined(LPR_GUI_PROFILE) && LPR_GUI_PROFILE
        gui_start = gui ? pacha_trace_read_tsc() : 0;
#endif
        const int64_t ready = lpr_linux_poll_scan(fds, nfds, 1);
#if defined(LPR_GUI_PROFILE) && LPR_GUI_PROFILE
        if (gui) lpr_gui_profile_span(LPR_GUI_POLL_SCAN, gui_start, pacha_trace_read_tsc());
#endif
        if (ready != 0 || timeout_ms == 0) return ready;
        int expired = 0;
        status = lpr_wait_deadline_expired(&deadline, &expired);
        if (status != 0) return status;
        if (expired) return 0;
#if defined(LPR_GUI_PROFILE) && LPR_GUI_PROFILE
        gui_start = gui ? pacha_trace_read_tsc() : 0;
#endif
        lpr_wait_graph_t graph;
        lpr_wait_graph_init(&graph);
        for (uint64_t i = 0; i < nfds; ++i) {
            if (fds[i].fd < 0) continue;
            status = lpr_wait_graph_add_fd(
                &graph,
                (uint64_t)(uint32_t)fds[i].fd,
                (uint32_t)(uint16_t)fds[i].events);
            if (status != 0) return status;
        }
#if defined(LPR_GUI_PROFILE) && LPR_GUI_PROFILE
        if (gui) lpr_gui_profile_span(LPR_GUI_POLL_GRAPH, gui_start, pacha_trace_read_tsc());
        gui_start = gui ? pacha_trace_read_tsc() : 0;
#endif
        status = lpr_wait_graph_block(&graph, &deadline);
#if defined(LPR_GUI_PROFILE) && LPR_GUI_PROFILE
        if (gui) lpr_gui_profile_span(LPR_GUI_POLL_BLOCK, gui_start, pacha_trace_read_tsc());
#endif
        if (status == LPR_WAIT_RESTART_SYSCALL) {
            /* The graph has released its watches and pins. Deliver signals
             * while ppoll's temporary mask is still active; delivery may
             * abandon this stack. A spurious/early timeout wake must retain
             * this deadline, not restart the syscall's full relative wait. */
            status = lpr_linux_dispatch_pending_signals_with_result(-LPR_LINUX_EINTR);
            if (status != 0) return status;
            lpr_linux_deliver_native_pending_frame(-LPR_LINUX_EINTR);
        }
        if (status != 0) return status;
    }
}

int64_t lpr_linux_poll(uint64_t fds_raw, uint64_t nfds, uint64_t timeout_ms)
{
    if (fds_raw == 0 && nfds != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (nfds > UINT64_MAX / sizeof(lpr_linux_pollfd_t) ||
        !lpr_user_range_plausible(fds_raw, nfds * sizeof(lpr_linux_pollfd_t)))
    {
        return -LPR_LINUX_EFAULT;
    }
    return lpr_linux_poll_wait(
        (lpr_linux_pollfd_t *)(uintptr_t)fds_raw,
        nfds,
        (int64_t)timeout_ms);
}

int64_t lpr_linux_poll_cached(uint64_t fds_raw, uint64_t nfds)
{
    if (fds_raw == 0 && nfds != 0) return -LPR_LINUX_EFAULT;
    if (nfds > UINT64_MAX / sizeof(lpr_linux_pollfd_t) ||
        !lpr_user_range_plausible(
            fds_raw, nfds * sizeof(lpr_linux_pollfd_t)))
        return -LPR_LINUX_EFAULT;
    return lpr_linux_poll_scan(
        (lpr_linux_pollfd_t *)(uintptr_t)fds_raw, nfds, 1);
}

int64_t lpr_linux_poll_current(uint64_t fds_raw, uint64_t nfds)
{
    if (fds_raw == 0 && nfds != 0) return -LPR_LINUX_EFAULT;
    if (nfds > UINT64_MAX / sizeof(lpr_linux_pollfd_t) ||
        !lpr_user_range_plausible(
            fds_raw, nfds * sizeof(lpr_linux_pollfd_t)))
        return -LPR_LINUX_EFAULT;
    return lpr_linux_poll_scan(
        (lpr_linux_pollfd_t *)(uintptr_t)fds_raw, nfds, 0);
}

static int64_t lpr_linux_wait_mask_begin(
    uint64_t sigmask,
    uint64_t sigsetsize,
    uint64_t *out_old_mask)
{
    if (sigmask == 0) return 0;
    if (sigsetsize != sizeof(uint64_t)) return -LPR_LINUX_EINVAL;
    if (!lpr_user_range_plausible(sigmask, sizeof(uint64_t)))
        return -LPR_LINUX_EFAULT;
    lpr_linux_wait_restore_mask = lpr_linux_signal_mask;
    lpr_linux_wait_restore_mask_active = 1;
    const int64_t status = lpr_linux_rt_sigprocmask(
        LPR_LINUX_SIG_SETMASK,
        sigmask,
        (uint64_t)(uintptr_t)out_old_mask,
        sizeof(uint64_t));
    if (status != 0) lpr_linux_wait_restore_mask_active = 0;
    return status;
}

static void lpr_linux_wait_mask_end(
    uint64_t sigmask,
    uint64_t old_mask,
    int64_t result)
{
    if (sigmask == 0) return;
    /* ppoll/pselect6 must expose the temporary mask through the complete
     * atomic wait.  A native signal can become pending at the same time as an
     * fd becomes ready, so attempt delivery before restoring the caller mask. */
    lpr_linux_deliver_native_pending_frame(
        result == LPR_WAIT_RESTART_SYSCALL ?
            -LPR_LINUX_EINTR : result);
    (void)lpr_linux_rt_sigprocmask(
        LPR_LINUX_SIG_SETMASK,
        (uint64_t)(uintptr_t)&old_mask,
        0,
        sizeof(old_mask));
    lpr_linux_wait_restore_mask_active = 0;
}

int64_t lpr_linux_ppoll(uint64_t fds, uint64_t nfds, uint64_t timeout_ts, uint64_t sigmask, uint64_t sigsetsize)
{
    if (fds == 0 && nfds != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (nfds > UINT64_MAX / sizeof(lpr_linux_pollfd_t) ||
        !lpr_user_range_plausible(fds, nfds * sizeof(lpr_linux_pollfd_t)))
    {
        return -LPR_LINUX_EFAULT;
    }
    int64_t timeout_ms = -1;
    if (timeout_ts != 0) {
        if (!lpr_user_range_plausible(timeout_ts, sizeof(lpr_linux_timespec_t))) {
            return -LPR_LINUX_EFAULT;
        }
        const lpr_linux_timespec_t *ts = (const lpr_linux_timespec_t *)(uintptr_t)timeout_ts;
        if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000ll) {
            return -LPR_LINUX_EINVAL;
        }
        if (ts->tv_sec > 9223372036854775ll) {
            timeout_ms = 9223372036854775807ll;
        } else {
            timeout_ms = ts->tv_sec * 1000ll + (ts->tv_nsec + 999999ll) / 1000000ll;
        }
    }
    uint64_t old_mask = 0;
    const int64_t mask_status = lpr_linux_wait_mask_begin(
        sigmask, sigsetsize, &old_mask);
    if (mask_status != 0) return mask_status;
    const int64_t result = lpr_linux_poll_wait(
        (lpr_linux_pollfd_t *)(uintptr_t)fds, nfds, timeout_ms);
    lpr_linux_wait_mask_end(sigmask, old_mask, result);
    return result;
}

static int64_t lpr_linux_select_scan(
    uint64_t nfds,
    const uint64_t *read_in,
    const uint64_t *write_in,
    const uint64_t *except_in,
    uint64_t *read_out,
    uint64_t *write_out,
    uint64_t *except_out)
{
    int64_t ready = 0;
    for (uint64_t fd = 0; fd < nfds; fd += 1) {
        const int want_read = read_in != 0 && lpr_fdset_test(read_in, fd);
        const int want_write = write_in != 0 && lpr_fdset_test(write_in, fd);
        const int want_except = except_in != 0 && lpr_fdset_test(except_in, fd);
        if (!want_read && !want_write && !want_except) {
            continue;
        }

        int is_read = 0;
        int is_write = 0;
        int is_except = 0;
        if (lpr_unix_socket_active(fd)) {
            const int64_t revents = lpr_unix_socket_poll(fd,
                (want_read ? LPR_LINUX_POLLIN : 0) | (want_write ? LPR_LINUX_POLLOUT : 0));
            if (revents < 0) return revents;
            is_read = (revents & (LPR_LINUX_POLLIN | LPR_LINUX_POLLHUP | LPR_LINUX_POLLERR)) != 0;
            is_write = (revents & (LPR_LINUX_POLLOUT | LPR_LINUX_POLLERR)) != 0;
        } else if (lpr_linux_socket_fd_active(fd)) {
            uint32_t events = 0;
            if (want_read) {
                events |= LPR_LINUX_POLLIN;
            }
            if (want_write) {
                events |= LPR_LINUX_POLLOUT;
            }
            if (want_except) {
                events |= LPR_LINUX_POLLERR;
            }
            uint32_t revents = 0;
            const int64_t status = lpr_linux_socket_poll_one(fd, events, &revents);
            if (status != 0) {
                return status;
            }
            is_read = (revents &
                (LPR_LINUX_POLLIN | LPR_LINUX_POLLHUP)) != 0;
            is_write = (revents & LPR_LINUX_POLLOUT) != 0;
            is_except = (revents & LPR_LINUX_POLLERR) != 0 || lpr_socket_backend(fd)->last_error != 0;
        } else if (lpr_linux_inotify_active(fd)) {
            const uint32_t revents = lpr_linux_inotify_poll_events(
                fd, want_read ? LPR_LINUX_POLLIN : 0);
            is_read = (revents & LPR_LINUX_POLLIN) != 0;
        } else if (lpr_linux_timerfd_active(fd)) {
            uint32_t events = 0;
            if (want_read) {
                events |= LPR_LINUX_POLLIN;
            }
            const uint32_t revents = lpr_linux_timerfd_poll_events(fd, events);
            is_read = (revents & LPR_LINUX_POLLIN) != 0;
        } else if (lpr_linux_signalfd_active(fd)) {
            uint32_t events = 0;
            if (want_read) {
                events |= LPR_LINUX_POLLIN;
            }
            const uint32_t revents = lpr_linux_signalfd_poll_events(fd, events);
            is_read = (revents & LPR_LINUX_POLLIN) != 0;
        } else if (lpr_linux_eventfd_active(fd)) {
            uint32_t events = 0;
            if (want_read) {
                events |= LPR_LINUX_POLLIN;
            }
            if (want_write) {
                events |= LPR_LINUX_POLLOUT;
            }
            const uint32_t revents = lpr_linux_eventfd_poll_events(fd, events);
            is_read = (revents & LPR_LINUX_POLLIN) != 0;
            is_write = (revents & LPR_LINUX_POLLOUT) != 0;
        } else if (lpr_linux_sync_file_fd_active(fd)) {
            const uint32_t revents = lpr_sync_file_poll_events(
                fd, want_read ? LPR_LINUX_POLLIN : 0);
            is_read = (revents & LPR_LINUX_POLLIN) != 0;
        } else if (lpr_linux_tty_fd_active(fd)) {
            uint32_t events = 0;
            if (want_read) {
                events |= LPR_LINUX_POLLIN;
            }
            if (want_write) {
                events |= LPR_LINUX_POLLOUT;
            }
            if (want_except) {
                events |= LPR_LINUX_POLLERR;
            }
            const uint32_t revents = lpr_linux_tty_poll_events(fd, events);
            is_read = (revents & LPR_LINUX_POLLIN) != 0;
            is_write = (revents & LPR_LINUX_POLLOUT) != 0;
            is_except = (revents & LPR_LINUX_POLLERR) != 0;
        } else if (lpr_linux_pipe_fd_active(fd)) {
            uint32_t events = 0;
            if (want_read) {
                events |= LPR_LINUX_POLLIN;
            }
            if (want_write) {
                events |= LPR_LINUX_POLLOUT;
            }
            if (want_except) {
                events |= LPR_LINUX_POLLERR;
            }
            const uint32_t revents = lpr_linux_pipe_poll_events(fd, events);
            is_read = (revents & (LPR_LINUX_POLLIN | 0x0010u)) != 0;
            is_write = (revents & LPR_LINUX_POLLOUT) != 0;
            is_except = (revents & LPR_LINUX_POLLERR) != 0;
        } else if (lpr_linux_drm_fd_active(fd)) {
            uint32_t events = 0;
            if (want_read) events |= LPR_LINUX_POLLIN;
            if (want_except) events |= LPR_LINUX_POLLERR;
            const int64_t revents = lpr_drm_poll_events(fd, events);
            if (revents < 0) return revents;
            is_read = (revents & LPR_LINUX_POLLIN) != 0;
            is_except = (revents & LPR_LINUX_POLLERR) != 0;
        } else if (lpr_linux_input_fd_active(fd)) {
            uint32_t events = 0;
            if (want_read) events |= LPR_LINUX_POLLIN;
            if (want_except) events |= LPR_LINUX_POLLERR;
            const int64_t revents = lpr_input_poll_events(fd, events);
            if (revents < 0) return revents;
            is_read = (revents & LPR_LINUX_POLLIN) != 0;
            is_except = (revents & LPR_LINUX_POLLERR) != 0;
        } else {
            if (lpr_linux_filed_fd_active(fd) || lpr_linux_device_fd_active(fd)) {
                is_read = want_read;
                is_write = want_write;
            } else {
                return -LPR_LINUX_EBADF;
            }
        }

        if (want_read && is_read) {
            ready += 1;
        } else if (read_out != 0) {
            lpr_fdset_clear(read_out, fd);
        }
        if (want_write && is_write) {
            ready += 1;
        } else if (write_out != 0) {
            lpr_fdset_clear(write_out, fd);
        }
        if (want_except && is_except) {
            ready += 1;
        } else if (except_out != 0) {
            lpr_fdset_clear(except_out, fd);
        }
    }
    return ready;
}

static int64_t lpr_linux_select_wait(
    uint64_t nfds,
    uint64_t readfds,
    uint64_t writefds,
    uint64_t exceptfds,
    int64_t timeout_ms)
{
    enum { LPR_SELECT_MAX_FDS = 1024, LPR_SELECT_MAX_WORDS = LPR_SELECT_MAX_FDS / 64 };
    if (nfds > LPR_SELECT_MAX_FDS) {
        return -LPR_LINUX_EINVAL;
    }
    const uint64_t words = lpr_fdset_word_count(nfds);
    const uint64_t fdset_bytes = words * sizeof(uint64_t);
    if (!lpr_user_range_plausible(readfds, readfds != 0 ? fdset_bytes : 0) ||
        !lpr_user_range_plausible(writefds, writefds != 0 ? fdset_bytes : 0) ||
        !lpr_user_range_plausible(exceptfds, exceptfds != 0 ? fdset_bytes : 0))
    {
        return -LPR_LINUX_EFAULT;
    }
    uint64_t read_orig[LPR_SELECT_MAX_WORDS];
    uint64_t write_orig[LPR_SELECT_MAX_WORDS];
    uint64_t except_orig[LPR_SELECT_MAX_WORDS];
    lpr_memset(read_orig, 0, sizeof(read_orig));
    lpr_memset(write_orig, 0, sizeof(write_orig));
    lpr_memset(except_orig, 0, sizeof(except_orig));
    if (readfds != 0) {
        lpr_memcpy(read_orig, (const void *)(uintptr_t)readfds, (size_t)(words * sizeof(uint64_t)));
    }
    if (writefds != 0) {
        lpr_memcpy(write_orig, (const void *)(uintptr_t)writefds, (size_t)(words * sizeof(uint64_t)));
    }
    if (exceptfds != 0) {
        lpr_memcpy(except_orig, (const void *)(uintptr_t)exceptfds, (size_t)(words * sizeof(uint64_t)));
    }

    lpr_wait_deadline_t deadline;
    int64_t status = lpr_wait_deadline_init(&deadline, timeout_ms);
    if (status != 0) return status;
    for (;;) {
        if (readfds != 0) {
            lpr_memcpy((void *)(uintptr_t)readfds, read_orig, (size_t)(words * sizeof(uint64_t)));
        }
        if (writefds != 0) {
            lpr_memcpy((void *)(uintptr_t)writefds, write_orig, (size_t)(words * sizeof(uint64_t)));
        }
        if (exceptfds != 0) {
            lpr_memcpy((void *)(uintptr_t)exceptfds, except_orig, (size_t)(words * sizeof(uint64_t)));
        }

        const int64_t ready = lpr_linux_select_scan(
            nfds,
            readfds != 0 ? (const uint64_t *)(uintptr_t)readfds : 0,
            writefds != 0 ? (const uint64_t *)(uintptr_t)writefds : 0,
            exceptfds != 0 ? (const uint64_t *)(uintptr_t)exceptfds : 0,
            readfds != 0 ? (uint64_t *)(uintptr_t)readfds : 0,
            writefds != 0 ? (uint64_t *)(uintptr_t)writefds : 0,
            exceptfds != 0 ? (uint64_t *)(uintptr_t)exceptfds : 0);
        if (ready != 0 || timeout_ms == 0) {
            return ready;
        }

        int expired = 0;
        status = lpr_wait_deadline_expired(&deadline, &expired);
        if (status != 0) return status;
        if (expired) return 0;

        lpr_wait_graph_t graph;
        lpr_wait_graph_init(&graph);
        for (uint64_t fd = 0; fd < nfds; ++fd) {
            uint32_t events = 0;
            if (readfds != 0 && lpr_fdset_test(read_orig, fd))
                events |= LPR_LINUX_POLLIN;
            if (writefds != 0 && lpr_fdset_test(write_orig, fd))
                events |= LPR_LINUX_POLLOUT;
            if (exceptfds != 0 && lpr_fdset_test(except_orig, fd))
                events |= LPR_LINUX_POLLERR;
            if (events == 0) continue;
            status = lpr_wait_graph_add_fd(&graph, fd, events);
            if (status != 0) return status;
        }
        status = lpr_wait_graph_block(&graph, &deadline);
        if (status != 0) return status;
    }
}

int64_t lpr_linux_select(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, uint64_t timeout)
{
    int64_t timeout_ms = -1;
    if (timeout != 0) {
        if (!lpr_user_range_plausible(timeout, sizeof(lpr_linux_timeval_t))) {
            return -LPR_LINUX_EFAULT;
        }
        const int64_t status = lpr_socket_timeval_to_ms((const lpr_linux_timeval_t *)(uintptr_t)timeout, &timeout_ms);
        if (status != 0) {
            return status;
        }
    }
    return lpr_linux_select_wait(nfds, readfds, writefds, exceptfds, timeout_ms);
}

int64_t lpr_linux_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, uint64_t timeout, uint64_t sigmask)
{
    int64_t timeout_ms = -1;
    if (timeout != 0) {
        if (!lpr_user_range_plausible(timeout, sizeof(lpr_linux_timespec_t))) {
            return -LPR_LINUX_EFAULT;
        }
        const int64_t status = lpr_socket_timespec_to_ms((const lpr_linux_timespec_t *)(uintptr_t)timeout, &timeout_ms);
        if (status != 0) {
            return status;
        }
    }
    uint64_t sigmask_pointer = 0;
    uint64_t sigsetsize = 0;
    if (sigmask != 0) {
        if (!lpr_user_range_plausible(sigmask, sizeof(lpr_linux_pselect_sigmask_t)))
            return -LPR_LINUX_EFAULT;
        const lpr_linux_pselect_sigmask_t *mask =
            (const lpr_linux_pselect_sigmask_t *)(uintptr_t)sigmask;
        sigmask_pointer = mask->sigmask;
        sigsetsize = mask->sigsetsize;
    }
    uint64_t old_mask = 0;
    const int64_t mask_status = lpr_linux_wait_mask_begin(
        sigmask_pointer, sigsetsize, &old_mask);
    if (mask_status != 0) return mask_status;
    const int64_t result = lpr_linux_select_wait(
        nfds, readfds, writefds, exceptfds, timeout_ms);
    lpr_linux_wait_mask_end(sigmask_pointer, old_mask, result);
    return result;
}

#include "lpr_linux_syscall.h"
#include "lpr_filed_internal.h"
#include "lpr_memory.h"
#include "lpr_socket.h"
#include "lpr_vfs_local.h"
#include "support/string.h"
#include "support/syscall.h"
#include <pacha/ipc.h>
#include <pacha/status.h>
#include <pacha/trace.h>
#include <pachaos/abi.h>
#include <personality/linux_lpr.h>
#include <personality/zpoline.h>
#include <stddef.h>

#define LPR_LINUX_PROT_READ 0x1ull
#define LPR_LINUX_PROT_WRITE 0x2ull
#define LPR_LINUX_PROT_EXEC 0x4ull
#define LPR_LINUX_MAP_SHARED 0x01ull
#define LPR_LINUX_MAP_PRIVATE 0x02ull
#define LPR_LINUX_MAP_FIXED 0x10ull
#define LPR_LINUX_MAP_ANONYMOUS 0x20ull
#define LPR_LINUX_MAP_NORESERVE 0x4000ull
#define LPR_LINUX_MAP_FIXED_NOREPLACE 0x100000ull
#define LPR_LINUX_MS_ASYNC 0x1ull
#define LPR_LINUX_MS_INVALIDATE 0x2ull
#define LPR_LINUX_MS_SYNC 0x4ull
#define LPR_LINUX_MEMBARRIER_CMD_QUERY 0ull
#define LPR_LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED (1ull << 3)
#define LPR_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED (1ull << 4)
#define LPR_LINUX_MEMBARRIER_PRIVATE_EXPEDITED_MASK \
    (LPR_LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED | \
     LPR_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED)
#define LPR_LINUX_FUTEX_WAIT 0ull
#define LPR_LINUX_FUTEX_WAKE 1ull
#define LPR_LINUX_FUTEX_REQUEUE 3ull
#define LPR_LINUX_FUTEX_WAIT_BITSET 9ull
#define LPR_LINUX_FUTEX_PRIVATE_FLAG 128ull
#define LPR_LINUX_FUTEX_CLOCK_REALTIME 256ull
static int64_t lpr_linux_pacha_status_to_errno(int64_t status);
static uint64_t lpr_linux_prot_to_pacha(uint64_t prot);

static uint32_t lpr_linux_private_expedited_registered;

const struct lpr_linux_user_frame *lpr_current_linux_user_frame(void)
{
    enum { LPR_FRAME_ANCHOR_MAX_DEPTH = 64 };
    const uint64_t anchor_magic = 0x4c50524652414d45ull;
    const uint64_t anchor_guard = 0x454d41524652504cull;
    const uintptr_t max_frame_step = 1024u * 1024u;
    uintptr_t cursor;
    __asm__ volatile("mov %%rbp, %0" : "=r"(cursor));
    for (unsigned depth = 0; depth < LPR_FRAME_ANCHOR_MAX_DEPTH; ++depth) {
        if (cursor == 0 || (cursor & (sizeof(uintptr_t) - 1u)) != 0) {
            return 0;
        }
        const uintptr_t next = *(const uintptr_t *)(uintptr_t)cursor;
        if (next <= cursor || next - cursor > max_frame_step) {
            return 0;
        }
        const uint64_t *anchor = (const uint64_t *)(uintptr_t)next;
        if (anchor[0] == anchor_magic && anchor[2] == anchor_guard) {
            return (const struct lpr_linux_user_frame *)(uintptr_t)anchor[1];
        }
        cursor = next;
    }
    return 0;
}

static void lpr_linux_rlimits_init(void)
{
    if (lpr_linux_rlimits_initialized) {
        return;
    }
    lpr_linux_rlimits_initialized = 1;
    for (uint64_t i = 0; i < LPR_LINUX_RLIMIT_COUNT; ++i) {
        lpr_linux_rlimits[i].cur = UINT64_MAX;
        lpr_linux_rlimits[i].max = UINT64_MAX;
    }
    lpr_linux_rlimits[LPR_LINUX_RLIMIT_STACK].cur = 8ull * 1024ull * 1024ull;
    lpr_linux_rlimits[LPR_LINUX_RLIMIT_NOFILE].cur = LPR_LINUX_FD_LIMIT;
    lpr_linux_rlimits[LPR_LINUX_RLIMIT_NOFILE].max = LPR_LINUX_FD_LIMIT;
    lpr_linux_rlimits[LPR_LINUX_RLIMIT_MEMLOCK].cur = 64ull * 1024ull;
    lpr_linux_rlimits[LPR_LINUX_RLIMIT_MSGQUEUE].cur = 819200ull;
    lpr_linux_rlimits[LPR_LINUX_RLIMIT_NICE].cur = 0;
    lpr_linux_rlimits[LPR_LINUX_RLIMIT_RTPRIO].cur = 0;
    lpr_linux_rlimits[LPR_LINUX_RLIMIT_RTTIME].cur = UINT64_MAX;
}

static int64_t lpr_linux_copy_out_rlimit(uint64_t resource, uint64_t out_raw)
{
    if (resource >= LPR_LINUX_RLIMIT_COUNT) {
        return -LPR_LINUX_EINVAL;
    }
    if (out_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_linux_rlimits_init();
    uint64_t *out = (uint64_t *)(uintptr_t)out_raw;
    out[0] = lpr_linux_rlimits[resource].cur;
    out[1] = lpr_linux_rlimits[resource].max;
    return 0;
}

static int64_t lpr_linux_set_rlimit(uint64_t resource, uint64_t in_raw)
{
    if (resource >= LPR_LINUX_RLIMIT_COUNT) {
        return -LPR_LINUX_EINVAL;
    }
    if (in_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const uint64_t *in = (const uint64_t *)(uintptr_t)in_raw;
    const uint64_t cur = in[0];
    const uint64_t max = in[1];
    if (cur > max) {
        return -LPR_LINUX_EINVAL;
    }
    if (resource == LPR_LINUX_RLIMIT_NOFILE && max > LPR_LINUX_FD_LIMIT) {
        return -LPR_LINUX_EPERM;
    }
    lpr_linux_rlimits_init();
    lpr_linux_rlimits[resource].cur = cur;
    lpr_linux_rlimits[resource].max = max;
    return 0;
}

static int64_t lpr_linux_prlimit64(uint64_t pid, uint64_t resource, uint64_t new_limit, uint64_t old_limit)
{
    if (pid != 0 && pid != (uint64_t)lpr_linux_getpid()) {
        return -LPR_LINUX_ESRCH;
    }
    if (old_limit != 0) {
        const int64_t get_status = lpr_linux_copy_out_rlimit(resource, old_limit);
        if (get_status != 0) {
            return get_status;
        }
    } else if (resource >= LPR_LINUX_RLIMIT_COUNT) {
        return -LPR_LINUX_EINVAL;
    }
    if (new_limit != 0) {
        return lpr_linux_set_rlimit(resource, new_limit);
    }
    return 0;
}

static int64_t lpr_linux_getresid(uint64_t real_raw, uint64_t effective_raw, uint64_t saved_raw)
{
    if (real_raw == 0 || effective_raw == 0 || saved_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    *(uint32_t *)(uintptr_t)real_raw = 0;
    *(uint32_t *)(uintptr_t)effective_raw = 0;
    *(uint32_t *)(uintptr_t)saved_raw = 0;
    return 0;
}

static void lpr_trace_socket_syscall_event(const char *phase,
                                           uint64_t nr,
                                           uint64_t a0,
                                           uint64_t a1,
                                           uint64_t a2,
                                           int64_t result)
{
    const uint32_t event = lpr_strcmp(phase, "enter") == 0 ?
        PACHA_TRACE_EVENT_LPR_SYSCALL_ENTER :
        PACHA_TRACE_EVENT_LPR_SYSCALL_EXIT;
    pacha_trace5(PACHA_TRACE_COMPONENT_LPR, event, PACHA_TRACE_CLASS_SYSCALL, nr, a0, a1, a2, (uint64_t)result);
}

static void lpr_trace_mmap_call(
    const char *op,
    uint64_t addr,
    uint64_t len,
    uint64_t prot,
    uint64_t flags,
    uint64_t fd,
    uint64_t offset,
    int64_t result)
{
    (void)addr;
    pacha_trace6(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_MMAP_CALL,
        PACHA_TRACE_CLASS_DEBUG,
        pacha_trace_name_id(op),
        len,
        prot,
        flags,
        fd,
        (uint64_t)result);
    pacha_trace3(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_MMAP_CALL,
        PACHA_TRACE_CLASS_DEBUG,
        pacha_trace_name_id("offset"),
        offset,
        addr);
}

static void lpr_trace_patch_mapping(const struct lpr_patch_mapping_result *result)
{
    if (result == 0) {
        return;
    }
    pacha_trace4(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_PATCH_MAPPING,
        PACHA_TRACE_CLASS_METRIC,
        result->scanned_bytes,
        result->patched_sites,
        result->skipped_sites,
        result->cycles);
}

typedef struct lpr_trace_syscall_metric {
    uint64_t nr;
    uint64_t count;
    uint64_t total_cycles;
    uint64_t max_cycles;
    uint64_t errors;
} lpr_trace_syscall_metric_t;

#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
#ifndef LPR_SYSCALL_PROFILE_SNAPSHOT_CYCLES
#define LPR_SYSCALL_PROFILE_SNAPSHOT_CYCLES 18000000000ull
#endif

static uint32_t lpr_syscall_profile_enabled;
static uint32_t lpr_syscall_profile_snapshot_dumped;
static uint32_t lpr_syscall_profile_slow_dumped;
static uint64_t lpr_syscall_profile_started_at;
static uint64_t lpr_syscall_profile_owner_pid;
#endif

static lpr_trace_syscall_metric_t lpr_trace_syscall_metrics[] = {
    { .nr = LPR_LINUX_SYS_READ },
    { .nr = LPR_LINUX_SYS_WRITE },
    { .nr = LPR_LINUX_SYS_OPEN },
    { .nr = LPR_LINUX_SYS_CLOSE },
    { .nr = LPR_LINUX_SYS_STAT },
    { .nr = LPR_LINUX_SYS_FSTAT },
    { .nr = LPR_LINUX_SYS_LSTAT },
    { .nr = LPR_LINUX_SYS_POLL },
    { .nr = LPR_LINUX_SYS_LSEEK },
    { .nr = LPR_LINUX_SYS_MMAP },
    { .nr = LPR_LINUX_SYS_MPROTECT },
    { .nr = LPR_LINUX_SYS_MUNMAP },
    { .nr = LPR_LINUX_SYS_MREMAP },
    { .nr = LPR_LINUX_SYS_MSYNC },
    { .nr = LPR_LINUX_SYS_BRK },
    { .nr = LPR_LINUX_SYS_IOCTL },
    { .nr = LPR_LINUX_SYS_PREAD64 },
    { .nr = LPR_LINUX_SYS_READV },
    { .nr = LPR_LINUX_SYS_WRITEV },
    { .nr = LPR_LINUX_SYS_ACCESS },
    { .nr = LPR_LINUX_SYS_PIPE },
    { .nr = LPR_LINUX_SYS_DUP },
    { .nr = LPR_LINUX_SYS_DUP2 },
    { .nr = LPR_LINUX_SYS_SOCKET },
    { .nr = LPR_LINUX_SYS_CONNECT },
    { .nr = LPR_LINUX_SYS_ACCEPT },
    { .nr = LPR_LINUX_SYS_SENDTO },
    { .nr = LPR_LINUX_SYS_RECVFROM },
    { .nr = LPR_LINUX_SYS_SENDMSG },
    { .nr = LPR_LINUX_SYS_RECVMSG },
    { .nr = LPR_LINUX_SYS_SHUTDOWN },
    { .nr = LPR_LINUX_SYS_BIND },
    { .nr = LPR_LINUX_SYS_LISTEN },
    { .nr = LPR_LINUX_SYS_GETSOCKNAME },
    { .nr = LPR_LINUX_SYS_GETPEERNAME },
    { .nr = LPR_LINUX_SYS_SOCKETPAIR },
    { .nr = LPR_LINUX_SYS_SETSOCKOPT },
    { .nr = LPR_LINUX_SYS_GETSOCKOPT },
    { .nr = LPR_LINUX_SYS_CLONE },
    { .nr = LPR_LINUX_SYS_FORK },
    { .nr = LPR_LINUX_SYS_VFORK },
    { .nr = LPR_LINUX_SYS_EXECVE },
    { .nr = LPR_LINUX_SYS_WAIT4 },
    { .nr = LPR_LINUX_SYS_NANOSLEEP },
    { .nr = LPR_LINUX_SYS_GETITIMER },
    { .nr = LPR_LINUX_SYS_SETITIMER },
    { .nr = LPR_LINUX_SYS_GETPID },
    { .nr = LPR_LINUX_SYS_EXIT },
    { .nr = LPR_LINUX_SYS_FCNTL },
    { .nr = LPR_LINUX_SYS_FLOCK },
    { .nr = LPR_LINUX_SYS_SELECT },
    { .nr = LPR_LINUX_SYS_PSELECT6 },
    { .nr = LPR_LINUX_SYS_FSYNC },
    { .nr = LPR_LINUX_SYS_FDATASYNC },
    { .nr = LPR_LINUX_SYS_FTRUNCATE },
    { .nr = LPR_LINUX_SYS_SYNC },
    { .nr = LPR_LINUX_SYS_GETCWD },
    { .nr = LPR_LINUX_SYS_CHDIR },
    { .nr = LPR_LINUX_SYS_FCHDIR },
    { .nr = LPR_LINUX_SYS_RENAME },
    { .nr = LPR_LINUX_SYS_MKDIR },
    { .nr = LPR_LINUX_SYS_RMDIR },
    { .nr = LPR_LINUX_SYS_LINK },
    { .nr = LPR_LINUX_SYS_UNLINK },
    { .nr = LPR_LINUX_SYS_SYMLINK },
    { .nr = LPR_LINUX_SYS_READLINK },
    { .nr = LPR_LINUX_SYS_CHMOD },
    { .nr = LPR_LINUX_SYS_FCHMOD },
    { .nr = LPR_LINUX_SYS_ARCH_PRCTL },
    { .nr = LPR_LINUX_SYS_GETTID },
    { .nr = LPR_LINUX_SYS_GETDENTS64 },
    { .nr = LPR_LINUX_SYS_SET_TID_ADDRESS },
    { .nr = LPR_LINUX_SYS_RT_SIGTIMEDWAIT },
    { .nr = LPR_LINUX_SYS_RT_SIGSUSPEND },
    { .nr = LPR_LINUX_SYS_FADVISE64 },
    { .nr = LPR_LINUX_SYS_CLOCK_GETTIME },
    { .nr = LPR_LINUX_SYS_CLOCK_GETRES },
    { .nr = LPR_LINUX_SYS_CLOCK_NANOSLEEP },
    { .nr = LPR_LINUX_SYS_FUTEX },
    { .nr = LPR_LINUX_SYS_EPOLL_WAIT },
    { .nr = LPR_LINUX_SYS_EPOLL_CTL },
    { .nr = LPR_LINUX_SYS_PPOLL },
    { .nr = LPR_LINUX_SYS_EPOLL_PWAIT },
    { .nr = LPR_LINUX_SYS_TIMERFD_CREATE },
    { .nr = LPR_LINUX_SYS_TIMERFD_SETTIME },
    { .nr = LPR_LINUX_SYS_TIMERFD_GETTIME },
    { .nr = LPR_LINUX_SYS_EVENTFD },
    { .nr = LPR_LINUX_SYS_EVENTFD2 },
    { .nr = LPR_LINUX_SYS_INOTIFY_INIT },
    { .nr = LPR_LINUX_SYS_INOTIFY_ADD_WATCH },
    { .nr = LPR_LINUX_SYS_INOTIFY_RM_WATCH },
    { .nr = LPR_LINUX_SYS_INOTIFY_INIT1 },
    { .nr = LPR_LINUX_SYS_EPOLL_CREATE1 },
    { .nr = LPR_LINUX_SYS_DUP3 },
    { .nr = LPR_LINUX_SYS_PIPE2 },
    { .nr = LPR_LINUX_SYS_EXIT_GROUP },
    { .nr = LPR_LINUX_SYS_OPENAT },
    { .nr = LPR_LINUX_SYS_MKDIRAT },
    { .nr = LPR_LINUX_SYS_NEWFSTATAT },
    { .nr = LPR_LINUX_SYS_UNLINKAT },
    { .nr = LPR_LINUX_SYS_RENAMEAT },
    { .nr = LPR_LINUX_SYS_LINKAT },
    { .nr = LPR_LINUX_SYS_FCHMODAT },
    { .nr = LPR_LINUX_SYS_UTIMENSAT },
    { .nr = LPR_LINUX_SYS_RECVMMSG },
    { .nr = LPR_LINUX_SYS_SYNCFS },
    { .nr = LPR_LINUX_SYS_SENDMMSG },
    { .nr = LPR_LINUX_SYS_GETRANDOM },
};

#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
static void lpr_syscall_profile_enable(void)
{
    const uint64_t pid =
        (uint64_t)lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID);
    if (lpr_syscall_profile_enabled != 0 &&
        lpr_syscall_profile_owner_pid == pid)
    {
        return;
    }
    for (uint64_t i = 0;
         i < sizeof(lpr_trace_syscall_metrics) /
             sizeof(lpr_trace_syscall_metrics[0]);
         ++i)
    {
        lpr_trace_syscall_metrics[i].count = 0;
        lpr_trace_syscall_metrics[i].total_cycles = 0;
        lpr_trace_syscall_metrics[i].max_cycles = 0;
        lpr_trace_syscall_metrics[i].errors = 0;
    }
    lpr_syscall_profile_enabled = 1;
    lpr_syscall_profile_snapshot_dumped = 0;
    lpr_syscall_profile_slow_dumped = 0;
    lpr_syscall_profile_owner_pid = pid;
    lpr_syscall_profile_started_at = pacha_trace_read_tsc();
    pacha_trace_set_masks(
        PACHA_TRACE_COMPONENT_BIT(PACHA_TRACE_COMPONENT_LPR),
        PACHA_TRACE_CLASS_ERROR | PACHA_TRACE_CLASS_METRIC |
            PACHA_TRACE_CLASS_SYSCALL);
}
#endif

static lpr_trace_syscall_metric_t *lpr_trace_syscall_metric_slot(uint64_t nr)
{
    for (uint64_t i = 0; i < sizeof(lpr_trace_syscall_metrics) / sizeof(lpr_trace_syscall_metrics[0]); ++i) {
        if (lpr_trace_syscall_metrics[i].nr == nr) {
            return &lpr_trace_syscall_metrics[i];
        }
    }
    return 0;
}

static void lpr_trace_syscall_record(uint64_t nr, uint64_t cycles, int64_t result)
{
    if (!pacha_trace_enabled(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_CLASS_METRIC)) {
        return;
    }
    lpr_trace_syscall_metric_t *metric = lpr_trace_syscall_metric_slot(nr);
    if (metric == 0) {
        return;
    }
    metric->count++;
    metric->total_cycles += cycles;
    if (cycles > metric->max_cycles) {
        metric->max_cycles = cycles;
    }
    if (result < 0) {
        metric->errors++;
    }
}

static void lpr_trace_syscall_dump(uint64_t exit_nr)
{
    const uint64_t pid = (uint64_t)lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID);
    const uint64_t tid = (uint64_t)lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID);
    uint64_t total_count = 0;
    uint64_t total_cycles = 0;
    for (uint64_t i = 0; i < sizeof(lpr_trace_syscall_metrics) / sizeof(lpr_trace_syscall_metrics[0]); ++i) {
        const lpr_trace_syscall_metric_t *metric = &lpr_trace_syscall_metrics[i];
        if (metric->count == 0) {
            continue;
        }
        total_count += metric->count;
        total_cycles += metric->total_cycles;
        pacha_trace6(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_LPR_SYSCALL_METRIC,
            PACHA_TRACE_CLASS_METRIC,
            pid,
            tid,
            metric->nr,
            metric->count,
            metric->total_cycles / metric->count,
            metric->max_cycles);
        pacha_trace2(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_LPR_SYSCALL_METRIC,
            PACHA_TRACE_CLASS_METRIC,
            metric->nr,
            metric->errors);
    }
    pacha_trace5(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_SYSCALL_SUMMARY,
        PACHA_TRACE_CLASS_METRIC,
        pid,
        tid,
        exit_nr,
        total_count,
        total_cycles);
}

#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
#if defined(LPR_STARTUP_PROFILE) && LPR_STARTUP_PROFILE
static void lpr_startup_profile_dump(void);
#endif

static void lpr_syscall_profile_maybe_dump(uint64_t nr)
{
    if (lpr_syscall_profile_snapshot_dumped != 0 ||
        lpr_syscall_profile_started_at == 0)
    {
        return;
    }
    const uint64_t now = pacha_trace_read_tsc();
    if (now < lpr_syscall_profile_started_at ||
        now - lpr_syscall_profile_started_at <
            LPR_SYSCALL_PROFILE_SNAPSHOT_CYCLES)
    {
        return;
    }
    lpr_syscall_profile_snapshot_dumped = 1;
    lpr_trace_syscall_dump(nr);
#if defined(LPR_STARTUP_PROFILE) && LPR_STARTUP_PROFILE
    lpr_startup_profile_dump();
#else
    pacha_trace_dump_ring();
#endif
}
#endif

static void lpr_trace_slow_syscall(
    uint64_t nr,
    uint64_t a0,
    uint64_t a1,
    uint64_t a2,
    uint64_t a3,
    uint64_t a4,
    uint64_t a5,
    int64_t result,
    uint64_t cycles)
{
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    pacha_trace4(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_SLOW_SYSCALL,
        PACHA_TRACE_CLASS_SYSCALL,
        nr,
        a0,
        (uint64_t)result,
        cycles);
#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
    if (cycles >= 1000000000ull &&
        __atomic_exchange_n(
            &lpr_syscall_profile_slow_dumped, 1u, __ATOMIC_ACQ_REL) == 0u)
    {
        pacha_trace_dump_ring();
    }
#endif
}

static char *lpr_mmap_diag_append_text(
    char *out, const char *end, const char *text)
{
    if (out == 0 || end == 0 || text == 0) {
        return out;
    }
    while (out < end && *text != '\0') {
        *out++ = *text++;
    }
    return out;
}

static char *lpr_mmap_diag_append_u64(
    char *out, const char *end, uint64_t value)
{
    char digits[20];
    uint64_t count = 0;
    do {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0 && count < sizeof(digits));
    while (out < end && count != 0) {
        *out++ = digits[--count];
    }
    return out;
}

static char *lpr_mmap_diag_append_i64(
    char *out, const char *end, int64_t value)
{
    uint64_t magnitude = (uint64_t)value;
    if (value < 0) {
        if (out < end) {
            *out++ = '-';
        }
        magnitude = (~magnitude) + 1u;
    }
    return lpr_mmap_diag_append_u64(out, end, magnitude);
}

#if defined(LPR_MMAP_IMAGE_DIAG) && LPR_MMAP_IMAGE_DIAG
static void lpr_mmap_image_diag(
    const lpr_filed_backend_t *file,
    uint64_t len,
    uint64_t map_len,
    uint64_t prot,
    uint64_t offset,
    int64_t mapped)
{
    if (file == 0 || mapped < 4096 ||
        (prot & LPR_LINUX_PROT_EXEC) == 0 ||
        map_len < 512u * 1024u)
    {
        return;
    }
    char line[512];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_mmap_diag_append_text(
        out, end, "[lpr-mmap-image] native_pid=");
    out = lpr_mmap_diag_append_i64(
        out, end, lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID));
    out = lpr_mmap_diag_append_text(out, end, " native_tid=");
    out = lpr_mmap_diag_append_i64(
        out, end, lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID));
    out = lpr_mmap_diag_append_text(out, end, " linux_pid=");
    out = lpr_mmap_diag_append_i64(out, end, lpr_linux_current_pid);
    out = lpr_mmap_diag_append_text(out, end, " mapped=");
    out = lpr_mmap_diag_append_i64(out, end, mapped);
    out = lpr_mmap_diag_append_text(out, end, " len=");
    out = lpr_mmap_diag_append_u64(out, end, len);
    out = lpr_mmap_diag_append_text(out, end, " map_len=");
    out = lpr_mmap_diag_append_u64(out, end, map_len);
    out = lpr_mmap_diag_append_text(out, end, " offset=");
    out = lpr_mmap_diag_append_u64(out, end, offset);
    out = lpr_mmap_diag_append_text(out, end, " file_size=");
    out = lpr_mmap_diag_append_u64(out, end, file->stat_size);
    out = lpr_mmap_diag_append_text(out, end, " path=");
    out = lpr_mmap_diag_append_text(out, end, file->open_path);
    if (out < end) {
        *out++ = '\n';
    }
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_LOG,
        (uint64_t)(uintptr_t)line,
        (uint64_t)(out - line));
}

static void lpr_mmap_exec_diag(
    const char *operation,
    uint64_t address,
    uint64_t length,
    uint64_t prot)
{
    if (operation == 0 || address < 4096 ||
        (prot & LPR_LINUX_PROT_EXEC) == 0 ||
        length < 4096u)
    {
        return;
    }
    char line[256];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_mmap_diag_append_text(
        out, end, "[lpr-mmap-exec] native_pid=");
    out = lpr_mmap_diag_append_i64(
        out, end, lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID));
    out = lpr_mmap_diag_append_text(out, end, " native_tid=");
    out = lpr_mmap_diag_append_i64(
        out, end, lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID));
    out = lpr_mmap_diag_append_text(out, end, " linux_pid=");
    out = lpr_mmap_diag_append_i64(out, end, lpr_linux_current_pid);
    out = lpr_mmap_diag_append_text(out, end, " op=");
    out = lpr_mmap_diag_append_text(out, end, operation);
    out = lpr_mmap_diag_append_text(out, end, " address=");
    out = lpr_mmap_diag_append_u64(out, end, address);
    out = lpr_mmap_diag_append_text(out, end, " length=");
    out = lpr_mmap_diag_append_u64(out, end, length);
    out = lpr_mmap_diag_append_text(out, end, " prot=");
    out = lpr_mmap_diag_append_u64(out, end, prot);
    if (out < end) {
        *out++ = '\n';
    }
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_LOG,
        (uint64_t)(uintptr_t)line,
        (uint64_t)(out - line));
}
#else
static inline void lpr_mmap_image_diag(
    const lpr_filed_backend_t *file,
    uint64_t len,
    uint64_t map_len,
    uint64_t prot,
    uint64_t offset,
    int64_t mapped)
{
    (void)file;
    (void)len;
    (void)map_len;
    (void)prot;
    (void)offset;
    (void)mapped;
}

static inline void lpr_mmap_exec_diag(
    const char *operation,
    uint64_t address,
    uint64_t length,
    uint64_t prot)
{
    (void)operation;
    (void)address;
    (void)length;
    (void)prot;
}
#endif

#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
uint32_t lpr_glycin_diag_armed;
uint32_t lpr_glycin_diag_socket_fd;
uint32_t lpr_glycin_diag_follow_budget;
static uint32_t lpr_glycin_diag_owner_budget;
static uint64_t lpr_glycin_diag_owner_pid;
static uint64_t lpr_glycin_diag_owner_tid;
static uint64_t lpr_glycin_diag_lines;
static uint32_t lpr_glycin_diag_log_lock;

static int lpr_glycin_diag_event_selected(const char *event)
{
    if (event == 0) return 0;
    return lpr_memcmp(event, "wait.block.exit", 15u) == 0 ||
        lpr_memcmp(event, "wait.block.drained", 18u) == 0 ||
        lpr_memcmp(event, "dbus.", 5u) == 0 ||
        lpr_memcmp(event, "socket.watch", 12u) == 0 ||
        lpr_memcmp(event, "recvmsg.", 8u) == 0 ||
        lpr_memcmp(event, "sendmsg.", 8u) == 0 ||
        lpr_memcmp(event, "poll.native", 11u) == 0 ||
        lpr_memcmp(event, "poll.netd", 9u) == 0;
}

void lpr_glycin_diag_event(
    const char *event,
    uint64_t a,
    uint64_t b,
    uint64_t c,
    int64_t result)
{
    if (!lpr_glycin_diag_event_selected(event)) return;
    const uint64_t pid = (uint64_t)lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID);
    const uint64_t owner_pid = __atomic_load_n(
        &lpr_glycin_diag_owner_pid, __ATOMIC_ACQUIRE);
    if (owner_pid != 0u && pid != owner_pid) {
        return;
    }
    while (__atomic_exchange_n(
               &lpr_glycin_diag_log_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
    {
        __asm__ volatile("pause");
    }
    const uint64_t line_index = __atomic_fetch_add(
        &lpr_glycin_diag_lines, 1u, __ATOMIC_RELAXED);
    if (line_index >= 512u) {
        __atomic_store_n(
            &lpr_glycin_diag_log_lock, 0u, __ATOMIC_RELEASE);
        return;
    }
    char line[256];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_mmap_diag_append_text(out, end, "[lpr-glycin] event=");
    out = lpr_mmap_diag_append_text(out, end, event);
    out = lpr_mmap_diag_append_text(out, end, " pid=");
    out = lpr_mmap_diag_append_u64(out, end, pid);
    out = lpr_mmap_diag_append_text(out, end, " tid=");
    out = lpr_mmap_diag_append_i64(
        out, end, lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID));
    out = lpr_mmap_diag_append_text(out, end, " a=");
    out = lpr_mmap_diag_append_u64(out, end, a);
    out = lpr_mmap_diag_append_text(out, end, " b=");
    out = lpr_mmap_diag_append_u64(out, end, b);
    out = lpr_mmap_diag_append_text(out, end, " c=");
    out = lpr_mmap_diag_append_u64(out, end, c);
    out = lpr_mmap_diag_append_text(out, end, " result=");
    out = lpr_mmap_diag_append_i64(out, end, result);
    if (out < end) {
        *out++ = '\n';
    }
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_LOG,
        (uint64_t)(uintptr_t)line,
        (uint64_t)(out - line));
    __atomic_store_n(&lpr_glycin_diag_log_lock, 0u, __ATOMIC_RELEASE);
}

void lpr_glycin_diag_arm(const char *reason)
{
    if (__atomic_exchange_n(
            &lpr_glycin_diag_armed, 1u, __ATOMIC_ACQ_REL) == 0u)
    {
        __atomic_store_n(
            &lpr_glycin_diag_owner_pid,
            (uint64_t)lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID),
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &lpr_glycin_diag_owner_tid,
            (uint64_t)lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID),
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &lpr_glycin_diag_owner_budget, 8u, __ATOMIC_RELEASE);
        lpr_glycin_diag_event(reason, 0, 0, 0, 0);
    }
}

static int lpr_glycin_diag_contains(
    const void *data_raw,
    uint64_t length,
    const char *needle)
{
    if (data_raw == 0 || needle == 0) {
        return 0;
    }
    const uint8_t *data = (const uint8_t *)data_raw;
    const uint64_t needle_length = (uint64_t)lpr_strlen(needle);
    if (needle_length == 0 || length < needle_length) {
        return 0;
    }
    for (uint64_t i = 0; i <= length - needle_length; ++i) {
        if (lpr_memcmp(data + i, needle, (size_t)needle_length) == 0) {
            return 1;
        }
    }
    return 0;
}

static int lpr_glycin_diag_syscall_selected(
    uint64_t nr,
    uint64_t a1,
    uint64_t a4)
{
    switch (nr) {
    case LPR_LINUX_SYS_FUTEX:
    case LPR_LINUX_SYS_MREMAP:
    case LPR_LINUX_SYS_MEMFD_CREATE:
    case LPR_LINUX_SYS_FTRUNCATE:
        return 1;
    case LPR_LINUX_SYS_MMAP:
        return a1 >= 1024u * 1024u || a4 != UINT64_MAX;
    default:
        return 0;
    }
}

static int lpr_dbus_wait_diag_syscall(uint64_t nr)
{
    switch (nr) {
    case LPR_LINUX_SYS_POLL:
    case LPR_LINUX_SYS_NANOSLEEP:
    case LPR_LINUX_SYS_FUTEX:
    case LPR_LINUX_SYS_CLOCK_NANOSLEEP:
    case LPR_LINUX_SYS_EPOLL_WAIT:
    case LPR_LINUX_SYS_PSELECT6:
    case LPR_LINUX_SYS_PPOLL:
    case LPR_LINUX_SYS_EPOLL_PWAIT:
        return 1;
    default:
        return 0;
    }
}

#if defined(LPR_WAIT_ENTRY_DIAG) && LPR_WAIT_ENTRY_DIAG
static int lpr_wait_entry_diag_syscall(uint64_t nr, uint64_t operation)
{
    if (nr != LPR_LINUX_SYS_FUTEX)
        return lpr_dbus_wait_diag_syscall(nr);

    const uint64_t command = operation &
        ~(LPR_LINUX_FUTEX_PRIVATE_FLAG | LPR_LINUX_FUTEX_CLOCK_REALTIME);
    return command == LPR_LINUX_FUTEX_WAIT ||
        command == LPR_LINUX_FUTEX_WAIT_BITSET;
}
#endif

static uint32_t lpr_dbus_wait_diag_lock;
static uint32_t lpr_dbus_wait_diag_lines;

#if defined(LPR_WAIT_ENTRY_DIAG) && LPR_WAIT_ENTRY_DIAG
static uint32_t lpr_wait_entry_diag_lock;
static uint32_t lpr_wait_entry_diag_lines;

static void lpr_wait_entry_diag_log(
    const char *stage,
    uint64_t nr,
    uint64_t a0,
    uint64_t a1,
    uint64_t a2,
    uint64_t a3,
    uint64_t a4,
    int64_t result)
{
#if defined(LPR_WAIT_ENTRY_DIAG_LINUX_PID)
    if (lpr_linux_current_pid != LPR_WAIT_ENTRY_DIAG_LINUX_PID) return;
#endif
    while (__atomic_exchange_n(
               &lpr_wait_entry_diag_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
    {
        __asm__ volatile("pause");
    }
    if (__atomic_fetch_add(
            &lpr_wait_entry_diag_lines, 1u, __ATOMIC_RELAXED) >= 4096u)
    {
        __atomic_store_n(
            &lpr_wait_entry_diag_lock, 0u, __ATOMIC_RELEASE);
        return;
    }
    char line[384];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_mmap_diag_append_text(out, end, "[lpr-wait-call] stage=");
    out = lpr_mmap_diag_append_text(out, end, stage);
    out = lpr_mmap_diag_append_text(out, end, " native_pid=");
    out = lpr_mmap_diag_append_i64(
        out, end, lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID));
    out = lpr_mmap_diag_append_text(out, end, " native_tid=");
    out = lpr_mmap_diag_append_i64(
        out, end, lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID));
    out = lpr_mmap_diag_append_text(out, end, " linux_pid=");
    out = lpr_mmap_diag_append_i64(out, end, lpr_linux_current_pid);
    out = lpr_mmap_diag_append_text(out, end, " nr=");
    out = lpr_mmap_diag_append_u64(out, end, nr);
    out = lpr_mmap_diag_append_text(out, end, " a0=");
    out = lpr_mmap_diag_append_u64(out, end, a0);
    out = lpr_mmap_diag_append_text(out, end, " a1=");
    out = lpr_mmap_diag_append_u64(out, end, a1);
    out = lpr_mmap_diag_append_text(out, end, " a2=");
    out = lpr_mmap_diag_append_u64(out, end, a2);
    out = lpr_mmap_diag_append_text(out, end, " a3=");
    out = lpr_mmap_diag_append_u64(out, end, a3);
    out = lpr_mmap_diag_append_text(out, end, " a4=");
    out = lpr_mmap_diag_append_u64(out, end, a4);
    const uint64_t timeout_raw = nr == LPR_LINUX_SYS_PPOLL ? a2 :
        (nr == LPR_LINUX_SYS_PSELECT6 ? a4 : 0);
    if (timeout_raw >= 4096u &&
        timeout_raw <= 0x0000800000000000ull - 2u * sizeof(int64_t))
    {
        const int64_t *timeout = (const int64_t *)(uintptr_t)timeout_raw;
        out = lpr_mmap_diag_append_text(out, end, " timeout_sec=");
        out = lpr_mmap_diag_append_i64(out, end, timeout[0]);
        out = lpr_mmap_diag_append_text(out, end, " timeout_nsec=");
        out = lpr_mmap_diag_append_i64(out, end, timeout[1]);
    }
    out = lpr_mmap_diag_append_text(out, end, " result=");
    out = lpr_mmap_diag_append_i64(out, end, result);
    if (out < end) *out++ = '\n';
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_LOG,
        (uint64_t)(uintptr_t)line,
        (uint64_t)(out - line));
    __atomic_store_n(
        &lpr_wait_entry_diag_lock, 0u, __ATOMIC_RELEASE);
}

void lpr_wait_native_diag_event(
    const char *stage,
    uint64_t fd,
    uint64_t events,
    uint64_t leaf_count,
    uint64_t timeout_ticks,
    int64_t status)
{
    lpr_wait_entry_diag_log(
        stage,
        PACHA_FD_SYSCALL_WAIT_MANY,
        fd,
        events,
        leaf_count,
        timeout_ticks,
        0,
        status);
}
#endif

typedef struct lpr_dbus_wait_diag_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
} lpr_dbus_wait_diag_pollfd_t;

static void lpr_dbus_wait_diag_log(
    uint64_t nr,
    uint64_t a0,
    uint64_t a1,
    uint64_t a2,
    uint64_t a3,
    int64_t result,
    uint64_t elapsed_ns)
{
    if (elapsed_ns < 100000000u) return;
    while (__atomic_exchange_n(
               &lpr_dbus_wait_diag_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
    {
        __asm__ volatile("pause");
    }
    if (__atomic_fetch_add(
            &lpr_dbus_wait_diag_lines, 1u, __ATOMIC_RELAXED) >= 512u)
    {
        __atomic_store_n(
            &lpr_dbus_wait_diag_lock, 0u, __ATOMIC_RELEASE);
        return;
    }
    char line[640];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_mmap_diag_append_text(out, end, "[lpr-dbus-wait] pid=");
    out = lpr_mmap_diag_append_i64(
        out, end, lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID));
    out = lpr_mmap_diag_append_text(out, end, " tid=");
    out = lpr_mmap_diag_append_i64(
        out, end, lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID));
    out = lpr_mmap_diag_append_text(out, end, " nr=");
    out = lpr_mmap_diag_append_u64(out, end, nr);
    out = lpr_mmap_diag_append_text(out, end, " elapsed_ns=");
    out = lpr_mmap_diag_append_u64(out, end, elapsed_ns);
    out = lpr_mmap_diag_append_text(out, end, " result=");
    out = lpr_mmap_diag_append_i64(out, end, result);
    out = lpr_mmap_diag_append_text(out, end, " a0=");
    out = lpr_mmap_diag_append_u64(out, end, a0);
    out = lpr_mmap_diag_append_text(out, end, " a1=");
    out = lpr_mmap_diag_append_u64(out, end, a1);
    out = lpr_mmap_diag_append_text(out, end, " a2=");
    out = lpr_mmap_diag_append_u64(out, end, a2);
    out = lpr_mmap_diag_append_text(out, end, " a3=");
    out = lpr_mmap_diag_append_u64(out, end, a3);
    if ((nr == LPR_LINUX_SYS_POLL || nr == LPR_LINUX_SYS_PPOLL) &&
        a1 != 0 &&
        a0 >= 4096u &&
        a0 <= 0x0000800000000000ull -
            sizeof(lpr_dbus_wait_diag_pollfd_t))
    {
        const lpr_dbus_wait_diag_pollfd_t *pollfd =
            (const lpr_dbus_wait_diag_pollfd_t *)(uintptr_t)a0;
        const int64_t logical_fd = pollfd->fd;
        uint64_t backend_kind = 0;
        uint64_t event_state = 0;
        int64_t native_wait_fd = -1;
        if (logical_fd >= 0) {
            const uint64_t fd = (uint64_t)(uint32_t)logical_fd;
            if (lpr_linux_eventfd_active(fd)) {
                backend_kind |= 1u;
                const lpr_event_backend_t *event = lpr_event_backend(fd);
                if (event != 0) {
                    event_state =
                        (__atomic_load_n(
                             &event->counter, __ATOMIC_ACQUIRE) << 1u) |
                        __atomic_load_n(
                            &event->notify_pending, __ATOMIC_ACQUIRE);
                    native_wait_fd = event->wait_fd.raw;
                }
            }
            if (lpr_linux_pipe_fd_active(fd)) backend_kind |= 2u;
            if (lpr_linux_socket_fd_active(fd)) backend_kind |= 4u;
            if (lpr_linux_timerfd_active(fd)) backend_kind |= 8u;
            if (lpr_linux_epoll_fd_active(fd)) backend_kind |= 16u;
        }
        out = lpr_mmap_diag_append_text(out, end, " fd0=");
        out = lpr_mmap_diag_append_i64(out, end, logical_fd);
        out = lpr_mmap_diag_append_text(out, end, " events0=");
        out = lpr_mmap_diag_append_i64(out, end, pollfd->events);
        out = lpr_mmap_diag_append_text(out, end, " revents0=");
        out = lpr_mmap_diag_append_i64(out, end, pollfd->revents);
        out = lpr_mmap_diag_append_text(out, end, " kind0=");
        out = lpr_mmap_diag_append_u64(out, end, backend_kind);
        out = lpr_mmap_diag_append_text(out, end, " estate0=");
        out = lpr_mmap_diag_append_u64(out, end, event_state);
        out = lpr_mmap_diag_append_text(out, end, " native0=");
        out = lpr_mmap_diag_append_i64(out, end, native_wait_fd);
        if (a1 > 1u &&
            a0 <= 0x0000800000000000ull -
                2u * sizeof(lpr_dbus_wait_diag_pollfd_t))
        {
            pollfd++;
            const int64_t second_fd = pollfd->fd;
            backend_kind = 0;
            event_state = 0;
            native_wait_fd = -1;
            if (second_fd >= 0) {
                const uint64_t fd = (uint64_t)(uint32_t)second_fd;
                if (lpr_linux_eventfd_active(fd)) {
                    backend_kind |= 1u;
                    const lpr_event_backend_t *event =
                        lpr_event_backend(fd);
                    if (event != 0) {
                        event_state =
                            (__atomic_load_n(
                                 &event->counter,
                                 __ATOMIC_ACQUIRE) << 1u) |
                            __atomic_load_n(
                                &event->notify_pending,
                                __ATOMIC_ACQUIRE);
                        native_wait_fd = event->wait_fd.raw;
                    }
                }
                if (lpr_linux_pipe_fd_active(fd)) backend_kind |= 2u;
                if (lpr_linux_socket_fd_active(fd)) backend_kind |= 4u;
                if (lpr_linux_timerfd_active(fd)) backend_kind |= 8u;
                if (lpr_linux_epoll_fd_active(fd)) backend_kind |= 16u;
            }
            out = lpr_mmap_diag_append_text(out, end, " fd1=");
            out = lpr_mmap_diag_append_i64(out, end, second_fd);
            out = lpr_mmap_diag_append_text(out, end, " events1=");
            out = lpr_mmap_diag_append_i64(out, end, pollfd->events);
            out = lpr_mmap_diag_append_text(out, end, " revents1=");
            out = lpr_mmap_diag_append_i64(out, end, pollfd->revents);
            out = lpr_mmap_diag_append_text(out, end, " kind1=");
            out = lpr_mmap_diag_append_u64(out, end, backend_kind);
            out = lpr_mmap_diag_append_text(out, end, " estate1=");
            out = lpr_mmap_diag_append_u64(out, end, event_state);
            out = lpr_mmap_diag_append_text(out, end, " native1=");
            out = lpr_mmap_diag_append_i64(out, end, native_wait_fd);
        }
    }
    if (out < end) *out++ = '\n';
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_LOG,
        (uint64_t)(uintptr_t)line,
        (uint64_t)(out - line));
    __atomic_store_n(
        &lpr_dbus_wait_diag_lock, 0u, __ATOMIC_RELEASE);
}

static int lpr_glycin_diag_take_follow_slot(void)
{
    uint32_t remaining = __atomic_load_n(
        &lpr_glycin_diag_follow_budget, __ATOMIC_ACQUIRE);
    while (remaining != 0u) {
        if (__atomic_compare_exchange_n(
                &lpr_glycin_diag_follow_budget,
                &remaining,
                remaining - 1u,
                0,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE))
        {
            return 1;
        }
    }
    return 0;
}

static int lpr_glycin_diag_take_owner_slot(void)
{
    if ((uint64_t)lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID) !=
        __atomic_load_n(&lpr_glycin_diag_owner_tid, __ATOMIC_ACQUIRE))
    {
        return 0;
    }
    uint32_t remaining = __atomic_load_n(
        &lpr_glycin_diag_owner_budget, __ATOMIC_ACQUIRE);
    while (remaining != 0u) {
        if (__atomic_compare_exchange_n(
                &lpr_glycin_diag_owner_budget,
                &remaining,
                remaining - 1u,
                0,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE))
        {
            return 1;
        }
    }
    return 0;
}
#endif

static void lpr_trace_mmap_error(
    const char *stage,
    uint64_t addr,
    uint64_t len,
    uint64_t prot,
    uint64_t flags,
    uint64_t fd,
    uint64_t offset,
    int64_t status)
{
    char line[256];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_mmap_diag_append_text(out, end, "[lpr] mmap failure stage=");
    out = lpr_mmap_diag_append_text(out, end, stage);
    out = lpr_mmap_diag_append_text(out, end, " status=");
    out = lpr_mmap_diag_append_i64(out, end, status);
    out = lpr_mmap_diag_append_text(out, end, " len=");
    out = lpr_mmap_diag_append_u64(out, end, len);
    out = lpr_mmap_diag_append_text(out, end, " fd=");
    out = lpr_mmap_diag_append_u64(out, end, fd);
    out = lpr_mmap_diag_append_text(out, end, " offset=");
    out = lpr_mmap_diag_append_u64(out, end, offset);
    /* A rejection is only actionable with the request that was rejected:
     * the kernel refuses some prot/flags combinations outright. */
    out = lpr_mmap_diag_append_text(out, end, " addr=");
    out = lpr_mmap_diag_append_u64(out, end, addr);
    out = lpr_mmap_diag_append_text(out, end, " prot=");
    out = lpr_mmap_diag_append_u64(out, end, prot);
    out = lpr_mmap_diag_append_text(out, end, " flags=");
    out = lpr_mmap_diag_append_u64(out, end, flags);
    if (out < end) {
        *out++ = '\n';
    }
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_LOG,
        (uint64_t)(uintptr_t)line,
        (uint64_t)(out - line));
    pacha_trace6(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_MMAP_ERROR,
        PACHA_TRACE_CLASS_ERROR,
        pacha_trace_name_id(stage),
        len,
        prot,
        flags,
        fd,
        (uint64_t)status);
    pacha_trace2(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_EVENT_LPR_MMAP_ERROR, PACHA_TRACE_CLASS_ERROR, addr, offset);
}

static void lpr_trace_mmap_load(
    uint64_t len,
    uint64_t loaded,
    uint64_t prot,
    uint64_t flags,
    uint64_t fd,
    uint64_t offset)
{
    pacha_trace6(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_EVENT_LPR_MMAP_LOAD, PACHA_TRACE_CLASS_DEBUG, len, loaded, prot, flags, fd, offset);
}

enum {
    LPR_STARTUP_MMAP_STAGE_CACHE_LOOKUP = 0,
    LPR_STARTUP_MMAP_STAGE_FILE_VMO,
    LPR_STARTUP_MMAP_STAGE_VMO_CREATE,
    LPR_STARTUP_MMAP_STAGE_PREAD_TO_VMO,
    LPR_STARTUP_MMAP_STAGE_NATIVE_MMAP,
    LPR_STARTUP_MMAP_STAGE_PATCH,
    LPR_STARTUP_MMAP_STAGE_MPROTECT,
    LPR_STARTUP_MMAP_STAGE_FALLBACK_COPY,
    LPR_STARTUP_MMAP_STAGE_COUNT,
};

enum {
    LPR_STARTUP_MMAP_ROUTE_CACHE_HIT = 0,
    LPR_STARTUP_MMAP_ROUTE_CACHE_MAP_FAILED,
    LPR_STARTUP_MMAP_ROUTE_FILE_VMO_MAPPED,
    LPR_STARTUP_MMAP_ROUTE_FILE_VMO_RPC_FAILED,
    LPR_STARTUP_MMAP_ROUTE_FILE_VMO_MAP_FAILED,
    LPR_STARTUP_MMAP_ROUTE_NO_FILED_BACKEND,
    LPR_STARTUP_MMAP_ROUTE_NO_GENERATION,
    LPR_STARTUP_MMAP_ROUTE_EMPTY_FILE,
    LPR_STARTUP_MMAP_ROUTE_RANGE_OVERFLOW,
    LPR_STARTUP_MMAP_ROUTE_PAST_FILE_IMAGE,
    LPR_STARTUP_MMAP_ROUTE_LOCAL_FALLBACK,
    LPR_STARTUP_MMAP_ROUTE_SPLIT_IMAGE_AND_ANON_TAIL,
    LPR_STARTUP_MMAP_ROUTE_COUNT,
};

#if defined(LPR_STARTUP_PROFILE) && LPR_STARTUP_PROFILE
#ifndef LPR_STARTUP_PROFILE_MIN_MAPPED_BYTES
#define LPR_STARTUP_PROFILE_MIN_MAPPED_BYTES (4u * 1024u * 1024u)
#endif
enum {
    LPR_STARTUP_PROFILE_PATH_SLOTS = 512,
    LPR_STARTUP_PROFILE_FD_SLOTS = 256,
    LPR_STARTUP_PROFILE_DUMP_SLOTS = 40,
    LPR_STARTUP_PROFILE_FILED_DUMP_BYTES = 8u * 1024u * 1024u,
};

typedef struct lpr_startup_profile_path {
    uint64_t path_hash;
    uint64_t open_count;
    uint64_t open_cycles;
    uint64_t read_count;
    uint64_t read_cycles;
    uint64_t mmap_count;
    uint64_t mmap_cycles;
    uint64_t mmap_bytes;
    uint64_t file_vmo_count;
    uint64_t file_vmo_cycles;
    uint64_t local_pread_count;
    uint64_t local_pread_cycles;
    uint64_t patch_count;
    uint64_t patch_cycles;
    uint64_t patch_bytes;
    uint64_t patched_sites;
    uint64_t skipped_sites;
    uint64_t failed_sites;
    uint64_t errors;
    uint64_t path_sample[3];
    uint8_t emitted;
    uint8_t patch_emitted;
    uint8_t sample_emitted;
} lpr_startup_profile_path_t;

typedef struct lpr_startup_profile_fd {
    uint64_t fd;
    uint64_t path_hash;
    uint8_t active;
} lpr_startup_profile_fd_t;

static lpr_startup_profile_path_t
    lpr_startup_profile_paths[LPR_STARTUP_PROFILE_PATH_SLOTS];
static lpr_startup_profile_fd_t
    lpr_startup_profile_fds[LPR_STARTUP_PROFILE_FD_SLOTS];
static uint64_t lpr_startup_profile_mapped_bytes;
static uint64_t
    lpr_startup_profile_mmap_stage_cycles[LPR_STARTUP_MMAP_STAGE_COUNT];
static uint64_t
    lpr_startup_profile_mmap_stage_counts[LPR_STARTUP_MMAP_STAGE_COUNT];
static uint64_t
    lpr_startup_profile_mmap_route_counts[LPR_STARTUP_MMAP_ROUTE_COUNT];
static uint32_t lpr_startup_profile_lock;
static uint8_t lpr_startup_profile_enabled;

static lpr_startup_profile_path_t *lpr_startup_profile_path(
    uint64_t path_hash);
static uint64_t lpr_startup_profile_fd_hash(uint64_t fd);

static void lpr_startup_profile_acquire(void)
{
    while (__atomic_exchange_n(
        &lpr_startup_profile_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
    {
        __asm__ volatile("pause");
    }
}

static void lpr_startup_profile_release(void)
{
    __atomic_store_n(&lpr_startup_profile_lock, 0u, __ATOMIC_RELEASE);
}

static inline uint64_t lpr_startup_profile_stage_begin(void)
{
    return pacha_trace_read_tsc();
}

static uint64_t lpr_startup_profile_stage_end(uint32_t stage, uint64_t start)
{
    const uint64_t end = pacha_trace_read_tsc();
    if (stage >= LPR_STARTUP_MMAP_STAGE_COUNT || end < start) {
        return 0;
    }
    lpr_startup_profile_acquire();
    lpr_startup_profile_mmap_stage_cycles[stage] += end - start;
    lpr_startup_profile_mmap_stage_counts[stage]++;
    lpr_startup_profile_release();
    return end - start;
}

static void lpr_startup_profile_mmap_route(uint32_t route)
{
    if (route >= LPR_STARTUP_MMAP_ROUTE_COUNT) {
        return;
    }
    lpr_startup_profile_acquire();
    lpr_startup_profile_mmap_route_counts[route]++;
    lpr_startup_profile_release();
}

static void lpr_startup_profile_mmap_backend(
    uint64_t fd, uint8_t file_vmo, uint64_t cycles)
{
    lpr_startup_profile_acquire();
    lpr_startup_profile_path_t *metric =
        lpr_startup_profile_path(lpr_startup_profile_fd_hash(fd));
    if (metric != 0) {
        if (file_vmo) {
            metric->file_vmo_count++;
            metric->file_vmo_cycles += cycles;
        } else {
            metric->local_pread_count++;
            metric->local_pread_cycles += cycles;
        }
    }
    lpr_startup_profile_release();
}

static void lpr_startup_profile_patch(
    uint64_t fd,
    const struct lpr_patch_mapping_result *result)
{
    if (result == 0) {
        return;
    }
    lpr_startup_profile_acquire();
    lpr_startup_profile_path_t *metric =
        lpr_startup_profile_path(lpr_startup_profile_fd_hash(fd));
    if (metric != 0) {
        metric->patch_count++;
        metric->patch_cycles += result->cycles;
        metric->patch_bytes += result->scanned_bytes;
        metric->patched_sites += result->patched_sites;
        metric->skipped_sites += result->skipped_sites;
        metric->failed_sites += result->failed_sites;
    }
    lpr_startup_profile_release();
}

static void lpr_startup_profile_enable(void)
{
    if (lpr_startup_profile_enabled) {
        return;
    }
    lpr_startup_profile_enabled = 1;
    pacha_trace_set_masks(
        PACHA_TRACE_COMPONENT_BIT(PACHA_TRACE_COMPONENT_LPR),
        PACHA_TRACE_CLASS_ERROR | PACHA_TRACE_CLASS_METRIC);
}

static lpr_startup_profile_path_t *lpr_startup_profile_path(uint64_t path_hash)
{
    lpr_startup_profile_path_t *empty = 0;
    if (path_hash == 0) {
        return 0;
    }
    for (uint64_t i = 0; i < LPR_STARTUP_PROFILE_PATH_SLOTS; ++i) {
        lpr_startup_profile_path_t *entry = &lpr_startup_profile_paths[i];
        if (entry->path_hash == path_hash) {
            return entry;
        }
        if (entry->path_hash == 0 && empty == 0) {
            empty = entry;
        }
    }
    if (empty != 0) {
        empty->path_hash = path_hash;
    }
    return empty;
}

static uint64_t lpr_startup_profile_fd_hash(uint64_t fd)
{
    for (uint64_t i = 0; i < LPR_STARTUP_PROFILE_FD_SLOTS; ++i) {
        const lpr_startup_profile_fd_t *entry = &lpr_startup_profile_fds[i];
        if (entry->active && entry->fd == fd) {
            return entry->path_hash;
        }
    }
    return 0;
}

static void lpr_startup_profile_set_fd(uint64_t fd, uint64_t path_hash)
{
    lpr_startup_profile_fd_t *empty = 0;
    if (path_hash == 0) {
        return;
    }
    for (uint64_t i = 0; i < LPR_STARTUP_PROFILE_FD_SLOTS; ++i) {
        lpr_startup_profile_fd_t *entry = &lpr_startup_profile_fds[i];
        if (entry->active && entry->fd == fd) {
            entry->path_hash = path_hash;
            return;
        }
        if (!entry->active && empty == 0) {
            empty = entry;
        }
    }
    if (empty != 0) {
        empty->active = 1;
        empty->fd = fd;
        empty->path_hash = path_hash;
    }
}

static void lpr_startup_profile_clear_fd(uint64_t fd)
{
    for (uint64_t i = 0; i < LPR_STARTUP_PROFILE_FD_SLOTS; ++i) {
        lpr_startup_profile_fd_t *entry = &lpr_startup_profile_fds[i];
        if (entry->active && entry->fd == fd) {
            lpr_memset(entry, 0, sizeof(*entry));
            return;
        }
    }
}

static uint64_t lpr_startup_profile_open_path_hash(
    uint64_t nr,
    uint64_t a0,
    uint64_t a1,
    int64_t result)
{
    const char *path = 0;
    if (nr == LPR_LINUX_SYS_OPEN) {
        path = (const char *)(uintptr_t)a0;
    } else if (nr == LPR_LINUX_SYS_OPENAT) {
        path = (const char *)(uintptr_t)a1;
    }
    if (path == 0 || (result < 0 && result != -LPR_LINUX_ENOENT)) {
        return 0;
    }
    return pacha_trace_name_id(path);
}

static void lpr_startup_profile_record(
    uint64_t nr,
    uint64_t a0,
    uint64_t a1,
    uint64_t a2,
    uint64_t a3,
    uint64_t a4,
    uint64_t a5,
    int64_t result,
    uint64_t cycles)
{
    (void)a3;
    (void)a5;
    lpr_startup_profile_acquire();
    uint64_t path_hash = 0;
    uint64_t bytes = 0;
    uint32_t category = 0;
    if (nr == LPR_LINUX_SYS_OPEN || nr == LPR_LINUX_SYS_OPENAT) {
        path_hash = lpr_startup_profile_open_path_hash(nr, a0, a1, result);
        category = 1;
    } else if (nr == LPR_LINUX_SYS_READ ||
               nr == LPR_LINUX_SYS_PREAD64 ||
               nr == LPR_LINUX_SYS_READV ||
               nr == LPR_LINUX_SYS_LSEEK ||
               nr == LPR_LINUX_SYS_FTRUNCATE)
    {
        path_hash = lpr_startup_profile_fd_hash(a0);
        category = 2;
        if (result > 0 &&
            (nr == LPR_LINUX_SYS_READ ||
             nr == LPR_LINUX_SYS_PREAD64 ||
             nr == LPR_LINUX_SYS_READV))
        {
            bytes = (uint64_t)result;
        } else if (nr == LPR_LINUX_SYS_READ || nr == LPR_LINUX_SYS_PREAD64) {
            bytes = a2;
        }
    } else if (nr == LPR_LINUX_SYS_MMAP) {
        path_hash = lpr_startup_profile_fd_hash(a4);
        category = 3;
        if (result >= 4096) {
            bytes = a1;
        }
    } else if (nr == LPR_LINUX_SYS_CLOSE) {
        path_hash = lpr_startup_profile_fd_hash(a0);
    } else if ((nr == LPR_LINUX_SYS_DUP ||
                nr == LPR_LINUX_SYS_DUP2 ||
                nr == LPR_LINUX_SYS_DUP3) &&
               result >= 0)
    {
        path_hash = lpr_startup_profile_fd_hash(a0);
    }

    lpr_startup_profile_path_t *metric =
        lpr_startup_profile_path(path_hash);
    if (metric != 0) {
        if (category == 1) {
            const char *path = nr == LPR_LINUX_SYS_OPEN ?
                (const char *)(uintptr_t)a0 : (const char *)(uintptr_t)a1;
            if (metric->path_sample[0] == 0 && path != 0) {
                char *sample = (char *)metric->path_sample;
                for (uint32_t i = 0; i < sizeof(metric->path_sample) &&
                     path[i] != 0; i++) {
                    sample[i] = path[i];
                }
            }
            metric->open_count++;
            metric->open_cycles += cycles;
        } else if (category == 2) {
            metric->read_count++;
            metric->read_cycles += cycles;
        } else if (category == 3) {
            metric->mmap_count++;
            metric->mmap_cycles += cycles;
            metric->mmap_bytes += bytes;
            lpr_startup_profile_mapped_bytes += bytes;
        }
        if (result < 0) {
            metric->errors++;
        }
    }
    if (category == 1 && result >= 0) {
        lpr_startup_profile_set_fd((uint64_t)result, path_hash);
    } else if ((nr == LPR_LINUX_SYS_DUP ||
                nr == LPR_LINUX_SYS_DUP2 ||
                nr == LPR_LINUX_SYS_DUP3) &&
               result >= 0)
    {
        lpr_startup_profile_set_fd((uint64_t)result, path_hash);
    } else if (nr == LPR_LINUX_SYS_CLOSE && result == 0) {
        lpr_startup_profile_clear_fd(a0);
    }
    lpr_startup_profile_release();
}

static uint64_t lpr_startup_profile_total_cycles(
    const lpr_startup_profile_path_t *entry)
{
    return entry->open_cycles + entry->read_cycles + entry->mmap_cycles;
}

static void lpr_startup_profile_dump(void)
{
    if (lpr_startup_profile_mapped_bytes <
        LPR_STARTUP_PROFILE_MIN_MAPPED_BYTES)
    {
        return;
    }
    if (lpr_startup_profile_mapped_bytes >=
        LPR_STARTUP_PROFILE_FILED_DUMP_BYTES)
    {
        uint64_t ignored = 0;
        (void)lpr_filed_call(
            FILED_OP_DIAG_DUMP_METRICS, -1, 0, &ignored);
    }
    const uint64_t pid =
        (uint64_t)lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID);
    const uint64_t cycles_marker =
        pacha_trace_name_id("startup.path.cycles");
    const uint64_t counts_marker =
        pacha_trace_name_id("startup.path.counts");
    const uint64_t bytes_marker =
        pacha_trace_name_id("startup.path.bytes");
    const uint64_t backend_cycles_marker =
        pacha_trace_name_id("startup.path.mmap_backend_cycles");
    const uint64_t backend_counts_marker =
        pacha_trace_name_id("startup.path.mmap_backend_counts");
    const uint64_t patch_cycles_marker =
        pacha_trace_name_id("startup.path.patch.cycles");
    const uint64_t patch_counts_marker =
        pacha_trace_name_id("startup.path.patch.counts");
    const uint64_t patch_top_marker =
        pacha_trace_name_id("startup.patch.path");
    const uint64_t patch_sites_marker =
        pacha_trace_name_id("startup.patch.sites");
    const uint64_t path_sample_marker =
        pacha_trace_name_id("startup.path.sample");
    const uint64_t stage_cycles_a_marker =
        pacha_trace_name_id("startup.mmap.stage.cycles.a");
    const uint64_t stage_cycles_b_marker =
        pacha_trace_name_id("startup.mmap.stage.cycles.b");
    const uint64_t stage_cycles_c_marker =
        pacha_trace_name_id("startup.mmap.stage.cycles.c");
    const uint64_t stage_counts_a_marker =
        pacha_trace_name_id("startup.mmap.stage.counts.a");
    const uint64_t stage_counts_b_marker =
        pacha_trace_name_id("startup.mmap.stage.counts.b");
    const uint64_t stage_counts_c_marker =
        pacha_trace_name_id("startup.mmap.stage.counts.c");
    const uint64_t route_counts_a_marker =
        pacha_trace_name_id("startup.mmap.route.counts.a");
    const uint64_t route_counts_b_marker =
        pacha_trace_name_id("startup.mmap.route.counts.b");
    const uint64_t route_counts_c_marker =
        pacha_trace_name_id("startup.mmap.route.counts.c");
    for (uint64_t rank = 0; rank < LPR_STARTUP_PROFILE_DUMP_SLOTS; ++rank) {
        lpr_startup_profile_path_t *best = 0;
        uint64_t best_cycles = 0;
        for (uint64_t i = 0; i < LPR_STARTUP_PROFILE_PATH_SLOTS; ++i) {
            lpr_startup_profile_path_t *entry =
                &lpr_startup_profile_paths[i];
            const uint64_t total = lpr_startup_profile_total_cycles(entry);
            if (!entry->emitted && entry->path_hash != 0 &&
                (best == 0 || total > best_cycles))
            {
                best = entry;
                best_cycles = total;
            }
        }
        if (best == 0) {
            break;
        }
        best->emitted = 1;
        pacha_trace6(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
            PACHA_TRACE_CLASS_METRIC,
            cycles_marker,
            pid,
            best->path_hash,
            best->open_cycles,
            best->read_cycles,
            best->mmap_cycles);
        pacha_trace6(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
            PACHA_TRACE_CLASS_METRIC,
            counts_marker,
            pid,
            best->path_hash,
            best->open_count,
            best->read_count,
            best->mmap_count);
        pacha_trace5(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
            PACHA_TRACE_CLASS_METRIC,
            bytes_marker,
            pid,
            best->path_hash,
            best->mmap_bytes,
            best->errors);
        pacha_trace5(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
            PACHA_TRACE_CLASS_METRIC,
            backend_cycles_marker,
            pid,
            best->path_hash,
            best->file_vmo_cycles,
            best->local_pread_cycles);
        pacha_trace5(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
            PACHA_TRACE_CLASS_METRIC,
            backend_counts_marker,
            pid,
            best->path_hash,
            best->file_vmo_count,
            best->local_pread_count);
        pacha_trace6(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
            PACHA_TRACE_CLASS_METRIC,
            patch_cycles_marker,
            pid,
            best->path_hash,
            best->patch_cycles,
            best->patch_bytes,
            best->patched_sites);
        pacha_trace6(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
            PACHA_TRACE_CLASS_METRIC,
            patch_counts_marker,
            pid,
            best->path_hash,
            best->patch_count,
            best->skipped_sites,
            best->failed_sites);
    }
    pacha_trace5(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
        PACHA_TRACE_CLASS_METRIC,
        stage_cycles_a_marker,
        pid,
        lpr_startup_profile_mmap_stage_cycles[LPR_STARTUP_MMAP_STAGE_CACHE_LOOKUP],
        lpr_startup_profile_mmap_stage_cycles[LPR_STARTUP_MMAP_STAGE_FILE_VMO],
        lpr_startup_profile_mmap_stage_cycles[LPR_STARTUP_MMAP_STAGE_VMO_CREATE]);
    pacha_trace5(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
        PACHA_TRACE_CLASS_METRIC,
        stage_cycles_b_marker,
        pid,
        lpr_startup_profile_mmap_stage_cycles[LPR_STARTUP_MMAP_STAGE_PREAD_TO_VMO],
        lpr_startup_profile_mmap_stage_cycles[LPR_STARTUP_MMAP_STAGE_NATIVE_MMAP],
        lpr_startup_profile_mmap_stage_cycles[LPR_STARTUP_MMAP_STAGE_PATCH]);
    pacha_trace4(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
        PACHA_TRACE_CLASS_METRIC,
        stage_cycles_c_marker,
        pid,
        lpr_startup_profile_mmap_stage_cycles[LPR_STARTUP_MMAP_STAGE_MPROTECT],
        lpr_startup_profile_mmap_stage_cycles[LPR_STARTUP_MMAP_STAGE_FALLBACK_COPY]);
    pacha_trace5(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
        PACHA_TRACE_CLASS_METRIC,
        stage_counts_a_marker,
        pid,
        lpr_startup_profile_mmap_stage_counts[LPR_STARTUP_MMAP_STAGE_CACHE_LOOKUP],
        lpr_startup_profile_mmap_stage_counts[LPR_STARTUP_MMAP_STAGE_FILE_VMO],
        lpr_startup_profile_mmap_stage_counts[LPR_STARTUP_MMAP_STAGE_VMO_CREATE]);
    pacha_trace5(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
        PACHA_TRACE_CLASS_METRIC,
        stage_counts_b_marker,
        pid,
        lpr_startup_profile_mmap_stage_counts[LPR_STARTUP_MMAP_STAGE_PREAD_TO_VMO],
        lpr_startup_profile_mmap_stage_counts[LPR_STARTUP_MMAP_STAGE_NATIVE_MMAP],
        lpr_startup_profile_mmap_stage_counts[LPR_STARTUP_MMAP_STAGE_PATCH]);
    pacha_trace4(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
        PACHA_TRACE_CLASS_METRIC,
        stage_counts_c_marker,
        pid,
        lpr_startup_profile_mmap_stage_counts[LPR_STARTUP_MMAP_STAGE_MPROTECT],
        lpr_startup_profile_mmap_stage_counts[LPR_STARTUP_MMAP_STAGE_FALLBACK_COPY]);
    pacha_trace6(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
        PACHA_TRACE_CLASS_METRIC,
        route_counts_a_marker,
        pid,
        lpr_startup_profile_mmap_route_counts[LPR_STARTUP_MMAP_ROUTE_CACHE_HIT],
        lpr_startup_profile_mmap_route_counts[LPR_STARTUP_MMAP_ROUTE_CACHE_MAP_FAILED],
        lpr_startup_profile_mmap_route_counts[LPR_STARTUP_MMAP_ROUTE_FILE_VMO_MAPPED],
        lpr_startup_profile_mmap_route_counts[LPR_STARTUP_MMAP_ROUTE_FILE_VMO_RPC_FAILED]);
    pacha_trace6(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
        PACHA_TRACE_CLASS_METRIC,
        route_counts_b_marker,
        pid,
        lpr_startup_profile_mmap_route_counts[LPR_STARTUP_MMAP_ROUTE_FILE_VMO_MAP_FAILED],
        lpr_startup_profile_mmap_route_counts[LPR_STARTUP_MMAP_ROUTE_NO_FILED_BACKEND],
        lpr_startup_profile_mmap_route_counts[LPR_STARTUP_MMAP_ROUTE_NO_GENERATION],
        lpr_startup_profile_mmap_route_counts[LPR_STARTUP_MMAP_ROUTE_EMPTY_FILE]);
    pacha_trace6(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
        PACHA_TRACE_CLASS_METRIC,
        route_counts_c_marker,
        pid,
        lpr_startup_profile_mmap_route_counts[LPR_STARTUP_MMAP_ROUTE_RANGE_OVERFLOW],
        lpr_startup_profile_mmap_route_counts[LPR_STARTUP_MMAP_ROUTE_PAST_FILE_IMAGE],
        lpr_startup_profile_mmap_route_counts[LPR_STARTUP_MMAP_ROUTE_LOCAL_FALLBACK],
        lpr_startup_profile_mmap_route_counts[LPR_STARTUP_MMAP_ROUTE_SPLIT_IMAGE_AND_ANON_TAIL]);
    /* Emit the expensive executable ranges last. The trace ring is bounded,
     * and open/read probes can otherwise evict the library rows that explain
     * the aggregate patch time before the ring is dumped. */
    for (uint64_t rank = 0; rank < 24; ++rank) {
        lpr_startup_profile_path_t *best = 0;
        for (uint64_t i = 0; i < LPR_STARTUP_PROFILE_PATH_SLOTS; ++i) {
            lpr_startup_profile_path_t *entry =
                &lpr_startup_profile_paths[i];
            if (!entry->patch_emitted && entry->path_hash != 0 &&
                entry->patch_count != 0 &&
                (best == 0 || entry->patch_cycles > best->patch_cycles))
            {
                best = entry;
            }
        }
        if (best == 0) {
            break;
        }
        best->patch_emitted = 1;
        pacha_trace6(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
            PACHA_TRACE_CLASS_METRIC,
            patch_top_marker,
            pid,
            best->path_hash,
            best->patch_cycles,
            best->patch_count,
            best->patch_bytes);
        pacha_trace4(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
            PACHA_TRACE_CLASS_METRIC,
            patch_sites_marker,
            pid,
            best->path_hash,
            best->patched_sites);
    }
    pacha_trace_dump_ring();
    /* Hashes are compact but insufficient for transient /proc, /sys and
     * generated paths. Emit a bounded prefix for the hottest rows only after
     * the normal dump so these diagnostics cannot evict timing records. */
    for (uint64_t rank = 0; rank < 16; ++rank) {
        lpr_startup_profile_path_t *best = 0;
        for (uint64_t i = 0; i < LPR_STARTUP_PROFILE_PATH_SLOTS; ++i) {
            lpr_startup_profile_path_t *entry = &lpr_startup_profile_paths[i];
            if (!entry->sample_emitted && entry->path_hash != 0 &&
                entry->path_sample[0] != 0 &&
                (best == 0 || lpr_startup_profile_total_cycles(entry) >
                    lpr_startup_profile_total_cycles(best))) {
                best = entry;
            }
        }
        if (best == 0) break;
        best->sample_emitted = 1;
        pacha_trace6(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
            PACHA_TRACE_CLASS_METRIC,
            path_sample_marker,
            pid,
            best->path_hash,
            best->path_sample[0],
            best->path_sample[1],
            best->path_sample[2]);
    }
    pacha_trace_dump_ring();
}
#else
static inline uint64_t lpr_startup_profile_stage_begin(void)
{
    return 0;
}

static inline uint64_t lpr_startup_profile_stage_end(uint32_t stage, uint64_t start)
{
    (void)stage;
    (void)start;
    return 0;
}

static inline void lpr_startup_profile_mmap_route(uint32_t route)
{
    (void)route;
}

static inline void lpr_startup_profile_mmap_backend(
    uint64_t fd, uint8_t file_vmo, uint64_t cycles)
{
    (void)fd;
    (void)file_vmo;
    (void)cycles;
}

static inline void lpr_startup_profile_patch(
    uint64_t fd,
    const struct lpr_patch_mapping_result *result)
{
    (void)fd;
    (void)result;
}
#endif

static void lpr_trace_enosys_syscall(uint64_t nr,
                                     uint64_t a0,
                                     uint64_t a1,
                                     uint64_t a2,
                                     uint64_t a3,
                                     uint64_t a4,
                                     uint64_t a5)
{
    pacha_trace6(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_EVENT_LPR_ENOSYS, PACHA_TRACE_CLASS_ERROR, nr, a0, a1, a2, a3, a4);
    pacha_trace2(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_EVENT_LPR_ENOSYS, PACHA_TRACE_CLASS_ERROR, nr, a5);
}

static uint64_t lpr_mmap_page_align_up(uint64_t value)
{
    const uint64_t mask = 4095ull;
    if (value > UINT64_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

static void lpr_copy_uts_field(char *dst, const char *src)
{
    uint64_t i = 0;
    for (; i < 64 && src[i] != 0; i += 1) {
        dst[i] = src[i];
    }
    for (; i < 65; i += 1) {
        dst[i] = 0;
    }
}

static int64_t lpr_linux_uname(uint64_t uts_raw)
{
    if (uts_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    char *uts = (char *)(uintptr_t)uts_raw;
    lpr_memset(uts, 0, 65u * 6u);
    lpr_copy_uts_field(uts + 65u * 0u, "Linux");
    lpr_copy_uts_field(uts + 65u * 1u, "pachaos");
    lpr_copy_uts_field(uts + 65u * 2u, "6.12.0");
    lpr_copy_uts_field(uts + 65u * 3u, "PachaOS Linux shim");
    lpr_copy_uts_field(uts + 65u * 4u, "x86_64");
    lpr_copy_uts_field(uts + 65u * 5u, "localdomain");
    return 0;
}

static void lpr_file_image_cache_clear_locked(void)
{
    for (uint64_t i = 0; i < LPR_FILE_IMAGE_CACHE_ENTRIES; ++i) {
        lpr_file_image_cache_entry_t *entry = &lpr_file_image_cache[i];
        if (entry->active) {
            if (entry->vmo_fd >= 16) {
                (void)lpr_pacha_syscall1(
                    PACHAOS_SYSCALL_FD_CLOSE, entry->vmo_fd);
            }
            lpr_memset(entry, 0, sizeof(*entry));
        }
    }
    lpr_file_image_cache_clock = 0;
}

void lpr_file_image_cache_clear(void)
{
    lpr_state_lock(&lpr_file_image_cache_lock);
    lpr_file_image_cache_clear_locked();
    lpr_state_unlock(&lpr_file_image_cache_lock);
}

void lpr_file_image_cache_pause(void)
{
    lpr_state_lock(&lpr_file_image_cache_lock);
    if (lpr_file_image_cache_pause_count == 0) {
        /* A mapped VMA retains its VMO independently of this descriptor-only
         * lookup cache.  Drop the cache before a manifest reserves transient
         * transfer leases so cached ELF images cannot exhaust native FDs. */
        lpr_file_image_cache_clear_locked();
    }
    lpr_file_image_cache_pause_count++;
    lpr_state_unlock(&lpr_file_image_cache_lock);
}

void lpr_file_image_cache_resume(void)
{
    lpr_state_lock(&lpr_file_image_cache_lock);
    if (lpr_file_image_cache_pause_count != 0) {
        lpr_file_image_cache_pause_count--;
    }
    lpr_state_unlock(&lpr_file_image_cache_lock);
}

void lpr_file_image_cache_after_fork_child(void)
{
    __atomic_store_n(&lpr_file_image_cache_lock, 0u, __ATOMIC_RELEASE);
    lpr_file_image_cache_pause_count = 0;
    lpr_file_image_cache_clear();
}

static lpr_file_image_cache_entry_t *lpr_file_image_cache_acquire(
    uint64_t handle,
    uint64_t length,
    uint64_t *out_generation)
{
    uint64_t generation = 0;
    if (out_generation != 0) {
        *out_generation = 0;
    }
    const int generation_status =
        lpr_filed_live_object_generation(handle, &generation);
    lpr_state_lock(&lpr_file_image_cache_lock);
    if (lpr_file_image_cache_pause_count != 0) {
        lpr_state_unlock(&lpr_file_image_cache_lock);
        return 0;
    }
    lpr_file_image_cache_entry_t *hit = 0;
    for (uint64_t i = 0; i < LPR_FILE_IMAGE_CACHE_ENTRIES; ++i) {
        lpr_file_image_cache_entry_t *entry = &lpr_file_image_cache[i];
        if (!entry->active || entry->handle != handle) {
            continue;
        }
        if (generation_status != 0 ||
            entry->object_generation != generation ||
            entry->length != length)
        {
            if (entry->vmo_fd >= 16) {
                (void)lpr_pacha_syscall1(
                    PACHAOS_SYSCALL_FD_CLOSE, entry->vmo_fd);
            }
            lpr_memset(entry, 0, sizeof(*entry));
            continue;
        }
        entry->clock = ++lpr_file_image_cache_clock;
        hit = entry;
        break;
    }
    if (hit == 0) {
        lpr_state_unlock(&lpr_file_image_cache_lock);
    } else if (out_generation != 0) {
        *out_generation = generation;
    }
    return hit;
}

static void lpr_file_image_cache_release(void)
{
    lpr_state_unlock(&lpr_file_image_cache_lock);
}

static void lpr_file_image_cache_store(
    uint64_t handle,
    uint64_t object_generation,
    uint64_t length,
    int64_t vmo_fd)
{
    if (handle == 0 || object_generation == 0 || length == 0 || vmo_fd < 16) {
        return;
    }
    lpr_state_lock(&lpr_file_image_cache_lock);
    if (lpr_file_image_cache_pause_count != 0) {
        (void)lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
        lpr_state_unlock(&lpr_file_image_cache_lock);
        return;
    }
    lpr_file_image_cache_entry_t *slot = 0;
    lpr_file_image_cache_entry_t *oldest = 0;
    for (uint64_t i = 0; i < LPR_FILE_IMAGE_CACHE_ENTRIES; ++i) {
        lpr_file_image_cache_entry_t *entry = &lpr_file_image_cache[i];
        if (entry->active &&
            entry->handle == handle &&
            entry->object_generation == object_generation &&
            entry->length == length)
        {
            (void)lpr_pacha_syscall1(
                PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
            entry->clock = ++lpr_file_image_cache_clock;
            lpr_state_unlock(&lpr_file_image_cache_lock);
            return;
        }
        if (!entry->active && slot == 0) {
            slot = entry;
        }
        if (entry->active && (oldest == 0 || entry->clock < oldest->clock)) {
            oldest = entry;
        }
    }
    if (slot == 0) {
        slot = oldest;
        if (slot != 0) {
            if (slot->vmo_fd >= 16) {
                (void)lpr_pacha_syscall1(
                    PACHAOS_SYSCALL_FD_CLOSE, slot->vmo_fd);
            }
            lpr_memset(slot, 0, sizeof(*slot));
        }
    }
    if (slot != 0) {
        slot->active = 1;
        slot->vmo_fd = (uint32_t)vmo_fd;
        slot->handle = handle;
        slot->object_generation = object_generation;
        slot->length = length;
        slot->clock = ++lpr_file_image_cache_clock;
    } else {
        (void)lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
    }
    lpr_state_unlock(&lpr_file_image_cache_lock);
}

static int64_t lpr_linux_pacha_status_to_errno(int64_t status)
{
    return pacha_kernel_status_to_errno(status);
}

static uint64_t lpr_linux_prot_to_pacha(uint64_t prot)
{
    uint64_t out = 0;
    if ((prot & LPR_LINUX_PROT_READ) != 0) {
        out |= PACHAOS_PROT_READ;
    }
    if ((prot & LPR_LINUX_PROT_WRITE) != 0) {
        out |= PACHAOS_PROT_READ | PACHAOS_PROT_WRITE;
    }
    if ((prot & LPR_LINUX_PROT_EXEC) != 0) {
        out |= PACHAOS_PROT_EXEC;
    }
    return out;
}

static int lpr_linux_mmap_flags_to_pacha(uint64_t flags, uint64_t *out)
{
    uint64_t pacha = 0;
    const int shared = (flags & LPR_LINUX_MAP_SHARED) != 0;
    const int private = (flags & LPR_LINUX_MAP_PRIVATE) != 0;
    if (out == 0 || shared == private) {
        return -LPR_LINUX_EINVAL;
    }
    if (shared) {
        pacha |= PACHAOS_MMAP_SHARED;
    }
    if (private) {
        pacha |= PACHAOS_MMAP_PRIVATE;
    }
    if ((flags & LPR_LINUX_MAP_FIXED) != 0) {
        pacha |= PACHAOS_MMAP_FIXED;
    }
    if ((flags & LPR_LINUX_MAP_FIXED_NOREPLACE) != 0) {
        pacha |= PACHAOS_MMAP_FIXED_NOREPLACE;
    }
    if ((flags & LPR_LINUX_MAP_ANONYMOUS) != 0) {
        pacha |= PACHAOS_MMAP_ANONYMOUS;
    }
    if ((flags & LPR_LINUX_MAP_NORESERVE) != 0) {
        pacha |= PACHAOS_MMAP_NORESERVE;
    }
    *out = pacha;
    return 0;
}

static int64_t lpr_dispatch_arch_prctl(uint64_t code, uint64_t value)
{
    switch (code) {
    case LPR_LINUX_ARCH_SET_FS:
        return lpr_linux_pacha_status_to_errno(lpr_pacha_syscall1(PACHAOS_SYSCALL_THREAD_SET_FS_BASE, value));
    case LPR_LINUX_ARCH_SET_GS:
        return lpr_linux_pacha_status_to_errno(lpr_pacha_syscall1(PACHAOS_SYSCALL_THREAD_SET_GS_BASE, value));
    case LPR_LINUX_ARCH_GET_FS:
    case LPR_LINUX_ARCH_GET_GS:
        return -LPR_LINUX_ENOSYS;
    default:
        return -LPR_LINUX_EINVAL;
    }
}

static int64_t lpr_map_private_file_image(
    uint64_t vmo_fd,
    uint64_t addr,
    uint64_t map_len,
    uint64_t file_map_len,
    uint64_t prot,
    uint64_t map_flags,
    uint64_t offset)
{
    if (file_map_len == map_len) {
        return lpr_pacha_syscall6(
            PACHAOS_SYSCALL_MMAP,
            vmo_fd,
            addr,
            map_len,
            prot,
            map_flags,
            offset);
    }
    const uint64_t reservation_flags =
        (map_flags | PACHAOS_MMAP_ANONYMOUS) & ~PACHAOS_MMAP_SHARED;
    const int64_t reservation = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        0,
        addr,
        map_len,
        prot,
        reservation_flags,
        0);
    if (reservation < 4096) {
        return reservation;
    }
    const uint64_t prefix_flags =
        ((map_flags | PACHAOS_MMAP_FIXED | PACHAOS_MMAP_PRIVATE) &
         ~(PACHAOS_MMAP_SHARED | PACHAOS_MMAP_FIXED_NOREPLACE));
    const int64_t prefix = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        vmo_fd,
        (uint64_t)reservation,
        file_map_len,
        prot,
        prefix_flags,
        offset);
    if (prefix < 4096) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP, (uint64_t)reservation, map_len);
        return prefix;
    }
    return reservation;
}

int64_t lpr_backend_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset)
{
    uint64_t pacha_flags;
    const int flag_status = lpr_linux_mmap_flags_to_pacha(flags, &pacha_flags);
    if (flag_status != 0 || len == 0) {
        return -LPR_LINUX_EINVAL;
    }
    if ((flags & LPR_LINUX_MAP_ANONYMOUS) == 0 && lpr_linux_dmabuf_fd_active(fd)) {
        lpr_dmabuf_backend_t *dmabuf = lpr_dmabuf_backend(fd);
        if (dmabuf == 0 || dmabuf->native.raw < 0 ||
            (offset & 4095ull) != 0 || offset > dmabuf->size ||
            len > dmabuf->size - offset ||
            ((prot & LPR_LINUX_PROT_WRITE) != 0 && !dmabuf->writable)) {
            return -LPR_LINUX_EINVAL;
        }
        const int64_t mapped = lpr_pacha_syscall6(
            PACHAOS_SYSCALL_MMAP,
            (uint64_t)(uint32_t)dmabuf->native.raw,
            addr,
            len,
            lpr_linux_prot_to_pacha(prot),
            pacha_flags,
            offset);
        return mapped >= 4096 ? mapped : lpr_linux_pacha_status_to_errno(mapped);
    }
    if ((flags & LPR_LINUX_MAP_ANONYMOUS) == 0 && lpr_linux_drm_fd_active(fd)) {
        return lpr_drm_mmap(fd, addr, len, lpr_linux_prot_to_pacha(prot), pacha_flags, offset);
    }
    if ((flags & LPR_LINUX_MAP_ANONYMOUS) == 0 && lpr_linux_filed_fd_active(fd)) {
        if ((offset & 4095ull) != 0) {
            return -LPR_LINUX_EINVAL;
        }
        const lpr_filed_backend_t *file = lpr_filed_backend(fd);
        if ((flags & LPR_LINUX_MAP_SHARED) != 0 &&
            (prot & LPR_LINUX_PROT_WRITE) != 0)
        {
            if ((file->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY) {
                return -LPR_LINUX_EACCES;
            }
            if (lpr_memfd_write_is_sealed(file->reserved1)) {
                return -LPR_LINUX_EPERM;
            }
        }
        const uint64_t map_len = lpr_mmap_page_align_up(len);
        if (map_len == 0) {
            return -LPR_LINUX_ENOMEM;
        }
        if ((flags & LPR_LINUX_MAP_SHARED) != 0) {
            uint64_t file_size = 0;
            /* The shared VMO's transferred rights are the lifetime ceiling
             * for mprotect, not merely the initial PTE protection.  Derive
             * write authority from the open file description and seals;
             * Filed still validates it with pwrite_prepare before issuing
             * MAP_WRITE.  Readable Filed mappings may later become RX under
             * the existing executable-file policy. */
            const int writable =
                (file->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDWR &&
                !lpr_memfd_write_is_sealed(file->reserved1);
            const int executable = 1;
            const int64_t shared_vmo_fd = lpr_linux_shared_file_vmo(
                fd,
                offset,
                map_len,
                writable,
                executable,
                &file_size);
            (void)file_size;
            if (shared_vmo_fd < 16) {
                lpr_trace_mmap_error(
                    "shared_file_vmo",
                    addr,
                    len,
                    prot,
                    flags,
                    fd,
                    offset,
                    shared_vmo_fd);
                return shared_vmo_fd;
            }
            const int64_t mapped = lpr_pacha_syscall6(
                PACHAOS_SYSCALL_MMAP,
                (uint64_t)(uint32_t)shared_vmo_fd,
                addr,
                map_len,
                lpr_linux_prot_to_pacha(prot),
                pacha_flags,
                offset);
            (void)lpr_pacha_syscall1(
                PACHAOS_SYSCALL_FD_CLOSE,
                (uint64_t)(uint32_t)shared_vmo_fd);
            if (mapped < 4096) {
                lpr_trace_mmap_error(
                    "shared_mmap",
                    addr,
                    len,
                    prot,
                    flags,
                    fd,
                    offset,
                    mapped);
                return lpr_linux_pacha_status_to_errno(mapped);
            }
            lpr_page_cache_clear();
            lpr_shared_file_mapping_active = 1;
            lpr_trace_mmap_call("mmap", addr, len, prot, flags, fd, offset, mapped);
            return mapped;
        }
        const uint64_t load_prot =
            PACHAOS_PROT_READ |
            PACHAOS_PROT_WRITE;
        const uint64_t final_prot = lpr_linux_prot_to_pacha(prot);
        uint64_t done = 0;
        int64_t mapped = 0;
        int64_t vmo_fd = -1;
        const uint64_t initial_prot =
            (prot & LPR_LINUX_PROT_EXEC) != 0 ? load_prot : final_prot;
        uint64_t mapped_prot = initial_prot;
        const uint64_t private_file_map_flags =
            pacha_flags & (PACHAOS_MMAP_FIXED |
                           PACHAOS_MMAP_FIXED_NOREPLACE |
                           PACHAOS_MMAP_NORESERVE |
                           PACHAOS_MMAP_PRIVATE |
                           PACHAOS_MMAP_SHARED);
        const uint64_t direct_map_flags = private_file_map_flags;
        uint64_t profile_stage = 0;
        lpr_filed_backend_t *private_file = lpr_filed_backend(fd);
        const uint64_t requested_end =
            offset <= UINT64_MAX - map_len ? offset + map_len : 0;
        if (private_file != 0 &&
            (private_file->object_generation == 0 ||
             requested_end == 0 ||
             requested_end > private_file->stat_size))
        {
            lpr_linux_stat_t stat_snapshot;
            lpr_memset(&stat_snapshot, 0, sizeof(stat_snapshot));
            (void)lpr_backend_fstat(
                fd, (uint64_t)(uintptr_t)&stat_snapshot);
        }
        private_file = lpr_filed_backend(fd);
        const uint64_t whole_file_bytes = private_file != 0 ?
            private_file->stat_size : 0;
        const uint64_t whole_file_map_len =
            lpr_mmap_page_align_up(whole_file_bytes);
        const uint8_t split_file_image =
            private_file != 0 &&
            private_file->object_generation != 0 &&
            requested_end != 0 &&
            whole_file_bytes != 0 &&
            offset < whole_file_map_len &&
            requested_end > whole_file_map_len;
        const uint64_t direct_file_map_len = split_file_image ?
            whole_file_map_len - offset : map_len;
        if (split_file_image) {
            lpr_startup_profile_mmap_route(
                LPR_STARTUP_MMAP_ROUTE_PAST_FILE_IMAGE);
        }
        const uint8_t anonymous_eof_tail =
            private_file != 0 &&
            private_file->object_generation != 0 &&
            requested_end != 0 &&
            whole_file_bytes != 0 &&
            offset == whole_file_map_len &&
            requested_end > whole_file_map_len;
        if (anonymous_eof_tail) {
            lpr_startup_profile_mmap_route(
                LPR_STARTUP_MMAP_ROUTE_PAST_FILE_IMAGE);
            profile_stage = lpr_startup_profile_stage_begin();
            mapped = lpr_pacha_syscall6(
                PACHAOS_SYSCALL_MMAP,
                0,
                addr,
                map_len,
                initial_prot,
                ((direct_map_flags | PACHAOS_MMAP_ANONYMOUS |
                  PACHAOS_MMAP_FIXED) &
                 ~(PACHAOS_MMAP_SHARED |
                   PACHAOS_MMAP_FIXED_NOREPLACE)),
                0);
            lpr_startup_profile_stage_end(
                LPR_STARTUP_MMAP_STAGE_NATIVE_MMAP, profile_stage);
            if (mapped >= 4096) {
                lpr_startup_profile_mmap_route(
                    LPR_STARTUP_MMAP_ROUTE_SPLIT_IMAGE_AND_ANON_TAIL);
                done = 0;
                mapped_prot = initial_prot;
                goto private_file_mapping_ready;
            }
        }
        if (private_file != 0 &&
            private_file->object_generation != 0 &&
            requested_end != 0 &&
            whole_file_bytes != 0 &&
            (whole_file_map_len >= requested_end || split_file_image))
        {
            uint64_t live_generation = 0;
            profile_stage = lpr_startup_profile_stage_begin();
            lpr_file_image_cache_entry_t *image =
                lpr_file_image_cache_acquire(
                    private_file->handle,
                    whole_file_bytes,
                    &live_generation);
            lpr_startup_profile_stage_end(
                LPR_STARTUP_MMAP_STAGE_CACHE_LOOKUP, profile_stage);
            if (image != 0) {
                profile_stage = lpr_startup_profile_stage_begin();
                mapped = lpr_map_private_file_image(
                    image->vmo_fd,
                    addr,
                    map_len,
                    direct_file_map_len,
                    initial_prot,
                    direct_map_flags,
                    offset);
                lpr_startup_profile_stage_end(
                    LPR_STARTUP_MMAP_STAGE_NATIVE_MMAP, profile_stage);
                if (mapped >= 4096 && split_file_image) {
                    lpr_startup_profile_mmap_route(
                        LPR_STARTUP_MMAP_ROUTE_SPLIT_IMAGE_AND_ANON_TAIL);
                }
                lpr_file_image_cache_release();
                if (mapped >= 4096) {
                    lpr_startup_profile_mmap_route(
                        LPR_STARTUP_MMAP_ROUTE_CACHE_HIT);
                    const uint64_t available = whole_file_bytes > offset ?
                        whole_file_bytes - offset : 0;
                    done = available < len ? available : len;
                    mapped_prot = initial_prot;
                    goto private_file_mapping_ready;
                }
                lpr_startup_profile_mmap_route(
                    LPR_STARTUP_MMAP_ROUTE_CACHE_MAP_FAILED);
            }
            uint64_t loaded = 0;
            profile_stage = lpr_startup_profile_stage_begin();
            int64_t file_vmo_fd = lpr_linux_file_vmo(
                fd, 0, whole_file_bytes, &loaded);
            const uint64_t file_vmo_cycles = lpr_startup_profile_stage_end(
                LPR_STARTUP_MMAP_STAGE_FILE_VMO, profile_stage);
            lpr_startup_profile_mmap_backend(fd, 1, file_vmo_cycles);
            if (file_vmo_fd >= 16) {
                profile_stage = lpr_startup_profile_stage_begin();
                mapped = lpr_map_private_file_image(
                    (uint64_t)(uint32_t)file_vmo_fd,
                    addr,
                    map_len,
                    direct_file_map_len,
                    initial_prot,
                    direct_map_flags,
                    offset);
                lpr_startup_profile_stage_end(
                    LPR_STARTUP_MMAP_STAGE_NATIVE_MMAP, profile_stage);
                if (mapped >= 4096 && split_file_image) {
                    lpr_startup_profile_mmap_route(
                        LPR_STARTUP_MMAP_ROUTE_SPLIT_IMAGE_AND_ANON_TAIL);
                }
                if (mapped >= 4096) {
                    lpr_startup_profile_mmap_route(
                        LPR_STARTUP_MMAP_ROUTE_FILE_VMO_MAPPED);
                    live_generation = 0;
                    if (lpr_filed_live_object_generation(
                            private_file->handle,
                            &live_generation) == 0)
                    {
                        lpr_file_image_cache_store(
                            private_file->handle,
                            live_generation,
                            whole_file_bytes,
                            file_vmo_fd);
                        file_vmo_fd = -1;
                    }
                    const uint64_t available = loaded > offset ? loaded - offset : 0;
                    done = available < len ? available : len;
                    mapped_prot = initial_prot;
                    if (file_vmo_fd >= 16) {
                        (void)lpr_pacha_syscall1(
                            PACHAOS_SYSCALL_FD_CLOSE,
                            (uint64_t)(uint32_t)file_vmo_fd);
                    }
                    goto private_file_mapping_ready;
                }
                lpr_startup_profile_mmap_route(
                    LPR_STARTUP_MMAP_ROUTE_FILE_VMO_MAP_FAILED);
                (void)lpr_pacha_syscall1(
                    PACHAOS_SYSCALL_FD_CLOSE,
                    (uint64_t)(uint32_t)file_vmo_fd);
            } else {
                lpr_startup_profile_mmap_route(
                    LPR_STARTUP_MMAP_ROUTE_FILE_VMO_RPC_FAILED);
            }
        } else if (private_file == 0) {
            lpr_startup_profile_mmap_route(
                LPR_STARTUP_MMAP_ROUTE_NO_FILED_BACKEND);
        } else if (private_file->object_generation == 0) {
            lpr_startup_profile_mmap_route(
                LPR_STARTUP_MMAP_ROUTE_NO_GENERATION);
        } else if (requested_end == 0) {
            lpr_startup_profile_mmap_route(
                LPR_STARTUP_MMAP_ROUTE_RANGE_OVERFLOW);
        } else if (whole_file_bytes == 0) {
            lpr_startup_profile_mmap_route(
                LPR_STARTUP_MMAP_ROUTE_EMPTY_FILE);
        } else {
            lpr_startup_profile_mmap_route(
                LPR_STARTUP_MMAP_ROUTE_PAST_FILE_IMAGE);
        }
        lpr_startup_profile_mmap_route(
            LPR_STARTUP_MMAP_ROUTE_LOCAL_FALLBACK);
        const uint64_t vmo_rights =
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE |
            PACHA_FD_RIGHT_MAP_EXEC;
        profile_stage = lpr_startup_profile_stage_begin();
        vmo_fd = lpr_pacha_syscall3(
            PACHAOS_SYSCALL_VMO_CREATE,
            map_len,
            vmo_rights,
            0);
        lpr_startup_profile_stage_end(
            LPR_STARTUP_MMAP_STAGE_VMO_CREATE, profile_stage);
        if (vmo_fd < 16) {
            lpr_trace_mmap_error("vmo_create", addr, len, prot, flags, fd, offset, vmo_fd);
            return lpr_linux_pacha_status_to_errno(vmo_fd);
        }
        profile_stage = lpr_startup_profile_stage_begin();
        const int64_t loaded = lpr_linux_pread_to_vmo(fd, (uint64_t)(uint32_t)vmo_fd, 0, len, offset);
        const uint64_t local_pread_cycles = lpr_startup_profile_stage_end(
            LPR_STARTUP_MMAP_STAGE_PREAD_TO_VMO, profile_stage);
        lpr_startup_profile_mmap_backend(fd, 0, local_pread_cycles);
        if (loaded < 0) {
            lpr_trace_mmap_error("pread_to_vmo", addr, len, prot, flags, fd, offset, loaded);
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
            return loaded;
        }
        done = (uint64_t)loaded;
        /* A file mapping has to agree with the file it names.  A short read
         * leaves the rest of the VMO zero and hands the caller a mapping that
         * silently disagrees, which surfaces far away as a null pointer read
         * out of what should have been file content.  Reading less than the
         * file holds is only legitimate past end of file. */
        {
            const uint64_t available_bytes =
                whole_file_bytes > offset ? whole_file_bytes - offset : 0;
            const uint64_t expected_bytes =
                available_bytes < len ? available_bytes : len;
            if (done < expected_bytes) {
                lpr_trace_mmap_error(
                    "pread_to_vmo_short",
                    addr,
                    len,
                    prot,
                    flags,
                    fd,
                    offset,
                    (int64_t)done);
            }
        }
        lpr_trace_mmap_load(len, done, prot, flags, fd, offset);
        profile_stage = lpr_startup_profile_stage_begin();
        mapped = lpr_pacha_syscall6(
            PACHAOS_SYSCALL_MMAP,
            (uint64_t)(uint32_t)vmo_fd,
            addr,
            map_len,
            initial_prot,
            direct_map_flags,
            0);
        lpr_startup_profile_stage_end(
            LPR_STARTUP_MMAP_STAGE_NATIVE_MMAP, profile_stage);
        mapped_prot = initial_prot;
        if (mapped < 4096) {
            lpr_trace_mmap_error("direct_vmo_mmap", addr, len, prot, flags, fd, offset, mapped);
            profile_stage = lpr_startup_profile_stage_begin();
            mapped = lpr_pacha_syscall6(
                PACHAOS_SYSCALL_MMAP,
                0,
                addr,
                map_len,
                load_prot,
                (pacha_flags | PACHAOS_MMAP_ANONYMOUS) & ~PACHAOS_MMAP_SHARED,
                0);
            lpr_startup_profile_stage_end(
                LPR_STARTUP_MMAP_STAGE_NATIVE_MMAP, profile_stage);
            mapped_prot = load_prot;
            if (mapped < 4096) {
                lpr_trace_mmap_error("target_mmap", addr, len, prot, flags, fd, offset, mapped);
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
                return lpr_linux_pacha_status_to_errno(mapped);
            }
            profile_stage = lpr_startup_profile_stage_begin();
            const int64_t source = lpr_pacha_syscall6(
                PACHAOS_SYSCALL_MMAP,
                (uint64_t)(uint32_t)vmo_fd,
                0,
                map_len,
                PACHAOS_PROT_READ,
                PACHAOS_MMAP_SHARED,
                0);
            lpr_startup_profile_stage_end(
                LPR_STARTUP_MMAP_STAGE_NATIVE_MMAP, profile_stage);
            if (source < 4096) {
                lpr_trace_mmap_error("source_mmap", addr, len, prot, flags, fd, offset, source);
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
                (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)mapped, map_len);
                return lpr_linux_pacha_status_to_errno(source);
            }
            profile_stage = lpr_startup_profile_stage_begin();
            if (done != 0) {
                lpr_memcpy((void *)(uintptr_t)mapped, (const void *)(uintptr_t)source, (size_t)done);
            }
            lpr_startup_profile_stage_end(
                LPR_STARTUP_MMAP_STAGE_FALLBACK_COPY, profile_stage);
            (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)source, map_len);
        }
        if (vmo_fd >= 16) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
        }
private_file_mapping_ready:
        if ((prot & LPR_LINUX_PROT_EXEC) != 0 && done != 0) {
            struct lpr_patch_mapping_result patch_result;
            const struct lpr_patch_mapping_request patch_request = {
                .start_va = (uint64_t)mapped,
                .size_bytes = done,
                .flags = LPR_PATCH_FLAG_EXECUTABLE | LPR_PATCH_FLAG_PRIVATE,
            };
            profile_stage = lpr_startup_profile_stage_begin();
            const int64_t patch_status = lpr_patch_mapping(&patch_request, &patch_result);
            lpr_startup_profile_stage_end(
                LPR_STARTUP_MMAP_STAGE_PATCH, profile_stage);
            lpr_startup_profile_patch(fd, &patch_result);
            lpr_trace_patch_mapping(&patch_result);
            if (patch_status != PERSONALITY_STATUS_OK) {
                (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)mapped, map_len);
                return -LPR_LINUX_EINVAL;
            }
        }
        if (final_prot != mapped_prot) {
            profile_stage = lpr_startup_profile_stage_begin();
            const int64_t protect_status = lpr_pacha_syscall3(
                PACHAOS_SYSCALL_MPROTECT,
                (uint64_t)mapped,
                map_len,
                final_prot);
            lpr_startup_profile_stage_end(
                LPR_STARTUP_MMAP_STAGE_MPROTECT, profile_stage);
            if (protect_status != 0) {
                (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)mapped, map_len);
                return lpr_linux_pacha_status_to_errno(protect_status);
            }
        }
        const int64_t mmap_result = mapped;
        lpr_mmap_image_diag(
            private_file,
            len,
            map_len,
            prot,
            offset,
            mmap_result);
        lpr_trace_mmap_call("mmap", addr, len, prot, flags, fd, offset, mmap_result);
        return mmap_result;
    }
    const uint64_t pacha_fd = (flags & LPR_LINUX_MAP_ANONYMOUS) != 0 ? 0 : fd;
    const int64_t ret = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        pacha_fd,
        addr,
        len,
        lpr_linux_prot_to_pacha(prot),
        pacha_flags,
        offset);
    if (ret >= 4096 && (flags & LPR_LINUX_MAP_ANONYMOUS) != 0) {
        lpr_mmap_exec_diag("mmap-anonymous", (uint64_t)ret, len, prot);
    }
    if (ret < 4096) {
        lpr_trace_mmap_error(
            (flags & LPR_LINUX_MAP_ANONYMOUS) != 0 ?
                "anonymous_mmap" : "native_mmap",
            addr,
            len,
            prot,
            flags,
            fd,
            offset,
            ret);
    }
    const int64_t result = lpr_linux_pacha_status_to_errno(ret);
    lpr_trace_mmap_call("mmap", addr, len, prot, flags, fd, offset, result);
    return result;
}

int64_t lpr_linux_mmap(
    uint64_t addr,
    uint64_t len,
    uint64_t prot,
    uint64_t flags,
    uint64_t fd,
    uint64_t offset)
{
    if ((flags & LPR_LINUX_MAP_ANONYMOUS) != 0) {
        return lpr_backend_mmap(addr, len, prot, flags, fd, offset);
    }
    return lpr_fd_dispatch_mmap(addr, len, prot, flags, fd, offset);
}

static int64_t lpr_dispatch_msync(uint64_t addr, uint64_t len, uint64_t flags)
{
    const uint64_t known_flags =
        LPR_LINUX_MS_ASYNC |
        LPR_LINUX_MS_INVALIDATE |
        LPR_LINUX_MS_SYNC;
    if ((addr & 4095u) != 0 ||
        (flags & ~known_flags) != 0 ||
        ((flags & LPR_LINUX_MS_ASYNC) != 0 && (flags & LPR_LINUX_MS_SYNC) != 0))
    {
        return -LPR_LINUX_EINVAL;
    }
    (void)len;
    return lpr_linux_sync();
}

static uint32_t lpr_linux_online_cpu_count(void)
{
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0), "c"(0));
    const uint32_t max_leaf = eax;
    uint32_t count = 0;
    if (max_leaf >= 0xbu) {
        for (uint32_t level = 0; level < 8; level++) {
            __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0xbu), "c"(level));
            if (ebx == 0) break;
            if (((ecx >> 8u) & 0xffu) == 2u) count = ebx & 0xffffu;
        }
    }
    if (count == 0 && max_leaf >= 1u) {
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
        count = (ebx >> 16u) & 0xffu;
    }
    if (count == 0) count = 1;
    if (count > 4u) count = 4u;
    return count;
}

static int64_t lpr_dispatch_sched_getaffinity(uint64_t tid, uint64_t size, uint64_t mask_raw)
{
    const uint64_t kernel_mask_bytes = sizeof(uint64_t);
    if (mask_raw == 0) return -LPR_LINUX_EFAULT;
    if (size < kernel_mask_bytes) return -LPR_LINUX_EINVAL;
    if (!lpr_linux_thread_exists(tid)) return -LPR_LINUX_ESRCH;
    const uint32_t cpu_count = lpr_linux_online_cpu_count();
    *(uint64_t *)(uintptr_t)mask_raw = (1ull << cpu_count) - 1ull;
    return (int64_t)kernel_mask_bytes;
}

typedef int64_t (*lpr_syscall_handler_t)(
    uint64_t a0,
    uint64_t a1,
    uint64_t a2,
    uint64_t a3,
    uint64_t a4,
    uint64_t a5);

typedef struct lpr_syscall_entry {
    uint64_t nr;
    char name[24];
    enum lpr_linux_syscall_class cls;
    enum lpr_linux_syscall_backend backend;
    lpr_syscall_handler_t handler;
    uint8_t trace_socket;
} lpr_syscall_entry_t;

static int64_t lpr_sys_read(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_read(a0, a1, a2); }
static int64_t lpr_sys_write(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    if (a0 == 2u && lpr_glycin_diag_contains(
            (const void *)(uintptr_t)a1,
            a2,
            "Glycin running without sandbox"))
    {
        lpr_glycin_diag_arm("warning");
    }
#endif
    return lpr_linux_write(a0, a1, a2);
}
static int64_t lpr_sys_open(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_openat(LPR_LINUX_AT_FDCWD, a0, a1, a2); }
static int64_t lpr_sys_close(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_close(a0); }
static int64_t lpr_sys_close_range(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_close_range(a0, a1, a2); }
static int64_t lpr_sys_sched_getaffinity(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_dispatch_sched_getaffinity(a0, a1, a2); }
static int64_t lpr_sys_faccessat2(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_faccessat(a0, a1, a2, a3); }
static int64_t lpr_sys_stat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_newfstatat(LPR_LINUX_AT_FDCWD, a0, a1, 0); }
static int64_t lpr_sys_lstat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_newfstatat(LPR_LINUX_AT_FDCWD, a0, a1, 0x100); }
static int64_t lpr_sys_lseek(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_lseek(a0, a1, a2); }
static int64_t lpr_sys_mmap(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { return lpr_linux_mmap(a0, a1, a2, a3, a4, a5); }
static int64_t lpr_sys_mremap(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_linux_mremap(a0, a1, a2, a3, a4); }
static int64_t lpr_sys_mprotect(
    uint64_t a0,
    uint64_t a1,
    uint64_t a2,
    uint64_t a3,
    uint64_t a4,
    uint64_t a5)
{
    (void)a3;
    (void)a4;
    (void)a5;
    const int64_t status = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_MPROTECT,
        a0,
        a1,
        lpr_linux_prot_to_pacha(a2));
    if (status != 0) {
        lpr_trace_mmap_error(
            "mprotect",
            a0,
            a1,
            a2,
            0,
            0,
            0,
            status);
    } else {
        lpr_mmap_exec_diag("mprotect", a0, a1, a2);
    }
    return lpr_linux_pacha_status_to_errno(status);
}
static int64_t lpr_sys_munmap(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    const struct lpr_linux_user_frame *frame = lpr_current_linux_user_frame();
    if (frame != 0 && a1 != 0 && a0 <= UINT64_MAX - a1 &&
        frame->rsp >= a0 && frame->rsp < a0 + a1 &&
        frame->rip < a0 && frame->rip <= UINT64_MAX - 8u)
    {
        /* Patched Linux musl x86_64 __unmapself sequence immediately after
         * SYS_munmap: xor %rdi,%rdi; mov $SYS_exit,%eax. */
        const unsigned char *next = (const unsigned char *)(uintptr_t)frame->rip;
        static const unsigned char unmapself_tail[8] = {
            0x48, 0x31, 0xff, 0xb8, 0x3c, 0x00, 0x00, 0x00,
        };
        if (lpr_memcmp(next, unmapself_tail, sizeof(unmapself_tail)) == 0) {
            lpr_trace_mmap_call("unmapself", a0, a1, 0, 0, 0, 0, 0);
            lpr_linux_unmapself_exit(a0, a1);
        }
    }
    const int64_t result = lpr_linux_pacha_status_to_errno(
        lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, a0, a1));
    lpr_trace_mmap_call("munmap", a0, a1, 0, 0, 0, 0, result);
    return result;
}
static int64_t lpr_sys_msync(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_dispatch_msync(a0, a1, a2); }
static int64_t lpr_sys_brk(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_brk(a0); }
static int64_t lpr_sys_rt_sigaction(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_rt_sigaction(a0, a1, a2, a3); }
static int64_t lpr_sys_rt_sigprocmask(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_rt_sigprocmask(a0, a1, a2, a3); }
static int64_t lpr_sys_rt_sigpending(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_rt_sigpending(a0, a1); }
static int64_t lpr_sys_rt_sigtimedwait(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_rt_sigtimedwait(a0, a1, a2, a3); }
static int64_t lpr_sys_rt_sigreturn(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; lpr_linux_rt_sigreturn_frame(lpr_current_linux_user_frame()); }
static int64_t lpr_sys_sigaltstack(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_sigaltstack(a0, a1); }
static int64_t lpr_sys_ioctl(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_ioctl(a0, a1, a2); }
static int64_t lpr_sys_pread64(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_pread64(a0, a1, a2, a3); }
static int64_t lpr_sys_readv(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_readv(a0, a1, a2); }
static int64_t lpr_sys_writev(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    const lpr_linux_iovec_t *iov =
        (const lpr_linux_iovec_t *)(uintptr_t)a1;
    if (a0 == 2u && iov != 0) {
        for (uint64_t i = 0; i < a2; ++i) {
            if (lpr_glycin_diag_contains(
                    (const void *)(uintptr_t)iov[i].base,
                    iov[i].len,
                    "Glycin running without sandbox"))
            {
                lpr_glycin_diag_arm("warning.writev");
                break;
            }
        }
    }
#endif
    return lpr_linux_writev(a0, a1, a2);
}
static int64_t lpr_sys_access(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_access(a0, a1); }
static int64_t lpr_sys_pipe(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_pipe2(a0, 0); }
static int64_t lpr_sys_select(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_linux_select(a0, a1, a2, a3, a4); }
static int64_t lpr_sys_dup(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_dup(a0, 0, 0); }
static int64_t lpr_sys_dup2(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_dup2(a0, a1, 0); }
static int64_t lpr_sys_nanosleep(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_nanosleep(a0, a1); }
static int64_t lpr_sys_getpid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_getpid(); }
static int64_t lpr_sys_socket(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_socket(a0, a1, a2); }
static int64_t lpr_sys_socketpair(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_socketpair(a0, a1, a2, a3); }
static int64_t lpr_epoll_note_result(uint64_t fd, int64_t result) { lpr_epoll_note_fd_state(fd); return result; }
static int64_t lpr_sys_connect(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_epoll_note_result(a0, lpr_linux_connect(a0, a1, a2)); }
static int64_t lpr_sys_sendto(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { return lpr_epoll_note_result(a0, lpr_linux_sendto(a0, a1, a2, a3, a4, a5)); }
static int64_t lpr_sys_recvfrom(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { return lpr_epoll_note_result(a0, lpr_linux_recvfrom(a0, a1, a2, a3, a4, a5)); }
static int64_t lpr_sys_sendmsg(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_epoll_note_result(a0, lpr_linux_sendmsg(a0, a1, a2)); }
static int64_t lpr_sys_recvmsg(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_epoll_note_result(a0, lpr_linux_recvmsg(a0, a1, a2)); }
static int64_t lpr_sys_shutdown(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_shutdown(a0, a1); }
static int64_t lpr_sys_bind(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_bind(a0, a1, a2); }
static int64_t lpr_sys_getsockname(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_getsockname(a0, a1, a2); }
static int64_t lpr_sys_getpeername(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_getpeername(a0, a1, a2); }
static int64_t lpr_sys_setsockopt(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_linux_setsockopt(a0, a1, a2, a3, a4); }
static int64_t lpr_sys_getsockopt(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_linux_getsockopt(a0, a1, a2, a3, a4); }
static int64_t lpr_sys_clone(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_linux_clone(a0, a1, a2, a3, a4); }
static int64_t lpr_sys_fork(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_fork(); }
static int64_t lpr_sys_vfork(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_vfork(); }
static int64_t lpr_sys_execve(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_execve(a0, a1, a2); }
static int64_t lpr_sys_exit(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; lpr_linux_exit_thread(a0); }
static int64_t lpr_sys_exit_group(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; lpr_linux_exit_group(a0); }
static int64_t lpr_sys_wait4(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_wait4(a0, a1, a2, a3); }
static int64_t lpr_sys_kill(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_kill(a0, a1); }
static int64_t lpr_sys_tkill(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_tkill(a0, a1); }
static int64_t lpr_sys_tgkill(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_tgkill(a0, a1, a2); }
static int64_t lpr_sys_uname(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_uname(a0); }
static int64_t lpr_sys_fstat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_fstat(a0, a1); }
static int64_t lpr_sys_statfs(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_statfs(a0, a1); }
static int64_t lpr_sys_fstatfs(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_fstatfs(a0, a1); }
static int64_t lpr_sys_fsync(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_fsync(a0); }
static int64_t lpr_sys_ftruncate(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_ftruncate(a0, a1); }
static int64_t lpr_sys_fallocate(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return -LPR_LINUX_EOPNOTSUPP; }
static int64_t lpr_sys_sync(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_sync(); }
static int64_t lpr_sys_syncfs(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_syncfs(a0); }
static int64_t lpr_sys_getcwd(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_getcwd(a0, a1); }
static int64_t lpr_sys_chdir(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_chdir(a0); }
static int64_t lpr_sys_fchdir(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_fchdir(a0); }
static int64_t lpr_sys_rename(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_renameat(LPR_LINUX_AT_FDCWD, a0, LPR_LINUX_AT_FDCWD, a1); }
static int64_t lpr_sys_mkdir(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_mkdirat(LPR_LINUX_AT_FDCWD, a0, a1); }
static int64_t lpr_sys_rmdir(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_unlinkat(LPR_LINUX_AT_FDCWD, a0, LPR_LINUX_AT_REMOVEDIR); }
static int64_t lpr_sys_link(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_linkat(LPR_LINUX_AT_FDCWD, a0, LPR_LINUX_AT_FDCWD, a1, 0); }
static int64_t lpr_sys_unlink(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_unlinkat(LPR_LINUX_AT_FDCWD, a0, 0); }
static int64_t lpr_sys_symlink(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_symlinkat(a0, LPR_LINUX_AT_FDCWD, a1); }
static int64_t lpr_sys_readlink(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_readlink(a0, a1, a2); }
static int64_t lpr_sys_chmod(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_fchmodat(LPR_LINUX_AT_FDCWD, a0, a1, 0); }
static int64_t lpr_sys_fchmod(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_fchmod(a0, a1); }
static int64_t lpr_sys_chown(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_fchownat(LPR_LINUX_AT_FDCWD, a0, a1, a2, 0); }
static int64_t lpr_sys_fchown(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_fchownat(a0, (uint64_t)(uintptr_t)"", a1, a2, LPR_LINUX_AT_EMPTY_PATH); }
static int64_t lpr_sys_lchown(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_fchownat(LPR_LINUX_AT_FDCWD, a0, a1, a2, LPR_LINUX_AT_SYMLINK_NOFOLLOW); }
static int64_t lpr_sys_umask(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; const uint64_t previous = lpr_linux_umask_value; lpr_linux_umask_value = a0 & 0777ull; return (int64_t)previous; }
static int64_t lpr_sys_getrlimit(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_copy_out_rlimit(a0, a1); }
static int64_t lpr_sys_zero(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
static int64_t lpr_sys_setid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return a0 == 0 ? 0 : -LPR_LINUX_EPERM; }
static int64_t lpr_sys_setresid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    const uint32_t id0 = (uint32_t)a0;
    const uint32_t id1 = (uint32_t)a1;
    const uint32_t id2 = (uint32_t)a2;
    return (id0 == 0 || id0 == UINT32_MAX) &&
        (id1 == 0 || id1 == UINT32_MAX) &&
        (id2 == 0 || id2 == UINT32_MAX) ? 0 : -LPR_LINUX_EPERM;
}
static int64_t lpr_sys_getresid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_getresid(a0, a1, a2); }
static int64_t lpr_sys_capget(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_capget(a0, a1, lpr_linux_getpid()); }
static int64_t lpr_sys_capset(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_capset(a0, a1, lpr_linux_getpid()); }
static int64_t lpr_sys_getppid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_getppid(); }
static int64_t lpr_sys_getpgrp(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_getpgrp(); }
static int64_t lpr_sys_setpgid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_setpgid(a0, a1); }
static int64_t lpr_sys_getpgid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_getpgid(a0); }
static int64_t lpr_sys_setsid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_setsid(); }
static int64_t lpr_sys_getsid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_getsid(a0); }
static int64_t lpr_sys_setrlimit(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_set_rlimit(a0, a1); }
static int64_t lpr_sys_prctl(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_linux_prctl(a0, a1, a2, a3, a4); }
static int64_t lpr_sys_arch_prctl(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_dispatch_arch_prctl(a0, a1); }
static int64_t lpr_sys_unshare(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_unshare(a0); }
static int64_t lpr_sys_gettid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_gettid(); }
static int64_t lpr_sys_set_tid_address(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_set_tid_address(a0); }

static int64_t lpr_futex_timeout_ticks(
    uint64_t timeout_raw,
    int absolute,
    uint64_t clock_id,
    uint64_t *out_ticks)
{
    *out_ticks = 0;
    if (timeout_raw == 0) return 0;

    const struct pachaos_timespec *timeout =
        (const struct pachaos_timespec *)(uintptr_t)timeout_raw;
    const int64_t valid = lpr_linux_validate_timespec(timeout);
    if (valid != 0) return valid;

    uint64_t seconds = timeout->tv_sec;
    uint64_t nanoseconds = timeout->tv_nsec;
    if (absolute) {
        struct pachaos_timespec now;
        const int64_t clock_status = lpr_pacha_clock_gettime(clock_id, &now);
        if (clock_status != 0) return clock_status;
        if (seconds < now.tv_sec ||
            (seconds == now.tv_sec && nanoseconds <= now.tv_nsec))
        {
            return -LPR_LINUX_ETIMEDOUT;
        }
        seconds -= now.tv_sec;
        if (nanoseconds < now.tv_nsec) {
            seconds--;
            nanoseconds += 1000000000ull - now.tv_nsec;
        } else {
            nanoseconds -= now.tv_nsec;
        }
    }

    if (seconds > UINT64_MAX / 1000ull) return -LPR_LINUX_EINVAL;
    const uint64_t fractional_ticks =
        (nanoseconds + 999999ull) / 1000000ull;
    const uint64_t whole_ticks = seconds * 1000ull;
    if (whole_ticks > UINT64_MAX - fractional_ticks)
        return -LPR_LINUX_EINVAL;
    *out_ticks = whole_ticks + fractional_ticks;
    return *out_ticks == 0 ? -LPR_LINUX_ETIMEDOUT : 0;
}

static int64_t lpr_sys_futex(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    const uint64_t command = a1 & 0x7full;
    const uint64_t flags = a1 & ~0x7full;
    const int wait_bitset = command == LPR_LINUX_FUTEX_WAIT_BITSET;
    const uint64_t allowed_flags = LPR_LINUX_FUTEX_PRIVATE_FLAG |
        (wait_bitset ? LPR_LINUX_FUTEX_CLOCK_REALTIME : 0ull);
    if ((flags & ~allowed_flags) != 0 ||
        (command != LPR_LINUX_FUTEX_WAIT &&
         command != LPR_LINUX_FUTEX_WAKE &&
         command != LPR_LINUX_FUTEX_REQUEUE && !wait_bitset)) {
        return -LPR_LINUX_ENOSYS;
    }
    if (a0 == 0 || (a0 & 3u) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (command == LPR_LINUX_FUTEX_WAKE) {
        /* FUTEX_WAKE returns the number of waiters woken.  Native futex wake
         * has the same non-negative result, and this path already validates
         * every argument for which the native call can report an error.  Do
         * not feed a positive wake count through the status-code mapper: the
         * small values overlap native error numbers. */
        return lpr_pacha_syscall2(
            PACHAOS_SYSCALL_FUTEX_WAKE,
            a0,
            a2);
    }
    if (command == LPR_LINUX_FUTEX_REQUEUE) {
        if (a4 == 0 || (a4 & 3u) != 0 || a4 == a0) {
            return -LPR_LINUX_EINVAL;
        }
        /* The native result is the number of waiters woken plus requeued.
         * As with FUTEX_WAKE, return it directly because small successful
         * counts overlap native status values. */
        return lpr_pacha_syscall4(
            PACHAOS_SYSCALL_FUTEX_REQUEUE,
            a0,
            a2,
            a3,
            a4);
    }
    if (wait_bitset && (uint32_t)a5 == 0u) return -LPR_LINUX_EINVAL;

    volatile uint32_t *word = (volatile uint32_t *)(uintptr_t)a0;
    const uint32_t expected = (uint32_t)a2;
    if (__atomic_load_n(word, __ATOMIC_ACQUIRE) != expected) {
        return -LPR_LINUX_EAGAIN;
    }
    uint64_t timeout_ticks = 0;
    const int64_t timeout_status = lpr_futex_timeout_ticks(
        a3,
        wait_bitset,
        (flags & LPR_LINUX_FUTEX_CLOCK_REALTIME) != 0 ?
            LPR_LINUX_CLOCK_REALTIME : LPR_LINUX_CLOCK_MONOTONIC,
        &timeout_ticks);
    if (timeout_status != 0) return timeout_status;
    const int64_t status = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_FUTEX_WAIT,
        a0,
        expected,
        timeout_ticks);
    if (status == 0) {
        return 0;
    }
    if (status == PACHA_FUTEX_WAIT_TIMED_OUT_STATUS && a3 != 0) {
        return -LPR_LINUX_ETIMEDOUT;
    }
    const int64_t error = lpr_linux_pacha_status_to_errno(status);
    return error;
}
static int64_t lpr_sys_getdents64(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_getdents64(a0, a1, a2); }

typedef struct lpr_linux_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
} lpr_linux_timeval_t;

typedef struct lpr_linux_itimerval {
    lpr_linux_timeval_t interval;
    lpr_linux_timeval_t value;
} lpr_linux_itimerval_t;

typedef struct lpr_native_signal_timer_state {
    uint64_t signo;
    uint64_t remaining_ticks;
    uint64_t interval_ticks;
} lpr_native_signal_timer_state_t;

_Static_assert(
    offsetof(lpr_native_signal_timer_state_t, signo) ==
        PACHA_PROCESS_SIGNAL_TIMER_STATE_SIGNO_OFFSET,
    "native signal timer signo offset");
_Static_assert(
    offsetof(lpr_native_signal_timer_state_t, remaining_ticks) ==
        PACHA_PROCESS_SIGNAL_TIMER_STATE_REMAINING_TICKS_OFFSET,
    "native signal timer remaining offset");
_Static_assert(
    offsetof(lpr_native_signal_timer_state_t, interval_ticks) ==
        PACHA_PROCESS_SIGNAL_TIMER_STATE_INTERVAL_TICKS_OFFSET,
    "native signal timer interval offset");
_Static_assert(
    sizeof(lpr_native_signal_timer_state_t) ==
        PACHA_PROCESS_SIGNAL_TIMER_STATE_SIZE,
    "native signal timer state size");

static int64_t lpr_itimer_timeval_to_ticks(
    const lpr_linux_timeval_t *value,
    uint64_t *out_ticks)
{
    if (value == 0 || out_ticks == 0) return -LPR_LINUX_EFAULT;
    if (value->tv_sec < 0 || value->tv_usec < 0 ||
        value->tv_usec >= 1000000)
    {
        return -LPR_LINUX_EINVAL;
    }
    const uint64_t seconds = (uint64_t)value->tv_sec;
    const uint64_t fractional_ticks =
        ((uint64_t)value->tv_usec + 999u) / 1000u;
    if (seconds > (UINT64_MAX - fractional_ticks) / 1000u)
        return -LPR_LINUX_EINVAL;
    *out_ticks = seconds * 1000u + fractional_ticks;
    return 0;
}

static void lpr_itimer_ticks_to_timeval(
    uint64_t ticks,
    lpr_linux_timeval_t *out)
{
    out->tv_sec = (int64_t)(ticks / 1000u);
    out->tv_usec = (int64_t)((ticks % 1000u) * 1000u);
}

static void lpr_itimer_native_to_linux(
    const lpr_native_signal_timer_state_t *native,
    lpr_linux_itimerval_t *out)
{
    lpr_memset(out, 0, sizeof(*out));
    lpr_itimer_ticks_to_timeval(native->remaining_ticks, &out->value);
    lpr_itimer_ticks_to_timeval(native->interval_ticks, &out->interval);
}

static int64_t lpr_sys_getitimer(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    if (a0 != 0) return -LPR_LINUX_EINVAL;
    if (a1 == 0) return -LPR_LINUX_EFAULT;
    lpr_native_signal_timer_state_t native = {0};
    const int64_t status = lpr_linux_pacha_status_to_errno(
        lpr_pacha_syscall2(
            PACHAOS_SYSCALL_PROCESS_SIGNAL_CTL,
            PACHA_PROCESS_SIGNAL_CTL_GET_TIMER,
            (uint64_t)(uintptr_t)&native));
    if (status != 0) return status;
    lpr_itimer_native_to_linux(
        &native,
        (lpr_linux_itimerval_t *)(uintptr_t)a1);
    return 0;
}

static int64_t lpr_sys_setitimer(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3;
    (void)a4;
    (void)a5;
    if (a0 != 0) return -LPR_LINUX_EINVAL;

    const lpr_linux_itimerval_t zero = {0};
    const lpr_linux_itimerval_t *value = a1 != 0 ?
        (const lpr_linux_itimerval_t *)(uintptr_t)a1 : &zero;
    uint64_t initial_ticks = 0;
    uint64_t interval_ticks = 0;
    int64_t status = lpr_itimer_timeval_to_ticks(
        &value->value, &initial_ticks);
    if (status != 0) return status;
    status = lpr_itimer_timeval_to_ticks(
        &value->interval, &interval_ticks);
    if (status != 0) return status;

    lpr_native_signal_timer_state_t old = {0};
    status = lpr_linux_pacha_status_to_errno(lpr_pacha_syscall5(
        PACHAOS_SYSCALL_PROCESS_SIGNAL_CTL,
        PACHA_PROCESS_SIGNAL_CTL_SET_TIMER,
        LPR_LINUX_SIGALRM,
        initial_ticks,
        interval_ticks,
        (uint64_t)(uintptr_t)&old));
    if (status != 0) return status;
    if (a2 != 0) {
        lpr_itimer_native_to_linux(
            &old,
            (lpr_linux_itimerval_t *)(uintptr_t)a2);
    }
    return 0;
}

static int64_t lpr_sys_rt_sigsuspend(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    if (a0 == 0) return -LPR_LINUX_EFAULT;
    return lpr_linux_ppoll(0, 0, 0, a0, a1);
}

static int64_t lpr_sys_clock_gettime(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    uint64_t native_clock = 0;
    switch (a0) {
    case LPR_LINUX_CLOCK_REALTIME:
    case LPR_LINUX_CLOCK_REALTIME_COARSE:
        native_clock = LPR_LINUX_CLOCK_REALTIME;
        break;
    case LPR_LINUX_CLOCK_MONOTONIC:
    case LPR_LINUX_CLOCK_MONOTONIC_RAW:
    case LPR_LINUX_CLOCK_MONOTONIC_COARSE:
    case LPR_LINUX_CLOCK_BOOTTIME:
        native_clock = LPR_LINUX_CLOCK_MONOTONIC;
        break;
    default:
        return -LPR_LINUX_EINVAL;
    }
    int64_t native_status;
    do {
        native_status = lpr_pacha_syscall2(
            PACHAOS_SYSCALL_CLOCK_GETTIME, native_clock, a1);
        if (native_status == PACHA_SYSCALL_ERR_NOT_READY ||
            native_status == -PACHA_SYSCALL_ERR_NOT_READY)
        {
            __asm__ volatile("pause");
        }
    } while (native_status == PACHA_SYSCALL_ERR_NOT_READY ||
             native_status == -PACHA_SYSCALL_ERR_NOT_READY);
    return lpr_linux_pacha_status_to_errno(native_status);
}
static int64_t lpr_sys_clock_getres(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    uint64_t native_clock = 0;
    switch (a0) {
    case LPR_LINUX_CLOCK_REALTIME:
    case LPR_LINUX_CLOCK_REALTIME_COARSE:
        native_clock = LPR_LINUX_CLOCK_REALTIME;
        break;
    case LPR_LINUX_CLOCK_MONOTONIC:
    case LPR_LINUX_CLOCK_MONOTONIC_RAW:
    case LPR_LINUX_CLOCK_MONOTONIC_COARSE:
    case LPR_LINUX_CLOCK_BOOTTIME:
        native_clock = LPR_LINUX_CLOCK_MONOTONIC;
        break;
    default:
        return -LPR_LINUX_EINVAL;
    }
    struct pachaos_timespec ignored;
    const uint64_t out = a1 != 0 ? a1 : (uint64_t)(uintptr_t)&ignored;
    return lpr_linux_pacha_status_to_errno(lpr_pacha_syscall2(
        PACHAOS_SYSCALL_CLOCK_GETRES,
        native_clock,
        out));
}
static int64_t lpr_sys_fadvise64(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4;
    (void)a5;
    if (a3 > 5u || (int64_t)a1 < 0 || (int64_t)a2 < 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (a0 > LPR_LINUX_FD_MAX || !lpr_fd_linux_visible_active(a0)) {
        return -LPR_LINUX_EBADF;
    }
    if (!lpr_linux_filed_fd_active(a0)) {
        return -LPR_LINUX_ESPIPE;
    }
    /* POSIX_FADV_* is advisory.  filed has already populated the shared file
     * image/cache for regular files, so accepting the hint is the complete
     * observable Linux behavior; it must not fail merely because no extra
     * readahead action is necessary. */
    return 0;
}
static int64_t lpr_sys_clock_nanosleep(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_clock_nanosleep(a0, a1, a2, a3); }
static int64_t lpr_sys_openat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_openat(a0, a1, a2, a3); }
static int64_t lpr_sys_mkdirat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_mkdirat(a0, a1, a2); }
static int64_t lpr_sys_mknodat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_mknodat(a0, a1, a2, a3); }
static int64_t lpr_sys_fchownat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_linux_fchownat(a0, a1, a2, a3, a4); }
static int64_t lpr_sys_newfstatat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_newfstatat(a0, a1, a2, a3); }
static int64_t lpr_sys_statx(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_linux_statx(a0, a1, a2, a3, a4); }
static int64_t lpr_sys_unlinkat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_unlinkat(a0, a1, a2); }
static int64_t lpr_sys_renameat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_renameat(a0, a1, a2, a3); }
static int64_t lpr_sys_linkat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_linux_linkat(a0, a1, a2, a3, a4); }
static int64_t lpr_sys_symlinkat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_symlinkat(a0, a1, a2); }
static int64_t lpr_sys_fchmodat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_fchmodat(a0, a1, a2, 0); }
static int64_t lpr_sys_faccessat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_faccessat(a0, a1, a2, 0); }
static int64_t lpr_sys_pselect6(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { return lpr_linux_pselect6(a0, a1, a2, a3, a4, a5); }
static int64_t lpr_sys_ppoll(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_linux_ppoll(a0, a1, a2, a3, a4); }
static int64_t lpr_sys_epoll_wait(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_epoll_wait(a0, a1, a2, a3); }
static int64_t lpr_sys_epoll_ctl(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_epoll_ctl(a0, a1, a2, a3); }
static int64_t lpr_sys_epoll_pwait(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { return lpr_linux_epoll_pwait(a0, a1, a2, a3, a4, a5); }
static int64_t lpr_sys_epoll_create1(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_epoll_create1(a0); }
static int64_t lpr_sys_utimensat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_utimensat(a0, a1, a2, a3); }
static int64_t lpr_sys_eventfd(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_eventfd2(a0, 0); }
static int64_t lpr_sys_eventfd2(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_eventfd2(a0, a1); }
static int64_t lpr_sys_inotify_init(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_inotify_init1(0); }
static int64_t lpr_sys_inotify_init1(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_inotify_init1(a0); }
static int64_t lpr_sys_inotify_add_watch(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_inotify_add_watch(a0, a1, a2); }
static int64_t lpr_sys_inotify_rm_watch(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_inotify_rm_watch(a0, a1); }
static int64_t lpr_sys_timerfd_create(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_timerfd_create(a0, a1); }
static int64_t lpr_sys_timerfd_settime(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_timerfd_settime(a0, a1, a2, a3); }
static int64_t lpr_sys_timerfd_gettime(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_timerfd_gettime(a0, a1); }
static int64_t lpr_sys_signalfd4(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_signalfd4(a0, a1, a2, a3); }
static int64_t lpr_sys_accept(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_epoll_note_result(a0, lpr_linux_accept(a0, a1, a2, 0)); }
static int64_t lpr_sys_listen(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_listen(a0, a1); }
static int64_t lpr_sys_dup3(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_dup2(a0, a1, a2); }
static int64_t lpr_sys_pipe2(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_pipe2(a0, a1); }
static int64_t lpr_sys_recvmmsg(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_epoll_note_result(a0, lpr_linux_recvmmsg(a0, a1, a2, a3, a4)); }
static int64_t lpr_sys_prlimit64(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_prlimit64(a0, a1, a2, a3); }
static int64_t lpr_sys_sendmmsg(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_epoll_note_result(a0, lpr_linux_sendmmsg(a0, a1, a2, a3)); }
static int64_t lpr_sys_getrandom(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_pacha_syscall3(PACHAOS_SYSCALL_GETRANDOM, a0, a1, a2); }
static int64_t lpr_sys_memfd_create(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_memfd_create(a0, a1); }
static int64_t lpr_sys_membarrier(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    if (a1 != 0) {
        return -LPR_LINUX_EINVAL;
    }
    switch (a0) {
    case LPR_LINUX_MEMBARRIER_CMD_QUERY:
        return LPR_LINUX_MEMBARRIER_PRIVATE_EXPEDITED_MASK;
    case LPR_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
        __atomic_store_n(&lpr_linux_private_expedited_registered, 1u, __ATOMIC_RELEASE);
        return 0;
    case LPR_LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED:
        if (__atomic_load_n(&lpr_linux_private_expedited_registered, __ATOMIC_ACQUIRE) == 0u) {
            return -LPR_LINUX_EPERM;
        }
        return lpr_linux_pacha_status_to_errno(lpr_pacha_syscall1(
            PACHAOS_SYSCALL_PROCESS_MEMORY_BARRIER,
            PACHAOS_PROCESS_MEMORY_BARRIER_FLAG_NONE));
    default:
        return -LPR_LINUX_EINVAL;
    }
}
static int64_t lpr_sys_fcntl(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_socket_fd_active(a0) ? lpr_linux_socket_fcntl(a0, a1, a2) : lpr_linux_fcntl(a0, a1, a2); }
static int64_t lpr_sys_flock(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_flock(a0, a1); }
static int64_t lpr_sys_poll(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_poll(a0, a1, a2); }

#define LPR_SYSCALL_TRACE 1u
#define LPR_SYSCALL(nr_value, name_value, class_value, backend_value, handler_value, trace_value) \
    [nr_value] = { nr_value, name_value, class_value, backend_value, 0, trace_value }

static lpr_syscall_entry_t lpr_syscall_table[LPR_LINUX_SYS_LAST + 1u] = {
    LPR_SYSCALL(LPR_LINUX_SYS_READ, "read", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_read, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_WRITE, "write", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_write, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_OPEN, "open", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_open, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_CLOSE, "close", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_close, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_STAT, "stat", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_stat, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FSTAT, "fstat", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_fstat, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_LSTAT, "lstat", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_lstat, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_POLL, "poll", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_poll, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_LSEEK, "lseek", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_lseek, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_MMAP, "mmap", LPR_LINUX_SYSCALL_CLASS_MEMORY, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_mmap, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_MPROTECT, "mprotect", LPR_LINUX_SYSCALL_CLASS_MEMORY, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_mprotect, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_MUNMAP, "munmap", LPR_LINUX_SYSCALL_CLASS_MEMORY, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_munmap, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_MREMAP, "mremap", LPR_LINUX_SYSCALL_CLASS_MEMORY, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_mremap, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_MSYNC, "msync", LPR_LINUX_SYSCALL_CLASS_MEMORY, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_msync, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_BRK, "brk", LPR_LINUX_SYSCALL_CLASS_MEMORY, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_brk, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_RT_SIGACTION, "rt_sigaction", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_rt_sigaction, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_RT_SIGPROCMASK, "rt_sigprocmask", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_rt_sigprocmask, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_RT_SIGRETURN, "rt_sigreturn", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_rt_sigreturn, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_IOCTL, "ioctl", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_ioctl, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_PREAD64, "pread64", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_pread64, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_READV, "readv", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_readv, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_WRITEV, "writev", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_writev, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_ACCESS, "access", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_access, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_PIPE, "pipe", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_pipe, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SELECT, "select", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_select, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_DUP, "dup", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_dup, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_DUP2, "dup2", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_dup2, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_NANOSLEEP, "nanosleep", LPR_LINUX_SYSCALL_CLASS_TIME_RANDOM, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_nanosleep, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETITIMER, "getitimer", LPR_LINUX_SYSCALL_CLASS_TIME_RANDOM, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_getitimer, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SETITIMER, "setitimer", LPR_LINUX_SYSCALL_CLASS_TIME_RANDOM, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_setitimer, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETPID, "getpid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_getpid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SOCKET, "socket", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_socket, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_CONNECT, "connect", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_connect, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_ACCEPT, "accept", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_accept, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_SENDTO, "sendto", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_sendto, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_RECVFROM, "recvfrom", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_recvfrom, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_SENDMSG, "sendmsg", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_sendmsg, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_RECVMSG, "recvmsg", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_recvmsg, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_SHUTDOWN, "shutdown", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_shutdown, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_BIND, "bind", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_bind, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_LISTEN, "listen", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_listen, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_GETSOCKNAME, "getsockname", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_getsockname, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_GETPEERNAME, "getpeername", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_getpeername, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_SOCKETPAIR, "socketpair", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_socketpair, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_SETSOCKOPT, "setsockopt", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_setsockopt, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_GETSOCKOPT, "getsockopt", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_getsockopt, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_CLONE, "clone", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_clone, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FORK, "fork", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_fork, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_VFORK, "vfork", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_vfork, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_EXECVE, "execve", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_execve, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_EXIT, "exit", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_exit, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_WAIT4, "wait4", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_wait4, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_KILL, "kill", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_kill, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_TKILL, "tkill", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_tkill, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_TGKILL, "tgkill", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_tgkill, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_UNAME, "uname", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_uname, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FCNTL, "fcntl", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_fcntl, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_FLOCK, "flock", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_flock, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FSYNC, "fsync", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_fsync, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FDATASYNC, "fdatasync", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_fsync, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FTRUNCATE, "ftruncate", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_ftruncate, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FALLOCATE, "fallocate", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_fallocate, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETCWD, "getcwd", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_getcwd, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_CHDIR, "chdir", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_chdir, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FCHDIR, "fchdir", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_fchdir, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_RENAME, "rename", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_rename, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_MKDIR, "mkdir", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_mkdir, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_RMDIR, "rmdir", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_rmdir, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_LINK, "link", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_link, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_UNLINK, "unlink", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_unlink, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SYMLINK, "symlink", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_symlink, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_READLINK, "readlink", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_readlink, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_CHMOD, "chmod", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_chmod, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FCHMOD, "fchmod", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_fchmod, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_CHOWN, "chown", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_chown, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FCHOWN, "fchown", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_fchown, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_LCHOWN, "lchown", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_lchown, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_UMASK, "umask", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_umask, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETRLIMIT, "getrlimit", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_getrlimit, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETUID, "getuid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_zero, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETGID, "getgid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_zero, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SETUID, "setuid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_setid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SETGID, "setgid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_setid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETEUID, "geteuid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_zero, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETEGID, "getegid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_zero, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SETPGID, "setpgid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_setpgid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETPPID, "getppid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_getppid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETPGRP, "getpgrp", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_getpgrp, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SETSID, "setsid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_setsid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SETRESUID, "setresuid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_setresid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETRESUID, "getresuid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_getresid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SETRESGID, "setresgid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_setresid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETRESGID, "getresgid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_getresid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETPGID, "getpgid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_getpgid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETSID, "getsid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_getsid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_CAPGET, "capget", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_capget, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_CAPSET, "capset", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_capset, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_RT_SIGPENDING, "rt_sigpending", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_rt_sigpending, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_RT_SIGTIMEDWAIT, "rt_sigtimedwait", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_rt_sigtimedwait, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_RT_SIGSUSPEND, "rt_sigsuspend", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_rt_sigsuspend, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SIGALTSTACK, "sigaltstack", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_sigaltstack, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_STATFS, "statfs", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_statfs, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FSTATFS, "fstatfs", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_fstatfs, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SETPRIORITY, "setpriority", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_zero, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SETRLIMIT, "setrlimit", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_setrlimit, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_PRCTL, "prctl", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_prctl, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SYNC, "sync", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_sync, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_ARCH_PRCTL, "arch_prctl", LPR_LINUX_SYSCALL_CLASS_THREAD_ARCH, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_arch_prctl, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETTID, "gettid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_gettid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FUTEX, "futex", LPR_LINUX_SYSCALL_CLASS_THREAD_ARCH, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_futex, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETDENTS64, "getdents64", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_getdents64, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_SET_TID_ADDRESS, "set_tid_address", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_set_tid_address, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FADVISE64, "fadvise64", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_fadvise64, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_CLOCK_GETTIME, "clock_gettime", LPR_LINUX_SYSCALL_CLASS_TIME_RANDOM, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_clock_gettime, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_CLOCK_GETRES, "clock_getres", LPR_LINUX_SYSCALL_CLASS_TIME_RANDOM, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_clock_getres, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_CLOCK_NANOSLEEP, "clock_nanosleep", LPR_LINUX_SYSCALL_CLASS_TIME_RANDOM, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_clock_nanosleep, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_EXIT_GROUP, "exit_group", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_exit_group, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_OPENAT, "openat", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_openat, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_MKDIRAT, "mkdirat", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_mkdirat, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_MKNODAT, "mknodat", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_mknodat, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FCHOWNAT, "fchownat", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_fchownat, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_NEWFSTATAT, "newfstatat", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_newfstatat, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_UNLINKAT, "unlinkat", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_unlinkat, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_RENAMEAT, "renameat", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_renameat, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_LINKAT, "linkat", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_linkat, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SYMLINKAT, "symlinkat", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_symlinkat, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FCHMODAT, "fchmodat", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_fchmodat, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FACCESSAT, "faccessat", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_faccessat, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_PSELECT6, "pselect6", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_pselect6, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_PPOLL, "ppoll", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_ppoll, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_EPOLL_WAIT, "epoll_wait", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_epoll_wait, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_EPOLL_CTL, "epoll_ctl", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_epoll_ctl, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_UNSHARE, "unshare", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_unshare, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_UTIMENSAT, "utimensat", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_utimensat, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_EPOLL_PWAIT, "epoll_pwait", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_epoll_pwait, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_INOTIFY_INIT, "inotify_init", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_inotify_init, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_INOTIFY_ADD_WATCH, "inotify_add_watch", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_inotify_add_watch, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_INOTIFY_RM_WATCH, "inotify_rm_watch", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_inotify_rm_watch, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_TIMERFD_CREATE, "timerfd_create", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_timerfd_create, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_EVENTFD, "eventfd", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_eventfd, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_TIMERFD_SETTIME, "timerfd_settime", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_timerfd_settime, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_TIMERFD_GETTIME, "timerfd_gettime", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_timerfd_gettime, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_SIGNALFD4, "signalfd4", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_signalfd4, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_EVENTFD2, "eventfd2", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_eventfd2, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_EPOLL_CREATE1, "epoll_create1", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_epoll_create1, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_DUP3, "dup3", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_dup3, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_PIPE2, "pipe2", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_pipe2, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_INOTIFY_INIT1, "inotify_init1", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_inotify_init1, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_RECVMMSG, "recvmmsg", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_recvmmsg, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_PRLIMIT64, "prlimit64", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_prlimit64, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SYNCFS, "syncfs", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_syncfs, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SENDMMSG, "sendmmsg", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_sendmmsg, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_GETRANDOM, "getrandom", LPR_LINUX_SYSCALL_CLASS_TIME_RANDOM, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_getrandom, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_MEMFD_CREATE, "memfd_create", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_memfd_create, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_MEMBARRIER, "membarrier", LPR_LINUX_SYSCALL_CLASS_MEMORY, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_membarrier, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_STATX, "statx", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_statx, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_CLOSE_RANGE, "close_range", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_close_range, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_SCHED_GETAFFINITY, "sched_getaffinity", LPR_LINUX_SYSCALL_CLASS_THREAD_ARCH, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_sched_getaffinity, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FACCESSAT2, "faccessat2", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_faccessat2, 0),
};

static int lpr_syscall_table_initialized;

static void lpr_syscall_table_init(void)
{
    if (lpr_syscall_table_initialized) {
        return;
    }
    (void)lpr_load_manifest();
    lpr_syscall_table[LPR_LINUX_SYS_READ].handler = lpr_sys_read;
    lpr_syscall_table[LPR_LINUX_SYS_WRITE].handler = lpr_sys_write;
    lpr_syscall_table[LPR_LINUX_SYS_OPEN].handler = lpr_sys_open;
    lpr_syscall_table[LPR_LINUX_SYS_CLOSE].handler = lpr_sys_close;
    lpr_syscall_table[LPR_LINUX_SYS_STAT].handler = lpr_sys_stat;
    lpr_syscall_table[LPR_LINUX_SYS_FSTAT].handler = lpr_sys_fstat;
    lpr_syscall_table[LPR_LINUX_SYS_LSTAT].handler = lpr_sys_lstat;
    lpr_syscall_table[LPR_LINUX_SYS_POLL].handler = lpr_sys_poll;
    lpr_syscall_table[LPR_LINUX_SYS_LSEEK].handler = lpr_sys_lseek;
    lpr_syscall_table[LPR_LINUX_SYS_MMAP].handler = lpr_sys_mmap;
    lpr_syscall_table[LPR_LINUX_SYS_MPROTECT].handler = lpr_sys_mprotect;
    lpr_syscall_table[LPR_LINUX_SYS_MUNMAP].handler = lpr_sys_munmap;
    lpr_syscall_table[LPR_LINUX_SYS_MREMAP].handler = lpr_sys_mremap;
    lpr_syscall_table[LPR_LINUX_SYS_MSYNC].handler = lpr_sys_msync;
    lpr_syscall_table[LPR_LINUX_SYS_BRK].handler = lpr_sys_brk;
    lpr_syscall_table[LPR_LINUX_SYS_RT_SIGACTION].handler = lpr_sys_rt_sigaction;
    lpr_syscall_table[LPR_LINUX_SYS_RT_SIGPROCMASK].handler = lpr_sys_rt_sigprocmask;
    lpr_syscall_table[LPR_LINUX_SYS_RT_SIGPENDING].handler = lpr_sys_rt_sigpending;
    lpr_syscall_table[LPR_LINUX_SYS_RT_SIGTIMEDWAIT].handler = lpr_sys_rt_sigtimedwait;
    lpr_syscall_table[LPR_LINUX_SYS_RT_SIGRETURN].handler = lpr_sys_rt_sigreturn;
    lpr_syscall_table[LPR_LINUX_SYS_IOCTL].handler = lpr_sys_ioctl;
    lpr_syscall_table[LPR_LINUX_SYS_PREAD64].handler = lpr_sys_pread64;
    lpr_syscall_table[LPR_LINUX_SYS_READV].handler = lpr_sys_readv;
    lpr_syscall_table[LPR_LINUX_SYS_WRITEV].handler = lpr_sys_writev;
    lpr_syscall_table[LPR_LINUX_SYS_ACCESS].handler = lpr_sys_access;
    lpr_syscall_table[LPR_LINUX_SYS_PIPE].handler = lpr_sys_pipe;
    lpr_syscall_table[LPR_LINUX_SYS_SELECT].handler = lpr_sys_select;
    lpr_syscall_table[LPR_LINUX_SYS_DUP].handler = lpr_sys_dup;
    lpr_syscall_table[LPR_LINUX_SYS_DUP2].handler = lpr_sys_dup2;
    lpr_syscall_table[LPR_LINUX_SYS_NANOSLEEP].handler = lpr_sys_nanosleep;
    lpr_syscall_table[LPR_LINUX_SYS_GETITIMER].handler = lpr_sys_getitimer;
    lpr_syscall_table[LPR_LINUX_SYS_SETITIMER].handler = lpr_sys_setitimer;
    lpr_syscall_table[LPR_LINUX_SYS_GETPID].handler = lpr_sys_getpid;
    lpr_syscall_table[LPR_LINUX_SYS_SOCKET].handler = lpr_sys_socket;
    lpr_syscall_table[LPR_LINUX_SYS_CONNECT].handler = lpr_sys_connect;
    lpr_syscall_table[LPR_LINUX_SYS_ACCEPT].handler = lpr_sys_accept;
    lpr_syscall_table[LPR_LINUX_SYS_SENDTO].handler = lpr_sys_sendto;
    lpr_syscall_table[LPR_LINUX_SYS_RECVFROM].handler = lpr_sys_recvfrom;
    lpr_syscall_table[LPR_LINUX_SYS_SENDMSG].handler = lpr_sys_sendmsg;
    lpr_syscall_table[LPR_LINUX_SYS_RECVMSG].handler = lpr_sys_recvmsg;
    lpr_syscall_table[LPR_LINUX_SYS_SHUTDOWN].handler = lpr_sys_shutdown;
    lpr_syscall_table[LPR_LINUX_SYS_BIND].handler = lpr_sys_bind;
    lpr_syscall_table[LPR_LINUX_SYS_LISTEN].handler = lpr_sys_listen;
    lpr_syscall_table[LPR_LINUX_SYS_GETSOCKNAME].handler = lpr_sys_getsockname;
    lpr_syscall_table[LPR_LINUX_SYS_GETPEERNAME].handler = lpr_sys_getpeername;
    lpr_syscall_table[LPR_LINUX_SYS_SOCKETPAIR].handler = lpr_sys_socketpair;
    lpr_syscall_table[LPR_LINUX_SYS_SETSOCKOPT].handler = lpr_sys_setsockopt;
    lpr_syscall_table[LPR_LINUX_SYS_GETSOCKOPT].handler = lpr_sys_getsockopt;
    lpr_syscall_table[LPR_LINUX_SYS_CLONE].handler = lpr_sys_clone;
    lpr_syscall_table[LPR_LINUX_SYS_FORK].handler = lpr_sys_fork;
    lpr_syscall_table[LPR_LINUX_SYS_VFORK].handler = lpr_sys_vfork;
    lpr_syscall_table[LPR_LINUX_SYS_EXECVE].handler = lpr_sys_execve;
    lpr_syscall_table[LPR_LINUX_SYS_EXIT].handler = lpr_sys_exit;
    lpr_syscall_table[LPR_LINUX_SYS_WAIT4].handler = lpr_sys_wait4;
    lpr_syscall_table[LPR_LINUX_SYS_KILL].handler = lpr_sys_kill;
    lpr_syscall_table[LPR_LINUX_SYS_TKILL].handler = lpr_sys_tkill;
    lpr_syscall_table[LPR_LINUX_SYS_TGKILL].handler = lpr_sys_tgkill;
    lpr_syscall_table[LPR_LINUX_SYS_UNAME].handler = lpr_sys_uname;
    lpr_syscall_table[LPR_LINUX_SYS_FCNTL].handler = lpr_sys_fcntl;
    lpr_syscall_table[LPR_LINUX_SYS_FLOCK].handler = lpr_sys_flock;
    lpr_syscall_table[LPR_LINUX_SYS_FSYNC].handler = lpr_sys_fsync;
    lpr_syscall_table[LPR_LINUX_SYS_FDATASYNC].handler = lpr_sys_fsync;
    lpr_syscall_table[LPR_LINUX_SYS_FTRUNCATE].handler = lpr_sys_ftruncate;
    lpr_syscall_table[LPR_LINUX_SYS_FALLOCATE].handler = lpr_sys_fallocate;
    lpr_syscall_table[LPR_LINUX_SYS_GETCWD].handler = lpr_sys_getcwd;
    lpr_syscall_table[LPR_LINUX_SYS_CHDIR].handler = lpr_sys_chdir;
    lpr_syscall_table[LPR_LINUX_SYS_FCHDIR].handler = lpr_sys_fchdir;
    lpr_syscall_table[LPR_LINUX_SYS_RENAME].handler = lpr_sys_rename;
    lpr_syscall_table[LPR_LINUX_SYS_MKDIR].handler = lpr_sys_mkdir;
    lpr_syscall_table[LPR_LINUX_SYS_RMDIR].handler = lpr_sys_rmdir;
    lpr_syscall_table[LPR_LINUX_SYS_LINK].handler = lpr_sys_link;
    lpr_syscall_table[LPR_LINUX_SYS_UNLINK].handler = lpr_sys_unlink;
    lpr_syscall_table[LPR_LINUX_SYS_SYMLINK].handler = lpr_sys_symlink;
    lpr_syscall_table[LPR_LINUX_SYS_READLINK].handler = lpr_sys_readlink;
    lpr_syscall_table[LPR_LINUX_SYS_CHMOD].handler = lpr_sys_chmod;
    lpr_syscall_table[LPR_LINUX_SYS_FCHMOD].handler = lpr_sys_fchmod;
    lpr_syscall_table[LPR_LINUX_SYS_CHOWN].handler = lpr_sys_chown;
    lpr_syscall_table[LPR_LINUX_SYS_FCHOWN].handler = lpr_sys_fchown;
    lpr_syscall_table[LPR_LINUX_SYS_LCHOWN].handler = lpr_sys_lchown;
    lpr_syscall_table[LPR_LINUX_SYS_UMASK].handler = lpr_sys_umask;
    lpr_syscall_table[LPR_LINUX_SYS_GETRLIMIT].handler = lpr_sys_getrlimit;
    lpr_syscall_table[LPR_LINUX_SYS_GETUID].handler = lpr_sys_zero;
    lpr_syscall_table[LPR_LINUX_SYS_GETGID].handler = lpr_sys_zero;
    lpr_syscall_table[LPR_LINUX_SYS_SETUID].handler = lpr_sys_setid;
    lpr_syscall_table[LPR_LINUX_SYS_SETGID].handler = lpr_sys_setid;
    lpr_syscall_table[LPR_LINUX_SYS_GETEUID].handler = lpr_sys_zero;
    lpr_syscall_table[LPR_LINUX_SYS_GETEGID].handler = lpr_sys_zero;
    lpr_syscall_table[LPR_LINUX_SYS_SETPGID].handler = lpr_sys_setpgid;
    lpr_syscall_table[LPR_LINUX_SYS_GETPPID].handler = lpr_sys_getppid;
    lpr_syscall_table[LPR_LINUX_SYS_GETPGRP].handler = lpr_sys_getpgrp;
    lpr_syscall_table[LPR_LINUX_SYS_SETSID].handler = lpr_sys_setsid;
    lpr_syscall_table[LPR_LINUX_SYS_SETRESUID].handler = lpr_sys_setresid;
    lpr_syscall_table[LPR_LINUX_SYS_GETRESUID].handler = lpr_sys_getresid;
    lpr_syscall_table[LPR_LINUX_SYS_SETRESGID].handler = lpr_sys_setresid;
    lpr_syscall_table[LPR_LINUX_SYS_GETRESGID].handler = lpr_sys_getresid;
    lpr_syscall_table[LPR_LINUX_SYS_GETPGID].handler = lpr_sys_getpgid;
    lpr_syscall_table[LPR_LINUX_SYS_GETSID].handler = lpr_sys_getsid;
    lpr_syscall_table[LPR_LINUX_SYS_CAPGET].handler = lpr_sys_capget;
    lpr_syscall_table[LPR_LINUX_SYS_CAPSET].handler = lpr_sys_capset;
    lpr_syscall_table[LPR_LINUX_SYS_RT_SIGSUSPEND].handler = lpr_sys_rt_sigsuspend;
    lpr_syscall_table[LPR_LINUX_SYS_SIGALTSTACK].handler = lpr_sys_sigaltstack;
    lpr_syscall_table[LPR_LINUX_SYS_STATFS].handler = lpr_sys_statfs;
    lpr_syscall_table[LPR_LINUX_SYS_FSTATFS].handler = lpr_sys_fstatfs;
    lpr_syscall_table[LPR_LINUX_SYS_SETPRIORITY].handler = lpr_sys_zero;
    lpr_syscall_table[LPR_LINUX_SYS_SETRLIMIT].handler = lpr_sys_setrlimit;
    lpr_syscall_table[LPR_LINUX_SYS_PRCTL].handler = lpr_sys_prctl;
    lpr_syscall_table[LPR_LINUX_SYS_SYNC].handler = lpr_sys_sync;
    lpr_syscall_table[LPR_LINUX_SYS_ARCH_PRCTL].handler = lpr_sys_arch_prctl;
    lpr_syscall_table[LPR_LINUX_SYS_GETTID].handler = lpr_sys_gettid;
    lpr_syscall_table[LPR_LINUX_SYS_FUTEX].handler = lpr_sys_futex;
    lpr_syscall_table[LPR_LINUX_SYS_GETDENTS64].handler = lpr_sys_getdents64;
    lpr_syscall_table[LPR_LINUX_SYS_SET_TID_ADDRESS].handler = lpr_sys_set_tid_address;
    lpr_syscall_table[LPR_LINUX_SYS_FADVISE64].handler = lpr_sys_fadvise64;
    lpr_syscall_table[LPR_LINUX_SYS_CLOCK_GETTIME].handler = lpr_sys_clock_gettime;
    lpr_syscall_table[LPR_LINUX_SYS_CLOCK_GETRES].handler = lpr_sys_clock_getres;
    lpr_syscall_table[LPR_LINUX_SYS_CLOCK_NANOSLEEP].handler = lpr_sys_clock_nanosleep;
    lpr_syscall_table[LPR_LINUX_SYS_EXIT_GROUP].handler = lpr_sys_exit_group;
    lpr_syscall_table[LPR_LINUX_SYS_OPENAT].handler = lpr_sys_openat;
    lpr_syscall_table[LPR_LINUX_SYS_MKDIRAT].handler = lpr_sys_mkdirat;
    lpr_syscall_table[LPR_LINUX_SYS_MKNODAT].handler = lpr_sys_mknodat;
    lpr_syscall_table[LPR_LINUX_SYS_FCHOWNAT].handler = lpr_sys_fchownat;
    lpr_syscall_table[LPR_LINUX_SYS_NEWFSTATAT].handler = lpr_sys_newfstatat;
    lpr_syscall_table[LPR_LINUX_SYS_UNLINKAT].handler = lpr_sys_unlinkat;
    lpr_syscall_table[LPR_LINUX_SYS_RENAMEAT].handler = lpr_sys_renameat;
    lpr_syscall_table[LPR_LINUX_SYS_LINKAT].handler = lpr_sys_linkat;
    lpr_syscall_table[LPR_LINUX_SYS_SYMLINKAT].handler = lpr_sys_symlinkat;
    lpr_syscall_table[LPR_LINUX_SYS_FCHMODAT].handler = lpr_sys_fchmodat;
    lpr_syscall_table[LPR_LINUX_SYS_FACCESSAT].handler = lpr_sys_faccessat;
    lpr_syscall_table[LPR_LINUX_SYS_PSELECT6].handler = lpr_sys_pselect6;
    lpr_syscall_table[LPR_LINUX_SYS_PPOLL].handler = lpr_sys_ppoll;
    lpr_syscall_table[LPR_LINUX_SYS_EPOLL_WAIT].handler = lpr_sys_epoll_wait;
    lpr_syscall_table[LPR_LINUX_SYS_EPOLL_CTL].handler = lpr_sys_epoll_ctl;
    lpr_syscall_table[LPR_LINUX_SYS_UNSHARE].handler = lpr_sys_unshare;
    lpr_syscall_table[LPR_LINUX_SYS_UTIMENSAT].handler = lpr_sys_utimensat;
    lpr_syscall_table[LPR_LINUX_SYS_EPOLL_PWAIT].handler = lpr_sys_epoll_pwait;
    lpr_syscall_table[LPR_LINUX_SYS_INOTIFY_INIT].handler = lpr_sys_inotify_init;
    lpr_syscall_table[LPR_LINUX_SYS_INOTIFY_ADD_WATCH].handler = lpr_sys_inotify_add_watch;
    lpr_syscall_table[LPR_LINUX_SYS_INOTIFY_RM_WATCH].handler = lpr_sys_inotify_rm_watch;
    lpr_syscall_table[LPR_LINUX_SYS_TIMERFD_CREATE].handler = lpr_sys_timerfd_create;
    lpr_syscall_table[LPR_LINUX_SYS_EVENTFD].handler = lpr_sys_eventfd;
    lpr_syscall_table[LPR_LINUX_SYS_TIMERFD_SETTIME].handler = lpr_sys_timerfd_settime;
    lpr_syscall_table[LPR_LINUX_SYS_TIMERFD_GETTIME].handler = lpr_sys_timerfd_gettime;
    lpr_syscall_table[LPR_LINUX_SYS_SIGNALFD4].handler = lpr_sys_signalfd4;
    lpr_syscall_table[LPR_LINUX_SYS_EVENTFD2].handler = lpr_sys_eventfd2;
    lpr_syscall_table[LPR_LINUX_SYS_EPOLL_CREATE1].handler = lpr_sys_epoll_create1;
    lpr_syscall_table[LPR_LINUX_SYS_DUP3].handler = lpr_sys_dup3;
    lpr_syscall_table[LPR_LINUX_SYS_PIPE2].handler = lpr_sys_pipe2;
    lpr_syscall_table[LPR_LINUX_SYS_INOTIFY_INIT1].handler = lpr_sys_inotify_init1;
    lpr_syscall_table[LPR_LINUX_SYS_RECVMMSG].handler = lpr_sys_recvmmsg;
    lpr_syscall_table[LPR_LINUX_SYS_PRLIMIT64].handler = lpr_sys_prlimit64;
    lpr_syscall_table[LPR_LINUX_SYS_SYNCFS].handler = lpr_sys_syncfs;
    lpr_syscall_table[LPR_LINUX_SYS_SENDMMSG].handler = lpr_sys_sendmmsg;
    lpr_syscall_table[LPR_LINUX_SYS_GETRANDOM].handler = lpr_sys_getrandom;
    lpr_syscall_table[LPR_LINUX_SYS_MEMFD_CREATE].handler = lpr_sys_memfd_create;
    lpr_syscall_table[LPR_LINUX_SYS_MEMBARRIER].handler = lpr_sys_membarrier;
    lpr_syscall_table[LPR_LINUX_SYS_STATX].handler = lpr_sys_statx;
    lpr_syscall_table[LPR_LINUX_SYS_CLOSE_RANGE].handler = lpr_sys_close_range;
    lpr_syscall_table[LPR_LINUX_SYS_SCHED_GETAFFINITY].handler = lpr_sys_sched_getaffinity;
    lpr_syscall_table[LPR_LINUX_SYS_FACCESSAT2].handler = lpr_sys_faccessat2;
    lpr_syscall_table_initialized = 1;
}

static const lpr_syscall_entry_t *lpr_syscall_lookup_entry(uint64_t nr)
{
    lpr_syscall_table_init();
    if (nr >= sizeof(lpr_syscall_table) / sizeof(lpr_syscall_table[0]) ||
        lpr_syscall_table[nr].handler == 0)
    {
        return 0;
    }
    return &lpr_syscall_table[nr];
}

const struct lpr_linux_syscall_info *lpr_linux_syscall_lookup(uint64_t nr)
{
    const lpr_syscall_entry_t *entry = lpr_syscall_lookup_entry(nr);
    static struct lpr_linux_syscall_info info_cache[LPR_LINUX_SYS_LAST + 1u];
    if (entry == 0) {
        return 0;
    }
    struct lpr_linux_syscall_info *info = &info_cache[nr];
    if (info->name == 0) {
        info->nr = entry->nr;
        info->name = entry->name;
        info->cls = entry->cls;
        info->backend = entry->backend;
    }
    return info;
}

static int64_t lpr_dispatch_syscall_inner(const lpr_syscall_entry_t *entry,
                                          const struct lpr_linux_user_frame *frame,
                                          uint64_t a0,
                                          uint64_t a1,
                                          uint64_t a2,
                                          uint64_t a3,
                                          uint64_t a4,
                                          uint64_t a5)
{
    if (entry == 0) {
        return -LPR_LINUX_ENOSYS;
    }
    if (entry->nr == LPR_LINUX_SYS_CLONE) {
        return lpr_linux_clone_frame(frame, a0, a1, a2, a3, a4);
    }
    if (entry->nr == LPR_LINUX_SYS_FORK) {
        return lpr_linux_clone_frame(frame, 17u, 0, 0, 0, 0);
    }
    if (entry->nr == LPR_LINUX_SYS_VFORK) {
        return lpr_linux_clone_frame(frame, 0x4000ull | 0x100ull | 17u, 0, 0, 0, 0);
    }
    return entry->handler(a0, a1, a2, a3, a4, a5);
}

int64_t lpr_dispatch_syscall(uint64_t nr,
                             uint64_t a0,
                             uint64_t a1,
                             uint64_t a2,
                             uint64_t a3,
                             uint64_t a4,
                             uint64_t a5) {
    return lpr_dispatch_syscall_frame(0, nr, a0, a1, a2, a3, a4, a5);
}

int64_t lpr_dispatch_syscall_frame(struct lpr_linux_user_frame *frame,
                                   uint64_t nr,
                                   uint64_t a0,
                                   uint64_t a1,
                                   uint64_t a2,
                                   uint64_t a3,
                                   uint64_t a4,
                                   uint64_t a5) {
    /* The dynamic linker enters us for ARCH_SET_FS before musl has a usable
     * thread pointer. Registering native signal delivery first creates a
     * race: a pending signal can enter the LPR trampoline on the registration
     * syscall's return and touch TLS while FS is still zero. Establish TLS
     * first, then make signal delivery visible to the kernel. */
    if (nr == LPR_LINUX_SYS_ARCH_PRCTL && a0 == LPR_LINUX_ARCH_SET_FS) {
        const lpr_syscall_entry_t *entry = lpr_syscall_lookup_entry(nr);
        const int64_t result = lpr_dispatch_syscall_inner(
            entry, frame, a0, a1, a2, a3, a4, a5);
        if (result == 0) {
            lpr_linux_signal_runtime_init();
        }
        return result;
    }
    lpr_linux_signal_runtime_init();
#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
    lpr_syscall_profile_enable();
#endif
#if defined(LPR_STARTUP_PROFILE) && LPR_STARTUP_PROFILE
    lpr_startup_profile_enable();
#endif
    if (frame != 0 && frame->rip < LPR_LOW_GUARD_END_VA) {
        pacha_trace3(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_EVENT_LPR_BAD_RETURN, PACHA_TRACE_CLASS_ERROR, nr, frame->rip, frame->rcx);
    }
    const lpr_syscall_entry_t *entry = lpr_syscall_lookup_entry(nr);
    const int trace_socket_syscall = entry != 0 && entry->trace_socket;
    if (trace_socket_syscall) {
        lpr_trace_socket_syscall_event("enter", nr, a0, a1, a2, 0);
    }
    lpr_linux_ensure_default_stdio();
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    const int glycin_diag_owner =
        __atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u &&
        lpr_glycin_diag_take_owner_slot();
    const int glycin_diag_follow = lpr_glycin_diag_take_follow_slot();
    const int glycin_diag_syscall =
        __atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u &&
        (glycin_diag_owner || glycin_diag_follow ||
         lpr_glycin_diag_syscall_selected(nr, a1, a4));
    if (glycin_diag_syscall) {
        lpr_glycin_diag_event(
            glycin_diag_owner ? "owner.enter" :
                (glycin_diag_follow ? "follow.enter" : "sys.enter"),
            nr, a0, a1, (int64_t)a2);
        lpr_glycin_diag_event(
            glycin_diag_owner ? "owner.args" :
                (glycin_diag_follow ? "follow.args" : "sys.args"),
            a3, a4, a5, 0);
    }
#endif
    /* A queued signal must not suppress the syscall. Blocking waits surface
     * interruptions through LPR_WAIT_RESTART_SYSCALL below. */
    const int trace_metrics = pacha_trace_enabled(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_CLASS_METRIC);
    if (trace_metrics && (nr == LPR_LINUX_SYS_EXIT || nr == LPR_LINUX_SYS_EXIT_GROUP)) {
        lpr_trace_syscall_record(nr, 0, 0);
        lpr_linux_readv_cache_trace_dump();
        lpr_trace_syscall_dump(nr);
#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
        lpr_netd_profile_dump();
        pacha_trace_dump_ring();
#endif
#if defined(LPR_STARTUP_PROFILE) && LPR_STARTUP_PROFILE
        if (nr == LPR_LINUX_SYS_EXIT_GROUP) {
            lpr_startup_profile_dump();
            lpr_drm_startup_profile_dump();
        }
#endif
        if (nr == LPR_LINUX_SYS_EXIT_GROUP) {
            lpr_linux_exit_group(a0);
        } else {
            lpr_linux_exit_thread(a0);
        }
    }
    uint64_t cycles = 0;
    int64_t result = 0;
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    const int dbus_wait_diag =
        __atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u &&
        lpr_dbus_wait_diag_syscall(nr);
    struct pachaos_timespec dbus_wait_started;
    lpr_memset(&dbus_wait_started, 0, sizeof(dbus_wait_started));
    if (dbus_wait_diag)
        (void)lpr_pacha_clock_gettime(
            LPR_LINUX_CLOCK_MONOTONIC, &dbus_wait_started);
#if defined(LPR_WAIT_ENTRY_DIAG) && LPR_WAIT_ENTRY_DIAG
    const int wait_entry_diag = lpr_wait_entry_diag_syscall(nr, a1);
    if (wait_entry_diag)
        lpr_wait_entry_diag_log("enter", nr, a0, a1, a2, a3, a4, 0);
#endif
#endif
    for (;;) {
        const uint64_t start_cycles = trace_metrics ? pacha_trace_read_tsc() : 0;
        result = lpr_dispatch_syscall_inner(entry, frame, a0, a1, a2, a3, a4, a5);
        const uint64_t end_cycles = trace_metrics ? pacha_trace_read_tsc() : 0;
        if (end_cycles >= start_cycles) cycles += end_cycles - start_cycles;
        if (result != LPR_WAIT_RESTART_SYSCALL) break;

        // The blocking layer has released every stack-scoped resource.  Only
        // now may signal delivery abandon the LPR dispatch stack.
        const int64_t signal_status =
            lpr_linux_dispatch_pending_signals_with_result(-LPR_LINUX_EINTR);
        if (signal_status != 0) {
            result = signal_status;
            break;
        }
        lpr_linux_deliver_native_pending_frame(-LPR_LINUX_EINTR);
        // A spurious wake or an ignored synthetic signal leaves no native
        // frame to deliver, so restart the syscall transparently.
    }
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
#if defined(LPR_WAIT_ENTRY_DIAG) && LPR_WAIT_ENTRY_DIAG
    if (wait_entry_diag)
        lpr_wait_entry_diag_log("exit", nr, a0, a1, a2, a3, a4, result);
#endif
    if (dbus_wait_diag) {
        struct pachaos_timespec dbus_wait_finished;
        lpr_memset(&dbus_wait_finished, 0, sizeof(dbus_wait_finished));
        if (lpr_pacha_clock_gettime(
                LPR_LINUX_CLOCK_MONOTONIC, &dbus_wait_finished) == 0 &&
            lpr_timespec_less_equal(
                &dbus_wait_started, &dbus_wait_finished))
        {
            struct pachaos_timespec elapsed;
            lpr_timespec_subtract(
                &dbus_wait_finished, &dbus_wait_started, &elapsed);
            const uint64_t elapsed_ns =
                elapsed.tv_sec <= UINT64_MAX / 1000000000u ?
                elapsed.tv_sec * 1000000000u + elapsed.tv_nsec :
                UINT64_MAX;
            lpr_dbus_wait_diag_log(
                nr, a0, a1, a2, a3, result, elapsed_ns);
        }
    }
#endif
    if (trace_metrics) {
        lpr_trace_syscall_record(nr, cycles, result);
    }
#if defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE
    lpr_syscall_profile_maybe_dump(nr);
#endif
#if defined(LPR_STARTUP_PROFILE) && LPR_STARTUP_PROFILE
    lpr_startup_profile_record(
        nr, a0, a1, a2, a3, a4, a5, result, cycles);
#endif
    if (cycles >= 10000000ull) {
        lpr_trace_slow_syscall(nr, a0, a1, a2, a3, a4, a5, result, cycles);
    }
    if (trace_socket_syscall) {
        lpr_trace_socket_syscall_event("exit", nr, a0, a1, a2, result);
    }
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    if (glycin_diag_syscall) {
        lpr_glycin_diag_event(
            glycin_diag_owner ? "owner.exit" :
                (glycin_diag_follow ? "follow.exit" : "sys.exit"),
            nr, a0, a1, result);
    }
#endif
    /* The syscall has completed.  A failure while translating a subsequently
     * pending Linux signal into a native signal must leave that signal queued;
     * it is not the result of the completed Linux syscall. */
    (void)lpr_linux_dispatch_pending_signals_with_result(result);
    if (result == -LPR_LINUX_ENOSYS) {
        lpr_trace_enosys_syscall(nr, a0, a1, a2, a3, a4, a5);
    }
    lpr_linux_deliver_native_pending_frame(result);
    return result;
}

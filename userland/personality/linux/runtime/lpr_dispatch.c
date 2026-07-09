#include "lpr_linux_syscall.h"
#include "lpr_filed.h"
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
#define LPR_LINUX_AT_FDCWD ((uint64_t)(int64_t)-100)
#define LPR_LINUX_AT_SYMLINK_NOFOLLOW 0x100ull
#define LPR_LINUX_AT_REMOVEDIR 0x200ull
#define LPR_LINUX_AT_EMPTY_PATH 0x1000ull

static uint64_t lpr_linux_umask_value;

typedef struct lpr_linux_rlimit {
    uint64_t cur;
    uint64_t max;
} lpr_linux_rlimit_t;

enum {
    LPR_LINUX_RLIMIT_CPU = 0,
    LPR_LINUX_RLIMIT_FSIZE = 1,
    LPR_LINUX_RLIMIT_DATA = 2,
    LPR_LINUX_RLIMIT_STACK = 3,
    LPR_LINUX_RLIMIT_CORE = 4,
    LPR_LINUX_RLIMIT_RSS = 5,
    LPR_LINUX_RLIMIT_NPROC = 6,
    LPR_LINUX_RLIMIT_NOFILE = 7,
    LPR_LINUX_RLIMIT_MEMLOCK = 8,
    LPR_LINUX_RLIMIT_AS = 9,
    LPR_LINUX_RLIMIT_LOCKS = 10,
    LPR_LINUX_RLIMIT_SIGPENDING = 11,
    LPR_LINUX_RLIMIT_MSGQUEUE = 12,
    LPR_LINUX_RLIMIT_NICE = 13,
    LPR_LINUX_RLIMIT_RTPRIO = 14,
    LPR_LINUX_RLIMIT_RTTIME = 15,
    LPR_LINUX_RLIMIT_COUNT = 16,
};

static uint8_t lpr_linux_rlimits_initialized;
static lpr_linux_rlimit_t lpr_linux_rlimits[LPR_LINUX_RLIMIT_COUNT];

typedef struct lpr_file_map_cache_entry {
    uint8_t active;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t vmo_fd;
    uint64_t handle;
    uint64_t length;
} lpr_file_map_cache_entry_t;

enum {
    LPR_FILE_MAP_CACHE_ENTRIES = 4,
    LPR_FILE_MAP_CACHE_MIN_BYTES = 65536,
};

static lpr_file_map_cache_entry_t lpr_file_map_cache[LPR_FILE_MAP_CACHE_ENTRIES];
static uint64_t lpr_file_map_cache_clock;
static const struct lpr_linux_user_frame *lpr_active_user_frame;

static int64_t lpr_linux_pacha_status_to_errno(int64_t status);
static uint64_t lpr_linux_prot_to_pacha(uint64_t prot);

const struct lpr_linux_user_frame *lpr_current_linux_user_frame(void)
{
    return lpr_active_user_frame;
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

static lpr_trace_syscall_metric_t lpr_trace_syscall_metrics[] = {
    { .nr = LPR_LINUX_SYS_READ },
    { .nr = LPR_LINUX_SYS_WRITE },
    { .nr = LPR_LINUX_SYS_OPEN },
    { .nr = LPR_LINUX_SYS_CLOSE },
    { .nr = LPR_LINUX_SYS_STAT },
    { .nr = LPR_LINUX_SYS_FSTAT },
    { .nr = LPR_LINUX_SYS_LSTAT },
    { .nr = LPR_LINUX_SYS_LSEEK },
    { .nr = LPR_LINUX_SYS_MMAP },
    { .nr = LPR_LINUX_SYS_MPROTECT },
    { .nr = LPR_LINUX_SYS_MUNMAP },
    { .nr = LPR_LINUX_SYS_BRK },
    { .nr = LPR_LINUX_SYS_IOCTL },
    { .nr = LPR_LINUX_SYS_PREAD64 },
    { .nr = LPR_LINUX_SYS_READV },
    { .nr = LPR_LINUX_SYS_WRITEV },
    { .nr = LPR_LINUX_SYS_ACCESS },
    { .nr = LPR_LINUX_SYS_CLONE },
    { .nr = LPR_LINUX_SYS_FORK },
    { .nr = LPR_LINUX_SYS_VFORK },
    { .nr = LPR_LINUX_SYS_EXECVE },
    { .nr = LPR_LINUX_SYS_WAIT4 },
    { .nr = LPR_LINUX_SYS_NANOSLEEP },
    { .nr = LPR_LINUX_SYS_GETPID },
    { .nr = LPR_LINUX_SYS_EXIT },
    { .nr = LPR_LINUX_SYS_FCNTL },
    { .nr = LPR_LINUX_SYS_FLOCK },
    { .nr = LPR_LINUX_SYS_SELECT },
    { .nr = LPR_LINUX_SYS_PSELECT6 },
    { .nr = LPR_LINUX_SYS_FSYNC },
    { .nr = LPR_LINUX_SYS_FDATASYNC },
    { .nr = LPR_LINUX_SYS_SYNC },
    { .nr = LPR_LINUX_SYS_GETCWD },
    { .nr = LPR_LINUX_SYS_CHDIR },
    { .nr = LPR_LINUX_SYS_FCHDIR },
    { .nr = LPR_LINUX_SYS_RENAME },
    { .nr = LPR_LINUX_SYS_MKDIR },
    { .nr = LPR_LINUX_SYS_RMDIR },
    { .nr = LPR_LINUX_SYS_LINK },
    { .nr = LPR_LINUX_SYS_UNLINK },
    { .nr = LPR_LINUX_SYS_READLINK },
    { .nr = LPR_LINUX_SYS_CHMOD },
    { .nr = LPR_LINUX_SYS_FCHMOD },
    { .nr = LPR_LINUX_SYS_ARCH_PRCTL },
    { .nr = LPR_LINUX_SYS_GETTID },
    { .nr = LPR_LINUX_SYS_GETDENTS64 },
    { .nr = LPR_LINUX_SYS_SET_TID_ADDRESS },
    { .nr = LPR_LINUX_SYS_CLOCK_GETTIME },
    { .nr = LPR_LINUX_SYS_CLOCK_NANOSLEEP },
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
    { .nr = LPR_LINUX_SYS_SENDMMSG },
    { .nr = LPR_LINUX_SYS_GETRANDOM },
};

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
}

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
    pacha_trace6(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_MMAP_ERROR,
        PACHA_TRACE_CLASS_DEBUG,
        pacha_trace_name_id(stage),
        len,
        prot,
        flags,
        fd,
        (uint64_t)status);
    pacha_trace2(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_EVENT_LPR_MMAP_ERROR, PACHA_TRACE_CLASS_DEBUG, addr, offset);
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

static void lpr_trace_file_map_cache(
    const char *event,
    uint64_t handle,
    uint64_t offset,
    uint64_t length,
    uint64_t entry_length)
{
    pacha_trace5(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_FILE_MAP_CACHE,
        PACHA_TRACE_CLASS_DEBUG,
        pacha_trace_name_id(event),
        handle,
        offset,
        length,
        entry_length);
}

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

static uint64_t lpr_page_align_up(uint64_t value)
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

static lpr_file_map_cache_entry_t *lpr_file_map_cache_find(uint64_t handle, uint64_t offset, uint64_t length)
{
    if (handle == 0 || offset + length < offset) {
        return 0;
    }
    for (uint64_t i = 0; i < LPR_FILE_MAP_CACHE_ENTRIES; i += 1) {
        lpr_file_map_cache_entry_t *entry = &lpr_file_map_cache[i];
        if (entry->active &&
            entry->handle == handle &&
            offset + length <= entry->length)
        {
            return entry;
        }
    }
    return 0;
}

static void lpr_file_map_cache_store(uint64_t handle, int64_t vmo_fd, uint64_t length)
{
    if (handle == 0 || vmo_fd < 16 || length < LPR_FILE_MAP_CACHE_MIN_BYTES) {
        return;
    }
    for (uint64_t i = 0; i < LPR_FILE_MAP_CACHE_ENTRIES; i += 1) {
        lpr_file_map_cache_entry_t *entry = &lpr_file_map_cache[i];
        if (entry->active && entry->handle == handle) {
            if (entry->vmo_fd >= 16 && entry->vmo_fd != (uint32_t)vmo_fd) {
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, entry->vmo_fd);
            }
            entry->vmo_fd = (uint32_t)vmo_fd;
            entry->length = length;
            return;
        }
    }
    const uint64_t slot = lpr_file_map_cache_clock++ % LPR_FILE_MAP_CACHE_ENTRIES;
    lpr_file_map_cache_entry_t *entry = &lpr_file_map_cache[slot];
    if (entry->active && entry->vmo_fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, entry->vmo_fd);
    }
    entry->active = 1;
    entry->vmo_fd = (uint32_t)vmo_fd;
    entry->handle = handle;
    entry->length = length;
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
        out |= PACHAOS_PROT_WRITE;
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

static int64_t lpr_dispatch_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset)
{
    uint64_t pacha_flags;
    const int flag_status = lpr_linux_mmap_flags_to_pacha(flags, &pacha_flags);
    if (flag_status != 0 || len == 0) {
        return -LPR_LINUX_EINVAL;
    }
    if ((flags & LPR_LINUX_MAP_ANONYMOUS) == 0 && lpr_linux_filed_fd_active(fd)) {
        if ((flags & LPR_LINUX_MAP_SHARED) != 0) {
            return -LPR_LINUX_ENOTSUP;
        }
        if ((offset & 4095ull) != 0) {
            return -LPR_LINUX_EINVAL;
        }
        const uint64_t map_len = lpr_page_align_up(len);
        if (map_len == 0) {
            return -LPR_LINUX_ENOMEM;
        }
        const uint64_t load_prot =
            PACHAOS_PROT_READ |
            PACHAOS_PROT_WRITE |
            (lpr_linux_prot_to_pacha(prot) & PACHAOS_PROT_EXEC);
        const uint64_t final_prot = lpr_linux_prot_to_pacha(prot);
        const uint64_t handle = lpr_linux_filed_fd_handle(fd);
        uint64_t done = 0;
        int64_t mapped = 0;
        int64_t vmo_fd = -1;
        uint64_t mapped_prot = load_prot;
        const uint64_t readonly_shared_map_flags =
            (pacha_flags & (PACHAOS_MMAP_FIXED | PACHAOS_MMAP_FIXED_NOREPLACE | PACHAOS_MMAP_NORESERVE)) |
            PACHAOS_MMAP_SHARED;
        const uint64_t private_file_map_flags =
            pacha_flags & (PACHAOS_MMAP_FIXED |
                           PACHAOS_MMAP_FIXED_NOREPLACE |
                           PACHAOS_MMAP_NORESERVE |
                           PACHAOS_MMAP_PRIVATE |
                           PACHAOS_MMAP_SHARED);
        const uint64_t direct_map_flags =
            (prot & (LPR_LINUX_PROT_WRITE | LPR_LINUX_PROT_EXEC)) == 0
                ? readonly_shared_map_flags
                : private_file_map_flags;
        lpr_file_map_cache_entry_t *cache = lpr_file_map_cache_find(handle, offset, map_len);
        lpr_trace_file_map_cache(cache != 0 ? "hit" : "miss", handle, offset, map_len, cache != 0 ? cache->length : 0);
        if (cache != 0) {
            if ((prot & (LPR_LINUX_PROT_WRITE | LPR_LINUX_PROT_EXEC)) == 0) {
                mapped = lpr_pacha_syscall6(
                    PACHAOS_SYSCALL_MMAP,
                    cache->vmo_fd,
                    addr,
                    map_len,
                    final_prot,
                    readonly_shared_map_flags,
                    offset);
                if (mapped >= 4096) {
                    done = len;
                    mapped_prot = final_prot;
                    goto file_mapping_ready;
                }
            }
            const uint64_t target_map_flags =
                (pacha_flags | PACHAOS_MMAP_ANONYMOUS) & ~PACHAOS_MMAP_SHARED;
            mapped = lpr_pacha_syscall6(
                PACHAOS_SYSCALL_MMAP,
                0,
                addr,
                map_len,
                load_prot,
                target_map_flags,
                0);
            if (mapped >= 4096) {
                const int64_t source = lpr_pacha_syscall6(
                    PACHAOS_SYSCALL_MMAP,
                    cache->vmo_fd,
                    0,
                    map_len,
                    PACHAOS_PROT_READ,
                    PACHAOS_MMAP_SHARED,
                    offset);
                if (source >= 4096) {
                    lpr_memcpy((void *)(uintptr_t)mapped, (const void *)(uintptr_t)source, (size_t)map_len);
                    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)source, map_len);
                    done = len;
                    goto file_mapping_ready;
                }
                (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)mapped, map_len);
                mapped = 0;
            }
        }
        if (prot == LPR_LINUX_PROT_READ) {
            uint64_t loaded = 0;
            const int64_t cached_vmo_fd = lpr_linux_file_vmo(fd, offset, map_len, &loaded);
            if (cached_vmo_fd >= 16) {
                mapped = lpr_pacha_syscall6(
                    PACHAOS_SYSCALL_MMAP,
                    (uint64_t)(uint32_t)cached_vmo_fd,
                    addr,
                    map_len,
                    final_prot,
                    direct_map_flags,
                    0);
                if (mapped >= 4096) {
                    vmo_fd = cached_vmo_fd;
                    done = loaded;
                    mapped_prot = final_prot;
                    goto maybe_store_file_mapping;
                }
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)cached_vmo_fd);
            }
        }
        const uint64_t vmo_rights =
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE |
            PACHA_FD_RIGHT_MAP_EXEC;
        vmo_fd = lpr_pacha_syscall3(
            PACHAOS_SYSCALL_VMO_CREATE,
            map_len,
            vmo_rights,
            0);
        if (vmo_fd < 16) {
            lpr_trace_mmap_error("vmo_create", addr, len, prot, flags, fd, offset, vmo_fd);
            return lpr_linux_pacha_status_to_errno(vmo_fd);
        }
        const int64_t loaded = lpr_linux_pread_to_vmo(fd, (uint64_t)(uint32_t)vmo_fd, 0, len, offset);
        if (loaded < 0) {
            lpr_trace_mmap_error("pread_to_vmo", addr, len, prot, flags, fd, offset, loaded);
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
            return loaded;
        }
        done = (uint64_t)loaded;
        lpr_trace_mmap_load(len, done, prot, flags, fd, offset);
        mapped = lpr_pacha_syscall6(
            PACHAOS_SYSCALL_MMAP,
            (uint64_t)(uint32_t)vmo_fd,
            addr,
            map_len,
            load_prot,
            direct_map_flags,
            0);
        mapped_prot = load_prot;
        if (mapped < 4096) {
            lpr_trace_mmap_error("direct_vmo_mmap", addr, len, prot, flags, fd, offset, mapped);
            mapped = lpr_pacha_syscall6(
                PACHAOS_SYSCALL_MMAP,
                0,
                addr,
                map_len,
                load_prot,
                (pacha_flags | PACHAOS_MMAP_ANONYMOUS) & ~PACHAOS_MMAP_SHARED,
                0);
            if (mapped < 4096) {
                lpr_trace_mmap_error("target_mmap", addr, len, prot, flags, fd, offset, mapped);
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
                return lpr_linux_pacha_status_to_errno(mapped);
            }
            const int64_t source = lpr_pacha_syscall6(
                PACHAOS_SYSCALL_MMAP,
                (uint64_t)(uint32_t)vmo_fd,
                0,
                map_len,
                PACHAOS_PROT_READ,
                PACHAOS_MMAP_SHARED,
                0);
            if (source < 4096) {
                lpr_trace_mmap_error("source_mmap", addr, len, prot, flags, fd, offset, source);
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
                (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)mapped, map_len);
                return lpr_linux_pacha_status_to_errno(source);
            }
            if (done != 0) {
                lpr_memcpy((void *)(uintptr_t)mapped, (const void *)(uintptr_t)source, (size_t)done);
            }
            (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)source, map_len);
        }
maybe_store_file_mapping:
        if (mapped >= 4096 &&
            offset == 0 &&
            prot == LPR_LINUX_PROT_READ &&
            (pacha_flags & (PACHAOS_MMAP_FIXED | PACHAOS_MMAP_FIXED_NOREPLACE)) == 0 &&
            done != 0)
        {
            lpr_file_map_cache_store(handle, vmo_fd, map_len);
            lpr_trace_file_map_cache("store", handle, offset, map_len, map_len);
            vmo_fd = -1;
        }
        if (vmo_fd >= 16) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
        }
file_mapping_ready:
        if ((prot & LPR_LINUX_PROT_EXEC) != 0 && done != 0) {
            struct lpr_patch_mapping_result patch_result;
            const struct lpr_patch_mapping_request patch_request = {
                .start_va = (uint64_t)mapped,
                .size_bytes = done,
                .flags = LPR_PATCH_FLAG_EXECUTABLE | LPR_PATCH_FLAG_PRIVATE,
            };
            const int64_t patch_status = lpr_patch_mapping(&patch_request, &patch_result);
            lpr_trace_patch_mapping(&patch_result);
            if (patch_status != PERSONALITY_STATUS_OK) {
                (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)mapped, map_len);
                return -LPR_LINUX_EINVAL;
            }
        }
        if (final_prot != mapped_prot) {
            const int64_t protect_status = lpr_pacha_syscall3(
                PACHAOS_SYSCALL_MPROTECT,
                (uint64_t)mapped,
                map_len,
                final_prot);
            if (protect_status != 0) {
                (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)mapped, map_len);
                return lpr_linux_pacha_status_to_errno(protect_status);
            }
        }
        const int64_t mmap_result = mapped;
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
    const int64_t result = lpr_linux_pacha_status_to_errno(ret);
    lpr_trace_mmap_call("mmap", addr, len, prot, flags, fd, offset, result);
    return result;
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
    char name[16];
    enum lpr_linux_syscall_class cls;
    enum lpr_linux_syscall_backend backend;
    lpr_syscall_handler_t handler;
    uint8_t trace_socket;
} lpr_syscall_entry_t;

static int64_t lpr_sys_read(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_socket_fd_active(a0) ? lpr_linux_socket_read(a0, a1, a2) : lpr_linux_read(a0, a1, a2); }
static int64_t lpr_sys_write(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_socket_fd_active(a0) ? lpr_linux_socket_write(a0, a1, a2) : lpr_linux_write(a0, a1, a2); }
static int64_t lpr_sys_open(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_openat(LPR_LINUX_AT_FDCWD, a0, a1, a2); }
static int64_t lpr_sys_close(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_socket_fd_active(a0) ? lpr_linux_socket_close(a0) : lpr_linux_close(a0); }
static int64_t lpr_sys_close_range(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_close_range(a0, a1, a2); }
static int64_t lpr_sys_stat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_newfstatat(LPR_LINUX_AT_FDCWD, a0, a1, 0); }
static int64_t lpr_sys_lstat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_newfstatat(LPR_LINUX_AT_FDCWD, a0, a1, 0x100); }
static int64_t lpr_sys_lseek(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_lseek(a0, a1, a2); }
static int64_t lpr_sys_mmap(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { return lpr_dispatch_mmap(a0, a1, a2, a3, a4, a5); }
static int64_t lpr_sys_mprotect(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_pacha_status_to_errno(lpr_pacha_syscall3(PACHAOS_SYSCALL_MPROTECT, a0, a1, lpr_linux_prot_to_pacha(a2))); }
static int64_t lpr_sys_munmap(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; const int64_t result = lpr_linux_pacha_status_to_errno(lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, a0, a1)); lpr_trace_mmap_call("munmap", a0, a1, 0, 0, 0, 0, result); return result; }
static int64_t lpr_sys_brk(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_brk(a0); }
static int64_t lpr_sys_rt_sigaction(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_rt_sigaction(a0, a1, a2, a3); }
static int64_t lpr_sys_rt_sigprocmask(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_rt_sigprocmask(a0, a1, a2, a3); }
static int64_t lpr_sys_ioctl(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_socket_fd_active(a0) ? lpr_linux_socket_ioctl(a0, a1, a2) : lpr_linux_ioctl(a0, a1, a2); }
static int64_t lpr_sys_pread64(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_pread64(a0, a1, a2, a3); }
static int64_t lpr_sys_readv(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_socket_fd_active(a0) ? lpr_linux_socket_readv(a0, a1, a2) : lpr_linux_readv(a0, a1, a2); }
static int64_t lpr_sys_writev(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_socket_fd_active(a0) ? lpr_linux_socket_writev(a0, a1, a2) : lpr_linux_writev(a0, a1, a2); }
static int64_t lpr_sys_access(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_access(a0, a1); }
static int64_t lpr_sys_pipe(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_pipe2(a0, 0); }
static int64_t lpr_sys_select(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_linux_select(a0, a1, a2, a3, a4); }
static int64_t lpr_sys_dup(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_dup(a0, 0, 0); }
static int64_t lpr_sys_dup2(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_dup2(a0, a1, 0); }
static int64_t lpr_sys_nanosleep(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_nanosleep(a0, a1); }
static int64_t lpr_sys_getpid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_getpid(); }
static int64_t lpr_sys_socket(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_socket(a0, a1, a2); }
static int64_t lpr_sys_connect(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_connect(a0, a1, a2); }
static int64_t lpr_sys_eopnotsupp(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return -LPR_LINUX_EOPNOTSUPP; }
static int64_t lpr_sys_sendto(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { return lpr_linux_sendto(a0, a1, a2, a3, a4, a5); }
static int64_t lpr_sys_recvfrom(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { return lpr_linux_recvfrom(a0, a1, a2, a3, a4, a5); }
static int64_t lpr_sys_sendmsg(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_sendmsg(a0, a1, a2); }
static int64_t lpr_sys_recvmsg(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_recvmsg(a0, a1, a2); }
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
static int64_t lpr_sys_exit(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; lpr_linux_prepare_process_exit(a0); (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, a0); for (;;) {} }
static int64_t lpr_sys_wait4(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_wait4(a0, a1, a2, a3); }
static int64_t lpr_sys_kill(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_kill(a0, a1); }
static int64_t lpr_sys_uname(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_uname(a0); }
static int64_t lpr_sys_fstat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_socket_fd_active(a0) ? lpr_linux_socket_fstat(a0, a1) : lpr_linux_fstat(a0, a1); }
static int64_t lpr_sys_fsync(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_fsync(a0); }
static int64_t lpr_sys_sync(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_sync(); }
static int64_t lpr_sys_getcwd(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_getcwd(a0, a1); }
static int64_t lpr_sys_chdir(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_chdir(a0); }
static int64_t lpr_sys_fchdir(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_fchdir(a0); }
static int64_t lpr_sys_rename(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_renameat(LPR_LINUX_AT_FDCWD, a0, LPR_LINUX_AT_FDCWD, a1); }
static int64_t lpr_sys_mkdir(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_mkdirat(LPR_LINUX_AT_FDCWD, a0, a1); }
static int64_t lpr_sys_rmdir(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_unlinkat(LPR_LINUX_AT_FDCWD, a0, LPR_LINUX_AT_REMOVEDIR); }
static int64_t lpr_sys_link(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_linkat(LPR_LINUX_AT_FDCWD, a0, LPR_LINUX_AT_FDCWD, a1, 0); }
static int64_t lpr_sys_unlink(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_unlinkat(LPR_LINUX_AT_FDCWD, a0, 0); }
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
static int64_t lpr_sys_getresid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_getresid(a0, a1, a2); }
static int64_t lpr_sys_getppid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_getppid(); }
static int64_t lpr_sys_getpgrp(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_getpgrp(); }
static int64_t lpr_sys_setpgid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_setpgid(a0, a1); }
static int64_t lpr_sys_getpgid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_getpgid(a0); }
static int64_t lpr_sys_setsid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_setsid(); }
static int64_t lpr_sys_getsid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_getsid(a0); }
static int64_t lpr_sys_setrlimit(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_set_rlimit(a0, a1); }
static int64_t lpr_sys_arch_prctl(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_dispatch_arch_prctl(a0, a1); }
static int64_t lpr_sys_getdents64(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_getdents64(a0, a1, a2); }
static int64_t lpr_sys_clock_gettime(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_pacha_status_to_errno(lpr_pacha_syscall2(PACHAOS_SYSCALL_CLOCK_GETTIME, a0, a1)); }
static int64_t lpr_sys_clock_nanosleep(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_clock_nanosleep(a0, a1, a2, a3); }
static int64_t lpr_sys_openat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_openat(a0, a1, a2, a3); }
static int64_t lpr_sys_mkdirat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_mkdirat(a0, a1, a2); }
static int64_t lpr_sys_mknodat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_mknodat(a0, a1, a2, a3); }
static int64_t lpr_sys_fchownat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_linux_fchownat(a0, a1, a2, a3, a4); }
static int64_t lpr_sys_newfstatat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_newfstatat(a0, a1, a2, a3); }
static int64_t lpr_sys_unlinkat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_unlinkat(a0, a1, a2); }
static int64_t lpr_sys_renameat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_renameat(a0, a1, a2, a3); }
static int64_t lpr_sys_linkat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_linux_linkat(a0, a1, a2, a3, a4); }
static int64_t lpr_sys_symlinkat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_symlinkat(a0, a1, a2); }
static int64_t lpr_sys_fchmodat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_fchmodat(a0, a1, a2, a3); }
static int64_t lpr_sys_faccessat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_faccessat(a0, a1, a2, 0); }
static int64_t lpr_sys_pselect6(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { return lpr_linux_pselect6(a0, a1, a2, a3, a4, a5); }
static int64_t lpr_sys_ppoll(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_linux_ppoll(a0, a1, a2, a3, a4); }
static int64_t lpr_sys_utimensat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_utimensat(a0, a1, a2, a3); }
static int64_t lpr_sys_eventfd(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_eventfd2(a0, 0); }
static int64_t lpr_sys_eventfd2(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_eventfd2(a0, a1); }
static int64_t lpr_sys_dup3(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_dup2(a0, a1, a2); }
static int64_t lpr_sys_pipe2(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_pipe2(a0, a1); }
static int64_t lpr_sys_recvmmsg(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a5; return lpr_linux_recvmmsg(a0, a1, a2, a3, a4); }
static int64_t lpr_sys_prlimit64(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_prlimit64(a0, a1, a2, a3); }
static int64_t lpr_sys_sendmmsg(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a4; (void)a5; return lpr_linux_sendmmsg(a0, a1, a2, a3); }
static int64_t lpr_sys_getrandom(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_pacha_syscall3(PACHAOS_SYSCALL_GETRANDOM, a0, a1, a2); }
static int64_t lpr_sys_fcntl(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_socket_fd_active(a0) ? lpr_linux_socket_fcntl(a0, a1, a2) : lpr_linux_fcntl(a0, a1, a2); }
static int64_t lpr_sys_flock(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a2; (void)a3; (void)a4; (void)a5; return lpr_linux_flock(a0, a1); }
static int64_t lpr_sys_poll(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { (void)a3; (void)a4; (void)a5; return lpr_linux_poll(a0, a1, a2); }

#define LPR_SYSCALL_TRACE 1u
#define LPR_SYSCALL(nr_value, name_value, class_value, backend_value, handler_value, trace_value) \
    [nr_value] = { nr_value, name_value, class_value, backend_value, 0, trace_value }

static lpr_syscall_entry_t lpr_syscall_table[LPR_LINUX_SYS_CLOSE_RANGE + 1u] = {
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
    LPR_SYSCALL(LPR_LINUX_SYS_BRK, "brk", LPR_LINUX_SYSCALL_CLASS_MEMORY, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_brk, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_RT_SIGACTION, "rt_sigaction", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_rt_sigaction, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_RT_SIGPROCMASK, "rt_sigprocmask", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_rt_sigprocmask, 0),
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
    LPR_SYSCALL(LPR_LINUX_SYS_GETPID, "getpid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_getpid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SOCKET, "socket", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_socket, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_CONNECT, "connect", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_connect, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_ACCEPT, "accept", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_eopnotsupp, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SENDTO, "sendto", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_sendto, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_RECVFROM, "recvfrom", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_recvfrom, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_SENDMSG, "sendmsg", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_sendmsg, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_RECVMSG, "recvmsg", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_recvmsg, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_SHUTDOWN, "shutdown", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_shutdown, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_BIND, "bind", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_bind, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_LISTEN, "listen", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_eopnotsupp, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETSOCKNAME, "getsockname", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_getsockname, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_GETPEERNAME, "getpeername", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_getpeername, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_SOCKETPAIR, "socketpair", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_eopnotsupp, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SETSOCKOPT, "setsockopt", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_setsockopt, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_GETSOCKOPT, "getsockopt", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_getsockopt, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_CLONE, "clone", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_clone, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FORK, "fork", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_fork, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_VFORK, "vfork", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_vfork, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_EXECVE, "execve", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_execve, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_EXIT, "exit", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_exit, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_WAIT4, "wait4", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_wait4, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_KILL, "kill", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_kill, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_UNAME, "uname", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_uname, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FCNTL, "fcntl", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_fcntl, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_FLOCK, "flock", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_flock, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FSYNC, "fsync", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_fsync, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FDATASYNC, "fdatasync", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_fsync, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETCWD, "getcwd", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_getcwd, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_CHDIR, "chdir", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_chdir, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_FCHDIR, "fchdir", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_fchdir, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_RENAME, "rename", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_rename, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_MKDIR, "mkdir", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_mkdir, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_RMDIR, "rmdir", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_rmdir, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_LINK, "link", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_link, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_UNLINK, "unlink", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_unlink, 0),
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
    LPR_SYSCALL(LPR_LINUX_SYS_GETRESUID, "getresuid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_getresid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETRESGID, "getresgid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_getresid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETPGID, "getpgid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_getpgid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETSID, "getsid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_getsid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SETPRIORITY, "setpriority", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_zero, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SETRLIMIT, "setrlimit", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_setrlimit, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SYNC, "sync", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_sync, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_ARCH_PRCTL, "arch_prctl", LPR_LINUX_SYSCALL_CLASS_THREAD_ARCH, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_arch_prctl, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETTID, "gettid", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_getpid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_GETDENTS64, "getdents64", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_getdents64, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_SET_TID_ADDRESS, "set_tid_address", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_getpid, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_CLOCK_GETTIME, "clock_gettime", LPR_LINUX_SYSCALL_CLASS_TIME_RANDOM, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_clock_gettime, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_CLOCK_NANOSLEEP, "clock_nanosleep", LPR_LINUX_SYSCALL_CLASS_TIME_RANDOM, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_clock_nanosleep, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_EXIT_GROUP, "exit_group", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_exit, 0),
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
    LPR_SYSCALL(LPR_LINUX_SYS_UNSHARE, "unshare", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_zero, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_UTIMENSAT, "utimensat", LPR_LINUX_SYSCALL_CLASS_VFS_PATH, LPR_LINUX_SYSCALL_BACKEND_FILED, lpr_sys_utimensat, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_EVENTFD, "eventfd", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_eventfd, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_EVENTFD2, "eventfd2", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_eventfd2, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_DUP3, "dup3", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_dup3, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_PIPE2, "pipe2", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_pipe2, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_RECVMMSG, "recvmmsg", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_recvmmsg, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_PRLIMIT64, "prlimit64", LPR_LINUX_SYSCALL_CLASS_PROCESS, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_prlimit64, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_SENDMMSG, "sendmmsg", LPR_LINUX_SYSCALL_CLASS_FD_IO, LPR_LINUX_SYSCALL_BACKEND_COORDINATOR, lpr_sys_sendmmsg, LPR_SYSCALL_TRACE),
    LPR_SYSCALL(LPR_LINUX_SYS_GETRANDOM, "getrandom", LPR_LINUX_SYSCALL_CLASS_TIME_RANDOM, LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT, lpr_sys_getrandom, 0),
    LPR_SYSCALL(LPR_LINUX_SYS_CLOSE_RANGE, "close_range", LPR_LINUX_SYSCALL_CLASS_FD_CONTROL, LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE, lpr_sys_close_range, LPR_SYSCALL_TRACE),
};

static int lpr_syscall_table_initialized;

static void lpr_syscall_table_init(void)
{
    if (lpr_syscall_table_initialized) {
        return;
    }
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
    lpr_syscall_table[LPR_LINUX_SYS_BRK].handler = lpr_sys_brk;
    lpr_syscall_table[LPR_LINUX_SYS_RT_SIGACTION].handler = lpr_sys_rt_sigaction;
    lpr_syscall_table[LPR_LINUX_SYS_RT_SIGPROCMASK].handler = lpr_sys_rt_sigprocmask;
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
    lpr_syscall_table[LPR_LINUX_SYS_GETPID].handler = lpr_sys_getpid;
    lpr_syscall_table[LPR_LINUX_SYS_SOCKET].handler = lpr_sys_socket;
    lpr_syscall_table[LPR_LINUX_SYS_CONNECT].handler = lpr_sys_connect;
    lpr_syscall_table[LPR_LINUX_SYS_ACCEPT].handler = lpr_sys_eopnotsupp;
    lpr_syscall_table[LPR_LINUX_SYS_SENDTO].handler = lpr_sys_sendto;
    lpr_syscall_table[LPR_LINUX_SYS_RECVFROM].handler = lpr_sys_recvfrom;
    lpr_syscall_table[LPR_LINUX_SYS_SENDMSG].handler = lpr_sys_sendmsg;
    lpr_syscall_table[LPR_LINUX_SYS_RECVMSG].handler = lpr_sys_recvmsg;
    lpr_syscall_table[LPR_LINUX_SYS_SHUTDOWN].handler = lpr_sys_shutdown;
    lpr_syscall_table[LPR_LINUX_SYS_BIND].handler = lpr_sys_bind;
    lpr_syscall_table[LPR_LINUX_SYS_LISTEN].handler = lpr_sys_eopnotsupp;
    lpr_syscall_table[LPR_LINUX_SYS_GETSOCKNAME].handler = lpr_sys_getsockname;
    lpr_syscall_table[LPR_LINUX_SYS_GETPEERNAME].handler = lpr_sys_getpeername;
    lpr_syscall_table[LPR_LINUX_SYS_SOCKETPAIR].handler = lpr_sys_eopnotsupp;
    lpr_syscall_table[LPR_LINUX_SYS_SETSOCKOPT].handler = lpr_sys_setsockopt;
    lpr_syscall_table[LPR_LINUX_SYS_GETSOCKOPT].handler = lpr_sys_getsockopt;
    lpr_syscall_table[LPR_LINUX_SYS_CLONE].handler = lpr_sys_clone;
    lpr_syscall_table[LPR_LINUX_SYS_FORK].handler = lpr_sys_fork;
    lpr_syscall_table[LPR_LINUX_SYS_VFORK].handler = lpr_sys_vfork;
    lpr_syscall_table[LPR_LINUX_SYS_EXECVE].handler = lpr_sys_execve;
    lpr_syscall_table[LPR_LINUX_SYS_EXIT].handler = lpr_sys_exit;
    lpr_syscall_table[LPR_LINUX_SYS_WAIT4].handler = lpr_sys_wait4;
    lpr_syscall_table[LPR_LINUX_SYS_KILL].handler = lpr_sys_kill;
    lpr_syscall_table[LPR_LINUX_SYS_UNAME].handler = lpr_sys_uname;
    lpr_syscall_table[LPR_LINUX_SYS_FCNTL].handler = lpr_sys_fcntl;
    lpr_syscall_table[LPR_LINUX_SYS_FLOCK].handler = lpr_sys_flock;
    lpr_syscall_table[LPR_LINUX_SYS_FSYNC].handler = lpr_sys_fsync;
    lpr_syscall_table[LPR_LINUX_SYS_FDATASYNC].handler = lpr_sys_fsync;
    lpr_syscall_table[LPR_LINUX_SYS_GETCWD].handler = lpr_sys_getcwd;
    lpr_syscall_table[LPR_LINUX_SYS_CHDIR].handler = lpr_sys_chdir;
    lpr_syscall_table[LPR_LINUX_SYS_FCHDIR].handler = lpr_sys_fchdir;
    lpr_syscall_table[LPR_LINUX_SYS_RENAME].handler = lpr_sys_rename;
    lpr_syscall_table[LPR_LINUX_SYS_MKDIR].handler = lpr_sys_mkdir;
    lpr_syscall_table[LPR_LINUX_SYS_RMDIR].handler = lpr_sys_rmdir;
    lpr_syscall_table[LPR_LINUX_SYS_LINK].handler = lpr_sys_link;
    lpr_syscall_table[LPR_LINUX_SYS_UNLINK].handler = lpr_sys_unlink;
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
    lpr_syscall_table[LPR_LINUX_SYS_GETRESUID].handler = lpr_sys_getresid;
    lpr_syscall_table[LPR_LINUX_SYS_GETRESGID].handler = lpr_sys_getresid;
    lpr_syscall_table[LPR_LINUX_SYS_GETPGID].handler = lpr_sys_getpgid;
    lpr_syscall_table[LPR_LINUX_SYS_GETSID].handler = lpr_sys_getsid;
    lpr_syscall_table[LPR_LINUX_SYS_SETPRIORITY].handler = lpr_sys_zero;
    lpr_syscall_table[LPR_LINUX_SYS_SETRLIMIT].handler = lpr_sys_setrlimit;
    lpr_syscall_table[LPR_LINUX_SYS_SYNC].handler = lpr_sys_sync;
    lpr_syscall_table[LPR_LINUX_SYS_ARCH_PRCTL].handler = lpr_sys_arch_prctl;
    lpr_syscall_table[LPR_LINUX_SYS_GETTID].handler = lpr_sys_getpid;
    lpr_syscall_table[LPR_LINUX_SYS_GETDENTS64].handler = lpr_sys_getdents64;
    lpr_syscall_table[LPR_LINUX_SYS_SET_TID_ADDRESS].handler = lpr_sys_getpid;
    lpr_syscall_table[LPR_LINUX_SYS_CLOCK_GETTIME].handler = lpr_sys_clock_gettime;
    lpr_syscall_table[LPR_LINUX_SYS_CLOCK_NANOSLEEP].handler = lpr_sys_clock_nanosleep;
    lpr_syscall_table[LPR_LINUX_SYS_EXIT_GROUP].handler = lpr_sys_exit;
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
    lpr_syscall_table[LPR_LINUX_SYS_UNSHARE].handler = lpr_sys_zero;
    lpr_syscall_table[LPR_LINUX_SYS_UTIMENSAT].handler = lpr_sys_utimensat;
    lpr_syscall_table[LPR_LINUX_SYS_EVENTFD].handler = lpr_sys_eventfd;
    lpr_syscall_table[LPR_LINUX_SYS_EVENTFD2].handler = lpr_sys_eventfd2;
    lpr_syscall_table[LPR_LINUX_SYS_DUP3].handler = lpr_sys_dup3;
    lpr_syscall_table[LPR_LINUX_SYS_PIPE2].handler = lpr_sys_pipe2;
    lpr_syscall_table[LPR_LINUX_SYS_RECVMMSG].handler = lpr_sys_recvmmsg;
    lpr_syscall_table[LPR_LINUX_SYS_PRLIMIT64].handler = lpr_sys_prlimit64;
    lpr_syscall_table[LPR_LINUX_SYS_SENDMMSG].handler = lpr_sys_sendmmsg;
    lpr_syscall_table[LPR_LINUX_SYS_GETRANDOM].handler = lpr_sys_getrandom;
    lpr_syscall_table[LPR_LINUX_SYS_CLOSE_RANGE].handler = lpr_sys_close_range;
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
    static struct lpr_linux_syscall_info info_cache[LPR_LINUX_SYS_CLOSE_RANGE + 1u];
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

int64_t lpr_dispatch_syscall_frame(const struct lpr_linux_user_frame *frame,
                                   uint64_t nr,
                                   uint64_t a0,
                                   uint64_t a1,
                                   uint64_t a2,
                                   uint64_t a3,
                                   uint64_t a4,
                                   uint64_t a5) {
    const struct lpr_linux_user_frame *saved_frame = lpr_active_user_frame;
    lpr_active_user_frame = frame;
    if (frame != 0 && frame->rip < LPR_LOW_GUARD_END_VA) {
        pacha_trace3(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_EVENT_LPR_BAD_RETURN, PACHA_TRACE_CLASS_ERROR, nr, frame->rip, frame->rcx);
    }
    const lpr_syscall_entry_t *entry = lpr_syscall_lookup_entry(nr);
    const int trace_socket_syscall = entry != 0 && entry->trace_socket;
    if (trace_socket_syscall) {
        lpr_trace_socket_syscall_event("enter", nr, a0, a1, a2, 0);
    }
    lpr_linux_apply_pending_fork_child();
    lpr_linux_ensure_default_stdio();
    const int64_t pre_signal_status = lpr_linux_dispatch_pending_signals();
    if (pre_signal_status != 0) {
        lpr_active_user_frame = saved_frame;
        return pre_signal_status;
    }
    const int trace_metrics = pacha_trace_enabled(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_CLASS_METRIC);
    if (trace_metrics && (nr == LPR_LINUX_SYS_EXIT || nr == LPR_LINUX_SYS_EXIT_GROUP)) {
        lpr_trace_syscall_record(nr, 0, 0);
        lpr_linux_readv_cache_trace_dump();
        lpr_trace_syscall_dump(nr);
        lpr_linux_prepare_process_exit(a0);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, a0);
        for (;;) {
        }
    }
    const uint64_t start_cycles = trace_metrics ? pacha_trace_read_tsc() : 0;
    const int64_t result = lpr_dispatch_syscall_inner(entry, a0, a1, a2, a3, a4, a5);
    const uint64_t end_cycles = trace_metrics ? pacha_trace_read_tsc() : 0;
    const uint64_t cycles = end_cycles >= start_cycles ? end_cycles - start_cycles : 0;
    if (trace_metrics) {
        lpr_trace_syscall_record(nr, cycles, result);
    }
    if (cycles >= 10000000ull) {
        lpr_trace_slow_syscall(nr, a0, a1, a2, a3, a4, a5, result, cycles);
    }
    if (trace_socket_syscall) {
        lpr_trace_socket_syscall_event("exit", nr, a0, a1, a2, result);
    }
    const int64_t post_signal_status = lpr_linux_dispatch_pending_signals();
    lpr_active_user_frame = saved_frame;
    if (result == -LPR_LINUX_ENOSYS) {
        lpr_trace_enosys_syscall(nr, a0, a1, a2, a3, a4, a5);
    }
    return post_signal_status != 0 ? post_signal_status : result;
}

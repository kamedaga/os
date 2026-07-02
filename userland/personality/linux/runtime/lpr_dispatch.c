#include "lpr_linux_syscall.h"
#include "lpr_filed.h"
#include "lpr_memory.h"
#include "lpr_vfs_local.h"
#include "support/string.h"
#include "support/syscall.h"
#include <pacha/ipc.h>
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
#define LPR_LINUX_AT_REMOVEDIR 0x200ull

static uint64_t lpr_linux_umask_value;

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

#if LPR_TRACE_PATCH_MAPPING || LPR_TRACE_SYSCALL_METRICS || LPR_TRACE_MMAP_LOADS || LPR_TRACE_MMAP_CALLS || LPR_TRACE_ENOSYS
static char *lpr_append_literal(char *out, const char *end, const char *text)
{
    while (out < end && *text != 0) {
        *out++ = *text++;
    }
    return out;
}

static char *lpr_append_u64(char *out, const char *end, uint64_t value)
{
    char tmp[20];
    uint64_t n = 0;
    do {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0 && n < sizeof(tmp));
    while (out < end && n != 0) {
        *out++ = tmp[--n];
    }
    return out;
}
#endif

#if LPR_TRACE_MMAP_CALLS
static char *lpr_append_i64(char *out, const char *end, int64_t value)
{
    if (value < 0) {
        out = lpr_append_literal(out, end, "-");
        return lpr_append_u64(out, end, (uint64_t)(-value));
    }
    return lpr_append_u64(out, end, (uint64_t)value);
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
    char line[320];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_append_literal(out, end, "[lpr_runtime] mmap_call op=");
    out = lpr_append_literal(out, end, op);
    out = lpr_append_literal(out, end, " addr=");
    out = lpr_append_u64(out, end, addr);
    out = lpr_append_literal(out, end, " len=");
    out = lpr_append_u64(out, end, len);
    out = lpr_append_literal(out, end, " prot=");
    out = lpr_append_u64(out, end, prot);
    out = lpr_append_literal(out, end, " flags=");
    out = lpr_append_u64(out, end, flags);
    out = lpr_append_literal(out, end, " fd=");
    out = lpr_append_u64(out, end, fd);
    out = lpr_append_literal(out, end, " offset=");
    out = lpr_append_u64(out, end, offset);
    out = lpr_append_literal(out, end, " result=");
    out = lpr_append_i64(out, end, result);
    out = lpr_append_literal(out, end, "\n");
    (void)lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, 2, (uint64_t)(uintptr_t)line, (uint64_t)(out - line));
}
#endif

#if LPR_TRACE_PATCH_MAPPING
static void lpr_trace_patch_mapping(const struct lpr_patch_mapping_result *result)
{
    if (result == 0) {
        return;
    }
    char line[192];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_append_literal(out, end, "[lpr_runtime] metric scope=lpr_runtime op=patch_mapping");
    out = lpr_append_literal(out, end, " scanned_bytes=");
    out = lpr_append_u64(out, end, result->scanned_bytes);
    out = lpr_append_literal(out, end, " patched_sites=");
    out = lpr_append_u64(out, end, result->patched_sites);
    out = lpr_append_literal(out, end, " skipped_sites=");
    out = lpr_append_u64(out, end, result->skipped_sites);
    out = lpr_append_literal(out, end, " cycles=");
    out = lpr_append_u64(out, end, result->cycles);
    out = lpr_append_literal(out, end, "\n");
    if (out > line) {
        (void)lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, 2, (uint64_t)(uintptr_t)line, (uint64_t)(out - line));
    }
}
#endif

#if LPR_TRACE_SYSCALL_METRICS
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
    { .nr = LPR_LINUX_SYS_GETPID },
    { .nr = LPR_LINUX_SYS_EXIT },
    { .nr = LPR_LINUX_SYS_FCNTL },
    { .nr = LPR_LINUX_SYS_FSYNC },
    { .nr = LPR_LINUX_SYS_FDATASYNC },
    { .nr = LPR_LINUX_SYS_GETCWD },
    { .nr = LPR_LINUX_SYS_RENAME },
    { .nr = LPR_LINUX_SYS_MKDIR },
    { .nr = LPR_LINUX_SYS_RMDIR },
    { .nr = LPR_LINUX_SYS_UNLINK },
    { .nr = LPR_LINUX_SYS_READLINK },
    { .nr = LPR_LINUX_SYS_CHMOD },
    { .nr = LPR_LINUX_SYS_FCHMOD },
    { .nr = LPR_LINUX_SYS_ARCH_PRCTL },
    { .nr = LPR_LINUX_SYS_GETTID },
    { .nr = LPR_LINUX_SYS_GETDENTS64 },
    { .nr = LPR_LINUX_SYS_SET_TID_ADDRESS },
    { .nr = LPR_LINUX_SYS_CLOCK_GETTIME },
    { .nr = LPR_LINUX_SYS_EXIT_GROUP },
    { .nr = LPR_LINUX_SYS_OPENAT },
    { .nr = LPR_LINUX_SYS_MKDIRAT },
    { .nr = LPR_LINUX_SYS_NEWFSTATAT },
    { .nr = LPR_LINUX_SYS_UNLINKAT },
    { .nr = LPR_LINUX_SYS_RENAMEAT },
    { .nr = LPR_LINUX_SYS_FCHMODAT },
    { .nr = LPR_LINUX_SYS_UTIMENSAT },
    { .nr = LPR_LINUX_SYS_GETRANDOM },
};

static uint64_t lpr_trace_read_tsc(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

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

static void lpr_trace_write_line(const char *line, uint64_t size)
{
    if (line != 0 && size != 0) {
        (void)lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, 2, (uint64_t)(uintptr_t)line, size);
    }
}

static char *lpr_trace_append_syscall_name(char *out, const char *end, uint64_t nr)
{
    switch (nr) {
    case LPR_LINUX_SYS_READ: return lpr_append_literal(out, end, "read");
    case LPR_LINUX_SYS_WRITE: return lpr_append_literal(out, end, "write");
    case LPR_LINUX_SYS_OPEN: return lpr_append_literal(out, end, "open");
    case LPR_LINUX_SYS_CLOSE: return lpr_append_literal(out, end, "close");
    case LPR_LINUX_SYS_STAT: return lpr_append_literal(out, end, "stat");
    case LPR_LINUX_SYS_FSTAT: return lpr_append_literal(out, end, "fstat");
    case LPR_LINUX_SYS_LSTAT: return lpr_append_literal(out, end, "lstat");
    case LPR_LINUX_SYS_LSEEK: return lpr_append_literal(out, end, "lseek");
    case LPR_LINUX_SYS_MMAP: return lpr_append_literal(out, end, "mmap");
    case LPR_LINUX_SYS_MPROTECT: return lpr_append_literal(out, end, "mprotect");
    case LPR_LINUX_SYS_MUNMAP: return lpr_append_literal(out, end, "munmap");
    case LPR_LINUX_SYS_BRK: return lpr_append_literal(out, end, "brk");
    case LPR_LINUX_SYS_RT_SIGACTION: return lpr_append_literal(out, end, "rt_sigaction");
    case LPR_LINUX_SYS_RT_SIGPROCMASK: return lpr_append_literal(out, end, "rt_sigprocmask");
    case LPR_LINUX_SYS_IOCTL: return lpr_append_literal(out, end, "ioctl");
    case LPR_LINUX_SYS_PREAD64: return lpr_append_literal(out, end, "pread64");
    case LPR_LINUX_SYS_READV: return lpr_append_literal(out, end, "readv");
    case LPR_LINUX_SYS_WRITEV: return lpr_append_literal(out, end, "writev");
    case LPR_LINUX_SYS_ACCESS: return lpr_append_literal(out, end, "access");
    case LPR_LINUX_SYS_PIPE: return lpr_append_literal(out, end, "pipe");
    case LPR_LINUX_SYS_DUP: return lpr_append_literal(out, end, "dup");
    case LPR_LINUX_SYS_CLONE: return lpr_append_literal(out, end, "clone");
    case LPR_LINUX_SYS_FORK: return lpr_append_literal(out, end, "fork");
    case LPR_LINUX_SYS_VFORK: return lpr_append_literal(out, end, "vfork");
    case LPR_LINUX_SYS_EXECVE: return lpr_append_literal(out, end, "execve");
    case LPR_LINUX_SYS_WAIT4: return lpr_append_literal(out, end, "wait4");
    case LPR_LINUX_SYS_GETPID: return lpr_append_literal(out, end, "getpid");
    case LPR_LINUX_SYS_EXIT: return lpr_append_literal(out, end, "exit");
    case LPR_LINUX_SYS_FCNTL: return lpr_append_literal(out, end, "fcntl");
    case LPR_LINUX_SYS_FSYNC: return lpr_append_literal(out, end, "fsync");
    case LPR_LINUX_SYS_FDATASYNC: return lpr_append_literal(out, end, "fdatasync");
    case LPR_LINUX_SYS_GETCWD: return lpr_append_literal(out, end, "getcwd");
    case LPR_LINUX_SYS_RENAME: return lpr_append_literal(out, end, "rename");
    case LPR_LINUX_SYS_MKDIR: return lpr_append_literal(out, end, "mkdir");
    case LPR_LINUX_SYS_RMDIR: return lpr_append_literal(out, end, "rmdir");
    case LPR_LINUX_SYS_UNLINK: return lpr_append_literal(out, end, "unlink");
    case LPR_LINUX_SYS_READLINK: return lpr_append_literal(out, end, "readlink");
    case LPR_LINUX_SYS_CHMOD: return lpr_append_literal(out, end, "chmod");
    case LPR_LINUX_SYS_FCHMOD: return lpr_append_literal(out, end, "fchmod");
    case LPR_LINUX_SYS_UMASK: return lpr_append_literal(out, end, "umask");
    case LPR_LINUX_SYS_GETUID: return lpr_append_literal(out, end, "getuid");
    case LPR_LINUX_SYS_GETGID: return lpr_append_literal(out, end, "getgid");
    case LPR_LINUX_SYS_SETUID: return lpr_append_literal(out, end, "setuid");
    case LPR_LINUX_SYS_SETGID: return lpr_append_literal(out, end, "setgid");
    case LPR_LINUX_SYS_GETEUID: return lpr_append_literal(out, end, "geteuid");
    case LPR_LINUX_SYS_GETEGID: return lpr_append_literal(out, end, "getegid");
    case LPR_LINUX_SYS_SETPRIORITY: return lpr_append_literal(out, end, "setpriority");
    case LPR_LINUX_SYS_ARCH_PRCTL: return lpr_append_literal(out, end, "arch_prctl");
    case LPR_LINUX_SYS_GETTID: return lpr_append_literal(out, end, "gettid");
    case LPR_LINUX_SYS_GETDENTS64: return lpr_append_literal(out, end, "getdents64");
    case LPR_LINUX_SYS_SET_TID_ADDRESS: return lpr_append_literal(out, end, "set_tid_address");
    case LPR_LINUX_SYS_CLOCK_GETTIME: return lpr_append_literal(out, end, "clock_gettime");
    case LPR_LINUX_SYS_EXIT_GROUP: return lpr_append_literal(out, end, "exit_group");
    case LPR_LINUX_SYS_OPENAT: return lpr_append_literal(out, end, "openat");
    case LPR_LINUX_SYS_MKDIRAT: return lpr_append_literal(out, end, "mkdirat");
    case LPR_LINUX_SYS_MKNODAT: return lpr_append_literal(out, end, "mknodat");
    case LPR_LINUX_SYS_FCHOWNAT: return lpr_append_literal(out, end, "fchownat");
    case LPR_LINUX_SYS_NEWFSTATAT: return lpr_append_literal(out, end, "newfstatat");
    case LPR_LINUX_SYS_UNLINKAT: return lpr_append_literal(out, end, "unlinkat");
    case LPR_LINUX_SYS_RENAMEAT: return lpr_append_literal(out, end, "renameat");
    case LPR_LINUX_SYS_FCHMODAT: return lpr_append_literal(out, end, "fchmodat");
    case LPR_LINUX_SYS_FACCESSAT: return lpr_append_literal(out, end, "faccessat");
    case LPR_LINUX_SYS_SYMLINKAT: return lpr_append_literal(out, end, "symlinkat");
    case LPR_LINUX_SYS_UNSHARE: return lpr_append_literal(out, end, "unshare");
    case LPR_LINUX_SYS_UTIMENSAT: return lpr_append_literal(out, end, "utimensat");
    case LPR_LINUX_SYS_DUP3: return lpr_append_literal(out, end, "dup3");
    case LPR_LINUX_SYS_PIPE2: return lpr_append_literal(out, end, "pipe2");
    case LPR_LINUX_SYS_GETRANDOM: return lpr_append_literal(out, end, "getrandom");
    default: return lpr_append_literal(out, end, "unknown");
    }
}

static void lpr_trace_syscall_dump(uint64_t exit_nr)
{
    const uint64_t pid = (uint64_t)lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID);
    const uint64_t tid = (uint64_t)lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID);
    uint64_t total_count = 0;
    uint64_t total_cycles = 0;
    char line[256];
    for (uint64_t i = 0; i < sizeof(lpr_trace_syscall_metrics) / sizeof(lpr_trace_syscall_metrics[0]); ++i) {
        const lpr_trace_syscall_metric_t *metric = &lpr_trace_syscall_metrics[i];
        if (metric->count == 0) {
            continue;
        }
        total_count += metric->count;
        total_cycles += metric->total_cycles;
        char *out = line;
        const char *end = line + sizeof(line);
        out = lpr_append_literal(out, end, "[lpr_runtime] metric scope=lpr_syscall pid=");
        out = lpr_append_u64(out, end, pid);
        out = lpr_append_literal(out, end, " tid=");
        out = lpr_append_u64(out, end, tid);
        out = lpr_append_literal(out, end, " op=");
        out = lpr_trace_append_syscall_name(out, end, metric->nr);
        out = lpr_append_literal(out, end, " nr=");
        out = lpr_append_u64(out, end, metric->nr);
        out = lpr_append_literal(out, end, " count=");
        out = lpr_append_u64(out, end, metric->count);
        out = lpr_append_literal(out, end, " avg_cycles=");
        out = lpr_append_u64(out, end, metric->total_cycles / metric->count);
        out = lpr_append_literal(out, end, " max_cycles=");
        out = lpr_append_u64(out, end, metric->max_cycles);
        out = lpr_append_literal(out, end, " errors=");
        out = lpr_append_u64(out, end, metric->errors);
        out = lpr_append_literal(out, end, "\n");
        lpr_trace_write_line(line, (uint64_t)(out - line));
    }
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_append_literal(out, end, "[lpr_runtime] metric scope=lpr_syscall_summary pid=");
    out = lpr_append_u64(out, end, pid);
    out = lpr_append_literal(out, end, " tid=");
    out = lpr_append_u64(out, end, tid);
    out = lpr_append_literal(out, end, " exit_nr=");
    out = lpr_append_u64(out, end, exit_nr);
    out = lpr_append_literal(out, end, " total_count=");
    out = lpr_append_u64(out, end, total_count);
    out = lpr_append_literal(out, end, " total_cycles=");
    out = lpr_append_u64(out, end, total_cycles);
    out = lpr_append_literal(out, end, "\n");
    lpr_trace_write_line(line, (uint64_t)(out - line));
}

#if LPR_TRACE_SLOW_SYSCALLS
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
    char line[384];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_append_literal(out, end, "[lpr_runtime] slow_syscall nr=");
    out = lpr_append_u64(out, end, nr);
    out = lpr_append_literal(out, end, " a0=");
    out = lpr_append_u64(out, end, a0);
    out = lpr_append_literal(out, end, " a1=");
    out = lpr_append_u64(out, end, a1);
    out = lpr_append_literal(out, end, " a2=");
    out = lpr_append_u64(out, end, a2);
    out = lpr_append_literal(out, end, " a3=");
    out = lpr_append_u64(out, end, a3);
    out = lpr_append_literal(out, end, " a4=");
    out = lpr_append_u64(out, end, a4);
    out = lpr_append_literal(out, end, " a5=");
    out = lpr_append_u64(out, end, a5);
    out = lpr_append_literal(out, end, " result=");
    if (result < 0) {
        out = lpr_append_literal(out, end, "-");
        out = lpr_append_u64(out, end, (uint64_t)(-result));
    } else {
        out = lpr_append_u64(out, end, (uint64_t)result);
    }
    out = lpr_append_literal(out, end, " cycles=");
    out = lpr_append_u64(out, end, cycles);
    out = lpr_append_literal(out, end, "\n");
    lpr_trace_write_line(line, (uint64_t)(out - line));
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
    char line[320];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_append_literal(out, end, "[lpr_runtime] mmap_error stage=");
    out = lpr_append_literal(out, end, stage);
    out = lpr_append_literal(out, end, " addr=");
    out = lpr_append_u64(out, end, addr);
    out = lpr_append_literal(out, end, " len=");
    out = lpr_append_u64(out, end, len);
    out = lpr_append_literal(out, end, " prot=");
    out = lpr_append_u64(out, end, prot);
    out = lpr_append_literal(out, end, " flags=");
    out = lpr_append_u64(out, end, flags);
    out = lpr_append_literal(out, end, " fd=");
    out = lpr_append_u64(out, end, fd);
    out = lpr_append_literal(out, end, " offset=");
    out = lpr_append_u64(out, end, offset);
    out = lpr_append_literal(out, end, " status=");
    if (status < 0) {
        out = lpr_append_literal(out, end, "-");
        out = lpr_append_u64(out, end, (uint64_t)(-status));
    } else {
        out = lpr_append_u64(out, end, (uint64_t)status);
    }
    out = lpr_append_literal(out, end, "\n");
    lpr_trace_write_line(line, (uint64_t)(out - line));
}
#endif

#if LPR_TRACE_MMAP_LOADS
static void lpr_trace_mmap_load(
    uint64_t len,
    uint64_t loaded,
    uint64_t prot,
    uint64_t flags,
    uint64_t fd,
    uint64_t offset)
{
    char line[240];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_append_literal(out, end, "[lpr_runtime] mmap_load len=");
    out = lpr_append_u64(out, end, len);
    out = lpr_append_literal(out, end, " loaded=");
    out = lpr_append_u64(out, end, loaded);
    out = lpr_append_literal(out, end, " prot=");
    out = lpr_append_u64(out, end, prot);
    out = lpr_append_literal(out, end, " flags=");
    out = lpr_append_u64(out, end, flags);
    out = lpr_append_literal(out, end, " fd=");
    out = lpr_append_u64(out, end, fd);
    out = lpr_append_literal(out, end, " offset=");
    out = lpr_append_u64(out, end, offset);
    out = lpr_append_literal(out, end, "\n");
    (void)lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, 2, (uint64_t)(uintptr_t)line, (uint64_t)(out - line));
}

static void lpr_trace_file_map_cache(
    const char *event,
    uint64_t handle,
    uint64_t offset,
    uint64_t length,
    uint64_t entry_length)
{
    char line[240];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_append_literal(out, end, "[lpr_runtime] file_map_cache event=");
    out = lpr_append_literal(out, end, event);
    out = lpr_append_literal(out, end, " handle=");
    out = lpr_append_u64(out, end, handle);
    out = lpr_append_literal(out, end, " offset=");
    out = lpr_append_u64(out, end, offset);
    out = lpr_append_literal(out, end, " length=");
    out = lpr_append_u64(out, end, length);
    out = lpr_append_literal(out, end, " entry_length=");
    out = lpr_append_u64(out, end, entry_length);
    out = lpr_append_literal(out, end, "\n");
    (void)lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, 2, (uint64_t)(uintptr_t)line, (uint64_t)(out - line));
}
#endif

#if LPR_TRACE_ENOSYS
static void lpr_trace_enosys_syscall(uint64_t nr,
                                     uint64_t a0,
                                     uint64_t a1,
                                     uint64_t a2,
                                     uint64_t a3,
                                     uint64_t a4,
                                     uint64_t a5)
{
    char line[384];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_append_literal(out, end, "[lpr_runtime] enosys nr=");
    out = lpr_append_u64(out, end, nr);
    out = lpr_append_literal(out, end, " a0=");
    out = lpr_append_u64(out, end, a0);
    out = lpr_append_literal(out, end, " a1=");
    out = lpr_append_u64(out, end, a1);
    out = lpr_append_literal(out, end, " a2=");
    out = lpr_append_u64(out, end, a2);
    out = lpr_append_literal(out, end, " a3=");
    out = lpr_append_u64(out, end, a3);
    out = lpr_append_literal(out, end, " a4=");
    out = lpr_append_u64(out, end, a4);
    out = lpr_append_literal(out, end, " a5=");
    out = lpr_append_u64(out, end, a5);
    out = lpr_append_literal(out, end, "\n");
    (void)lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, 2, (uint64_t)(uintptr_t)line, (uint64_t)(out - line));
}
#endif

static uint64_t lpr_page_align_up(uint64_t value)
{
    const uint64_t mask = 4095ull;
    if (value > UINT64_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
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
    if (status == 0) {
        return status;
    }
    int negative = 0;
    if (status < 0) {
        negative = 1;
        status = -status;
    }
    if (status > PACHAOS_SYSCALL_ERR_EMPTY) {
        return negative ? -status : status;
    }
    switch (status) {
    case PACHAOS_SYSCALL_ERR_INVALID:
        return -LPR_LINUX_EINVAL;
    case PACHAOS_SYSCALL_ERR_ALLOC:
    case PACHAOS_SYSCALL_ERR_MAP:
        return -LPR_LINUX_ENOMEM;
    case PACHAOS_SYSCALL_ERR_NOT_READY:
    case PACHAOS_SYSCALL_ERR_EMPTY:
        return -LPR_LINUX_EAGAIN;
    default:
        return -LPR_LINUX_EINVAL;
    }
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
        const uint64_t direct_map_flags =
            (pacha_flags & (PACHAOS_MMAP_FIXED | PACHAOS_MMAP_FIXED_NOREPLACE | PACHAOS_MMAP_NORESERVE)) |
            PACHAOS_MMAP_SHARED;
        lpr_file_map_cache_entry_t *cache = lpr_file_map_cache_find(handle, offset, map_len);
#if LPR_TRACE_MMAP_LOADS
        lpr_trace_file_map_cache(cache != 0 ? "hit" : "miss", handle, offset, map_len, cache != 0 ? cache->length : 0);
#endif
        if (cache != 0) {
            if ((prot & (LPR_LINUX_PROT_WRITE | LPR_LINUX_PROT_EXEC)) == 0) {
                const uint64_t cached_map_flags =
                    (pacha_flags & (PACHAOS_MMAP_FIXED | PACHAOS_MMAP_FIXED_NOREPLACE | PACHAOS_MMAP_NORESERVE)) |
                    PACHAOS_MMAP_SHARED;
                mapped = lpr_pacha_syscall6(
                    PACHAOS_SYSCALL_MMAP,
                    cache->vmo_fd,
                    addr,
                    map_len,
                    final_prot,
                    cached_map_flags,
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
#if LPR_TRACE_SYSCALL_METRICS
            lpr_trace_mmap_error("vmo_create", addr, len, prot, flags, fd, offset, vmo_fd);
#endif
            return lpr_linux_pacha_status_to_errno(vmo_fd);
        }
        const int64_t loaded = lpr_linux_pread_to_vmo(fd, (uint64_t)(uint32_t)vmo_fd, 0, len, offset);
        if (loaded < 0) {
#if LPR_TRACE_SYSCALL_METRICS
            lpr_trace_mmap_error("pread_to_vmo", addr, len, prot, flags, fd, offset, loaded);
#endif
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
            return loaded;
        }
        done = (uint64_t)loaded;
#if LPR_TRACE_MMAP_LOADS
        lpr_trace_mmap_load(len, done, prot, flags, fd, offset);
#endif
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
#if LPR_TRACE_SYSCALL_METRICS
            lpr_trace_mmap_error("direct_vmo_mmap", addr, len, prot, flags, fd, offset, mapped);
#endif
            mapped = lpr_pacha_syscall6(
                PACHAOS_SYSCALL_MMAP,
                0,
                addr,
                map_len,
                load_prot,
                (pacha_flags | PACHAOS_MMAP_ANONYMOUS) & ~PACHAOS_MMAP_SHARED,
                0);
            if (mapped < 4096) {
#if LPR_TRACE_SYSCALL_METRICS
                lpr_trace_mmap_error("target_mmap", addr, len, prot, flags, fd, offset, mapped);
#endif
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
#if LPR_TRACE_SYSCALL_METRICS
                lpr_trace_mmap_error("source_mmap", addr, len, prot, flags, fd, offset, source);
#endif
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
#if LPR_TRACE_MMAP_LOADS
            lpr_trace_file_map_cache("store", handle, offset, map_len, map_len);
#endif
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
#if LPR_TRACE_PATCH_MAPPING
            lpr_trace_patch_mapping(&patch_result);
#endif
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
#if LPR_TRACE_MMAP_CALLS
        lpr_trace_mmap_call("mmap", addr, len, prot, flags, fd, offset, mmap_result);
#endif
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
#if LPR_TRACE_MMAP_CALLS
    lpr_trace_mmap_call("mmap", addr, len, prot, flags, fd, offset, result);
#endif
    return result;
}

static int64_t lpr_dispatch_syscall_inner(uint64_t nr,
                                          uint64_t a0,
                                          uint64_t a1,
                                          uint64_t a2,
                                          uint64_t a3,
                                          uint64_t a4,
                                          uint64_t a5) {
    switch (nr) {
    case LPR_LINUX_SYS_READ:
        return lpr_linux_read(a0, a1, a2);
    case LPR_LINUX_SYS_GETPID:
        return lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID);
    case LPR_LINUX_SYS_GETTID:
    case LPR_LINUX_SYS_SET_TID_ADDRESS:
        return lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID);
    case LPR_LINUX_SYS_WRITE:
        return lpr_linux_write(a0, a1, a2);
    case LPR_LINUX_SYS_OPEN:
        return lpr_linux_openat(LPR_LINUX_AT_FDCWD, a0, a1, a2);
    case LPR_LINUX_SYS_CLOSE:
        return lpr_linux_close(a0);
    case LPR_LINUX_SYS_STAT:
        return lpr_linux_newfstatat(LPR_LINUX_AT_FDCWD, a0, a1, 0);
    case LPR_LINUX_SYS_LSEEK:
        return lpr_linux_lseek(a0, a1, a2);
    case LPR_LINUX_SYS_LSTAT:
        return lpr_linux_newfstatat(LPR_LINUX_AT_FDCWD, a0, a1, 0x100);
    case LPR_LINUX_SYS_MMAP:
        return lpr_dispatch_mmap(a0, a1, a2, a3, a4, a5);
    case LPR_LINUX_SYS_MPROTECT:
        return lpr_linux_pacha_status_to_errno(
            lpr_pacha_syscall3(PACHAOS_SYSCALL_MPROTECT, a0, a1, lpr_linux_prot_to_pacha(a2)));
    case LPR_LINUX_SYS_MUNMAP: {
        const int64_t result = lpr_linux_pacha_status_to_errno(lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, a0, a1));
#if LPR_TRACE_MMAP_CALLS
        lpr_trace_mmap_call("munmap", a0, a1, 0, 0, 0, 0, result);
#endif
        return result;
    }
    case LPR_LINUX_SYS_ARCH_PRCTL:
        return lpr_dispatch_arch_prctl(a0, a1);
    case LPR_LINUX_SYS_RT_SIGACTION:
    case LPR_LINUX_SYS_RT_SIGPROCMASK:
        return 0;
    case LPR_LINUX_SYS_CLOCK_GETTIME:
        return lpr_linux_pacha_status_to_errno(lpr_pacha_syscall2(PACHAOS_SYSCALL_CLOCK_GETTIME, a0, a1));
    case LPR_LINUX_SYS_GETRANDOM:
        return lpr_pacha_syscall3(PACHAOS_SYSCALL_GETRANDOM, a0, a1, a2);
    case LPR_LINUX_SYS_EXIT:
    case LPR_LINUX_SYS_EXIT_GROUP:
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, a0);
        for (;;) {
        }
    case LPR_LINUX_SYS_BRK:
        return lpr_linux_brk(a0);
    case LPR_LINUX_SYS_GETUID:
    case LPR_LINUX_SYS_GETGID:
    case LPR_LINUX_SYS_GETEUID:
    case LPR_LINUX_SYS_GETEGID:
        return 0;
    case LPR_LINUX_SYS_SETUID:
    case LPR_LINUX_SYS_SETGID:
        return a0 == 0 ? 0 : -LPR_LINUX_EPERM;
    case LPR_LINUX_SYS_SETPRIORITY:
        return 0;
    case LPR_LINUX_SYS_UMASK: {
        const uint64_t previous = lpr_linux_umask_value;
        lpr_linux_umask_value = a0 & 0777ull;
        return (int64_t)previous;
    }
    case LPR_LINUX_SYS_IOCTL:
        return lpr_linux_ioctl(a0, a1, a2);
    case LPR_LINUX_SYS_FSTAT:
        return lpr_linux_fstat(a0, a1);
    case LPR_LINUX_SYS_FSYNC:
    case LPR_LINUX_SYS_FDATASYNC:
        return lpr_linux_fsync(a0);
    case LPR_LINUX_SYS_PREAD64:
        return lpr_linux_pread64(a0, a1, a2, a3);
    case LPR_LINUX_SYS_READV:
        return lpr_linux_readv(a0, a1, a2);
    case LPR_LINUX_SYS_WRITEV:
        return lpr_linux_writev(a0, a1, a2);
    case LPR_LINUX_SYS_ACCESS:
        return lpr_linux_access(a0, a1);
    case LPR_LINUX_SYS_PIPE:
        return lpr_linux_pipe2(a0, 0);
    case LPR_LINUX_SYS_PIPE2:
        return lpr_linux_pipe2(a0, a1);
    case LPR_LINUX_SYS_DUP:
        return lpr_linux_dup(a0, 0, 0);
    case LPR_LINUX_SYS_CLONE:
        return lpr_linux_clone(a0, a1, a2, a3, a4);
    case LPR_LINUX_SYS_FORK:
        return lpr_linux_fork();
    case LPR_LINUX_SYS_VFORK:
        return lpr_linux_vfork();
    case LPR_LINUX_SYS_EXECVE:
        return lpr_linux_execve(a0, a1, a2);
    case LPR_LINUX_SYS_WAIT4:
        return lpr_linux_wait4(a0, a1, a2, a3);
    case LPR_LINUX_SYS_DUP3:
        return -LPR_LINUX_ENOTSUP;
    case LPR_LINUX_SYS_READLINK:
        return lpr_linux_readlink(a0, a1, a2);
    case LPR_LINUX_SYS_CHMOD:
        return lpr_linux_fchmodat(LPR_LINUX_AT_FDCWD, a0, a1, 0);
    case LPR_LINUX_SYS_FCHMOD:
        return lpr_linux_fchmod(a0, a1);
    case LPR_LINUX_SYS_RENAME:
        return lpr_linux_renameat(LPR_LINUX_AT_FDCWD, a0, LPR_LINUX_AT_FDCWD, a1);
    case LPR_LINUX_SYS_MKDIR:
        return lpr_linux_mkdirat(LPR_LINUX_AT_FDCWD, a0, a1);
    case LPR_LINUX_SYS_RMDIR:
        return lpr_linux_unlinkat(LPR_LINUX_AT_FDCWD, a0, LPR_LINUX_AT_REMOVEDIR);
    case LPR_LINUX_SYS_UNLINK:
        return lpr_linux_unlinkat(LPR_LINUX_AT_FDCWD, a0, 0);
    case LPR_LINUX_SYS_GETDENTS64:
        return lpr_linux_getdents64(a0, a1, a2);
    case LPR_LINUX_SYS_OPENAT:
        return lpr_linux_openat(a0, a1, a2, a3);
    case LPR_LINUX_SYS_MKDIRAT:
        return lpr_linux_mkdirat(a0, a1, a2);
    case LPR_LINUX_SYS_MKNODAT:
        return lpr_linux_mknodat(a0, a1, a2, a3);
    case LPR_LINUX_SYS_FCHOWNAT:
        return lpr_linux_fchownat(a0, a1, a2, a3, a4);
    case LPR_LINUX_SYS_NEWFSTATAT:
        return lpr_linux_newfstatat(a0, a1, a2, a3);
    case LPR_LINUX_SYS_UNLINKAT:
        return lpr_linux_unlinkat(a0, a1, a2);
    case LPR_LINUX_SYS_RENAMEAT:
        return lpr_linux_renameat(a0, a1, a2, a3);
    case LPR_LINUX_SYS_FCHMODAT:
        return lpr_linux_fchmodat(a0, a1, a2, a3);
    case LPR_LINUX_SYS_FACCESSAT:
        return lpr_linux_faccessat(a0, a1, a2, 0);
    case LPR_LINUX_SYS_SYMLINKAT:
        return lpr_linux_symlinkat(a0, a1, a2);
    case LPR_LINUX_SYS_UNSHARE:
        return 0;
    case LPR_LINUX_SYS_UTIMENSAT:
        return lpr_linux_utimensat(a0, a1, a2, a3);
    case LPR_LINUX_SYS_GETCWD:
        return lpr_linux_getcwd(a0, a1);
    case LPR_LINUX_SYS_FCNTL:
        return lpr_linux_fcntl(a0, a1, a2);
    default:
        return -LPR_LINUX_ENOSYS;
    }
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
#if LPR_TRACE_SYSCALL_METRICS
    if (nr == LPR_LINUX_SYS_EXIT || nr == LPR_LINUX_SYS_EXIT_GROUP) {
        lpr_trace_syscall_record(nr, 0, 0);
        lpr_linux_readv_cache_trace_dump();
        lpr_trace_syscall_dump(nr);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, a0);
        for (;;) {
        }
    }
    const uint64_t start_cycles = lpr_trace_read_tsc();
    const int64_t result = lpr_dispatch_syscall_inner(nr, a0, a1, a2, a3, a4, a5);
    const uint64_t end_cycles = lpr_trace_read_tsc();
    const uint64_t cycles = end_cycles >= start_cycles ? end_cycles - start_cycles : 0;
    lpr_trace_syscall_record(nr, cycles, result);
#if LPR_TRACE_SLOW_SYSCALLS
    if (cycles >= 10000000ull) {
        lpr_trace_slow_syscall(nr, a0, a1, a2, a3, a4, a5, result, cycles);
    }
#endif
    lpr_active_user_frame = saved_frame;
    return result;
#else
    const int64_t result = lpr_dispatch_syscall_inner(nr, a0, a1, a2, a3, a4, a5);
#if LPR_TRACE_ENOSYS
    if (result == -LPR_LINUX_ENOSYS) {
        lpr_trace_enosys_syscall(nr, a0, a1, a2, a3, a4, a5);
    }
#endif
    lpr_active_user_frame = saved_frame;
    return result;
#endif
}

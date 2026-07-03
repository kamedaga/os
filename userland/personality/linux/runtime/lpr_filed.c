#include "lpr_filed.h"
#include "lpr_linux_syscall.h"
#include "support/string.h"
#include "support/syscall.h"
#include <filed/ipc_protocol.h>
#include <pacha/ipc.h>
#include <pachaos/abi.h>
#include <personality/linux_lpr.h>
#include <stddef.h>
#include <stdint.h>

__attribute__((visibility("hidden")))
void *memset(void *dst, int c, size_t n)
{
    return lpr_memset(dst, c, n);
}

#define LPR_FD_TABLE_SIZE 128u
#define LPR_LINUX_AT_FDCWD (-100)
#define LPR_LINUX_AT_SYMLINK_NOFOLLOW 0x100ull
#define LPR_LINUX_AT_SYMLINK_FOLLOW 0x400ull
#define LPR_LINUX_AT_EMPTY_PATH 0x1000ull
#define LPR_LINUX_AT_REMOVEDIR 0x200ull
#define LPR_LINUX_O_ACCMODE 00000003ull
#define LPR_LINUX_O_RDONLY 00000000ull
#define LPR_LINUX_O_WRONLY 00000001ull
#define LPR_LINUX_O_RDWR 00000002ull
#define LPR_LINUX_O_CREAT 00000100ull
#define LPR_LINUX_O_EXCL 00000200ull
#define LPR_LINUX_O_TRUNC 00001000ull
#define LPR_LINUX_O_APPEND 00002000ull
#define LPR_LINUX_O_NONBLOCK 00004000ull
#define LPR_LINUX_O_DIRECTORY 00200000ull
#define LPR_LINUX_O_NOFOLLOW 00400000ull
#define LPR_LINUX_O_CLOEXEC 02000000ull
#define LPR_LINUX_F_DUPFD 0ull
#define LPR_LINUX_F_GETFD 1ull
#define LPR_LINUX_F_SETFD 2ull
#define LPR_LINUX_F_GETFL 3ull
#define LPR_LINUX_F_SETFL 4ull
#define LPR_LINUX_F_DUPFD_CLOEXEC 1030ull
#define LPR_LINUX_FD_CLOEXEC 1ull
#define LPR_LINUX_TCGETS 0x5401ull
#define LPR_LINUX_TIOCGWINSZ 0x5413ull
#define LPR_LINUX_UTIME_NOW 1073741823ll
#define LPR_LINUX_UTIME_OMIT 1073741822ll
#define LPR_FILED_PAGE_CACHE_ENTRIES 64u
#define LPR_FILED_PAGE_CACHE_BYTES 4096ull
#define LPR_FILED_READV_TO_VMO_MIN (16ull * 1024ull)
#define LPR_FILED_READV_TO_VMO_MAX (256ull * 1024ull)
#define LPR_LINUX_PIPE_BUF_BYTES 4096ull
#define LPR_LINUX_PIPE_MAP_BYTES 8192ull
#define LPR_LINUX_PIPE_COUNT 16u

#define LPR_LINUX_S_IFMT 0170000ull
#define LPR_LINUX_S_IFIFO 0010000ull
#define LPR_LINUX_S_IFCHR 0020000ull
#define LPR_LINUX_S_IFDIR 0040000ull
#define LPR_LINUX_S_IFBLK 0060000ull
#define LPR_LINUX_S_IFREG 0100000ull
#define LPR_LINUX_S_IFLNK 0120000ull
#define LPR_LINUX_S_IFSOCK 0140000ull

#define LPR_LINUX_DT_UNKNOWN 0u
#define LPR_LINUX_DT_FIFO 1u
#define LPR_LINUX_DT_CHR 2u
#define LPR_LINUX_DT_DIR 4u
#define LPR_LINUX_DT_BLK 6u
#define LPR_LINUX_DT_REG 8u
#define LPR_LINUX_DT_LNK 10u
#define LPR_LINUX_DT_SOCK 12u

typedef struct lpr_filed_fd {
    uint8_t active;
    uint8_t offset_valid;
    uint8_t pread_active;
    uint8_t reserved1;
    uint32_t flags;
    uint64_t handle;
    uint64_t offset;
} lpr_filed_fd_t;

typedef struct lpr_pipe_fd {
    uint8_t active;
    uint8_t pipe_id;
    uint8_t readable;
    uint8_t writable;
    uint32_t flags;
} lpr_pipe_fd_t;

typedef struct lpr_pipe_entry {
    uint8_t active;
    uint8_t read_refs;
    uint8_t write_refs;
    uint8_t reserved0;
    int32_t vmo_fd;
    uint32_t head;
    uint32_t tail;
    uint32_t used;
    uint8_t data[LPR_LINUX_PIPE_BUF_BYTES];
} lpr_pipe_entry_t;

typedef struct lpr_linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    int64_t st_atime_sec;
    int64_t st_atime_nsec;
    int64_t st_mtime_sec;
    int64_t st_mtime_nsec;
    int64_t st_ctime_sec;
    int64_t st_ctime_nsec;
    int64_t __unused[3];
} lpr_linux_stat_t;

typedef struct lpr_linux_iovec {
    uint64_t base;
    uint64_t len;
} lpr_linux_iovec_t;

typedef struct lpr_linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
} lpr_linux_timespec_t;

typedef struct lpr_readlink_cache_entry {
    uint8_t active;
    uint8_t reserved0;
    uint16_t length;
    int64_t status;
    char path[FILED_WIRE_PATH_BYTES];
} lpr_readlink_cache_entry_t;

typedef struct lpr_filed_page_cache_entry {
    uint8_t active;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t reserved2;
    uint64_t handle;
    uint64_t page_start;
    uint64_t length;
    uint64_t clock;
    unsigned char data[LPR_FILED_PAGE_CACHE_BYTES];
} lpr_filed_page_cache_entry_t;

enum {
    LPR_READLINK_CACHE_ENTRIES = 8,
};

static lpr_filed_fd_t lpr_fds[LPR_FD_TABLE_SIZE];
static lpr_pipe_fd_t lpr_pipe_fds[LPR_FD_TABLE_SIZE];
static lpr_pipe_entry_t *lpr_pipes[LPR_LINUX_PIPE_COUNT];
static lpr_readlink_cache_entry_t lpr_readlink_cache[LPR_READLINK_CACHE_ENTRIES];
static lpr_filed_page_cache_entry_t lpr_page_cache[LPR_FILED_PAGE_CACHE_ENTRIES];
static uint64_t lpr_readlink_cache_clock;
static uint64_t lpr_page_cache_clock;
static uint64_t lpr_request_id = 0x4c505246494c4501ull;
static int lpr_filed_endpoint_checked;
static int lpr_wire_page_fd = -1;
static void *lpr_wire_page;
static int lpr_wire_page_busy;
static int lpr_session_fd = -1;
static int lpr_session_page_fd = -1;
static void *lpr_session_page;
static int lpr_session_checked;
static int lpr_session_payload_busy;
static int lpr_readv_vmo_fd = -1;
static void *lpr_readv_vmo_map;
static uint64_t lpr_readv_vmo_len;
static int lpr_pread_vmo_page_fd = -1;
static void *lpr_pread_vmo_page;
static int lpr_pread_vmo_page_busy;

static int lpr_create_wire_page(void **out_page);
static void lpr_destroy_wire_page(int page_fd, void *page);
static int64_t lpr_filed_call(uint64_t op, int page_fd, uint64_t word2, uint64_t *out_result);
static int64_t lpr_filed_close_handle(uint64_t handle);

static void *lpr_session_payload_slot(uint64_t slot)
{
    if (lpr_session_page == 0 || slot >= FILED_WIRE_FAST_PAYLOAD_SLOT_COUNT) {
        return 0;
    }
    return (void *)((uintptr_t)lpr_session_page +
        FILED_WIRE_FAST_PAYLOAD_OFFSET +
        slot * FILED_WIRE_PAGE_BYTES);
}

static void lpr_zero_bytes(void *ptr, uint64_t len)
{
    unsigned char *p = (unsigned char *)ptr;
    while (len != 0) {
        *p++ = 0;
        len--;
    }
}

static char *lpr_clone_trace_append_literal(char *out, const char *end, const char *text)
{
    while (out < end && *text != 0) {
        *out++ = *text++;
    }
    return out;
}

static char *lpr_clone_trace_append_u64(char *out, const char *end, uint64_t value)
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

static void lpr_trace_clone_args(uint64_t flags, uint64_t child_stack, uint64_t parent_tid, uint64_t child_tid)
{
    char line[224];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_clone_trace_append_literal(out, end, "[lpr_runtime] clone flags=");
    out = lpr_clone_trace_append_u64(out, end, flags);
    out = lpr_clone_trace_append_literal(out, end, " child_stack=");
    out = lpr_clone_trace_append_u64(out, end, child_stack);
    out = lpr_clone_trace_append_literal(out, end, " parent_tid=");
    out = lpr_clone_trace_append_u64(out, end, parent_tid);
    out = lpr_clone_trace_append_literal(out, end, " child_tid=");
    out = lpr_clone_trace_append_u64(out, end, child_tid);
    out = lpr_clone_trace_append_literal(out, end, "\n");
    (void)lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, 2, (uint64_t)(uintptr_t)line, (uint64_t)(out - line));
}

#if LPR_TRACE_READV_SIZES || LPR_TRACE_READV_CACHE_STATS
static char *lpr_trace_append_literal(char *out, const char *end, const char *text)
{
    while (out < end && *text != 0) {
        *out++ = *text++;
    }
    return out;
}

static char *lpr_trace_append_u64(char *out, const char *end, uint64_t value)
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

#if LPR_TRACE_READV_SIZES
static void lpr_trace_readv_size(uint64_t fd, uint64_t iov_count, uint64_t requested, uint64_t coalesced, uint64_t offset)
{
    char line[224];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_trace_append_literal(out, end, "[lpr_runtime] readv_size fd=");
    out = lpr_trace_append_u64(out, end, fd);
    out = lpr_trace_append_literal(out, end, " iov=");
    out = lpr_trace_append_u64(out, end, iov_count);
    out = lpr_trace_append_literal(out, end, " requested=");
    out = lpr_trace_append_u64(out, end, requested);
    out = lpr_trace_append_literal(out, end, " coalesced=");
    out = lpr_trace_append_u64(out, end, coalesced);
    out = lpr_trace_append_literal(out, end, " offset=");
    out = lpr_trace_append_u64(out, end, offset);
    out = lpr_trace_append_literal(out, end, "\n");
    (void)lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, 2, (uint64_t)(uintptr_t)line, (uint64_t)(out - line));
}

static void lpr_trace_readv_to_vmo_status(uint64_t fd, uint64_t requested, int64_t status)
{
    char line[192];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_trace_append_literal(out, end, "[lpr_runtime] readv_to_vmo_status fd=");
    out = lpr_trace_append_u64(out, end, fd);
    out = lpr_trace_append_literal(out, end, " requested=");
    out = lpr_trace_append_u64(out, end, requested);
    out = lpr_trace_append_literal(out, end, " status=");
    if (status < 0) {
        out = lpr_trace_append_literal(out, end, "-");
        out = lpr_trace_append_u64(out, end, (uint64_t)(-status));
    } else {
        out = lpr_trace_append_u64(out, end, (uint64_t)status);
    }
    out = lpr_trace_append_literal(out, end, "\n");
    (void)lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, 2, (uint64_t)(uintptr_t)line, (uint64_t)(out - line));
}
#endif

#if LPR_TRACE_READV_CACHE_STATS
static uint64_t lpr_readv_cache_total;
static uint64_t lpr_readv_cache_coalesced;
static uint64_t lpr_readv_cache_hit;
static uint64_t lpr_readv_cache_fill;
static uint64_t lpr_readv_cache_fallback;
static uint64_t lpr_readv_cache_cross_page;
static uint64_t lpr_readv_cache_to_vmo;
static uint64_t lpr_readv_cache_bytes;

static char *lpr_trace_append_field_u64(char *out, const char *end, const char *name, uint64_t value)
{
    out = lpr_trace_append_literal(out, end, " ");
    out = lpr_trace_append_literal(out, end, name);
    out = lpr_trace_append_literal(out, end, "=");
    return lpr_trace_append_u64(out, end, value);
}

void lpr_linux_readv_cache_trace_dump(void)
{
    char line[384];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_trace_append_literal(out, end, "[lpr_runtime] metric scope=lpr_readv_cache");
    out = lpr_trace_append_field_u64(out, end, "total", lpr_readv_cache_total);
    out = lpr_trace_append_field_u64(out, end, "coalesced", lpr_readv_cache_coalesced);
    out = lpr_trace_append_field_u64(out, end, "hit", lpr_readv_cache_hit);
    out = lpr_trace_append_field_u64(out, end, "fill", lpr_readv_cache_fill);
    out = lpr_trace_append_field_u64(out, end, "fallback", lpr_readv_cache_fallback);
    out = lpr_trace_append_field_u64(out, end, "cross_page", lpr_readv_cache_cross_page);
    out = lpr_trace_append_field_u64(out, end, "to_vmo", lpr_readv_cache_to_vmo);
    out = lpr_trace_append_field_u64(out, end, "bytes", lpr_readv_cache_bytes);
    out = lpr_trace_append_literal(out, end, "\n");
    (void)lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, 2, (uint64_t)(uintptr_t)line, (uint64_t)(out - line));
}
#else
void lpr_linux_readv_cache_trace_dump(void)
{
}
#endif

static int64_t lpr_pacha_status_to_errno(int64_t status)
{
    if (status == 0) {
        return 0;
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

static int lpr_fd_is_filed(uint64_t fd)
{
    return fd < LPR_FD_TABLE_SIZE && lpr_fds[fd].active != 0;
}

static int lpr_fd_shadow_offset_eligible(uint64_t fd)
{
    return lpr_fd_is_filed(fd) &&
        (lpr_fds[fd].flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY;
}

int lpr_linux_filed_fd_active(uint64_t fd)
{
    return lpr_fd_is_filed(fd);
}

uint64_t lpr_linux_filed_fd_handle(uint64_t fd)
{
    return lpr_fd_is_filed(fd) ? lpr_fds[fd].handle : 0;
}

static int lpr_fd_alloc(uint64_t handle, uint64_t flags)
{
    for (uint64_t fd = 3; fd < LPR_FD_TABLE_SIZE; fd += 1) {
        if (fd == LPR_FILED_ENDPOINT_FD) {
            continue;
        }
        if (lpr_fds[fd].active == 0) {
            lpr_fds[fd].active = 1;
            lpr_fds[fd].offset_valid =
                ((flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY) ? 1u : 0u;
            lpr_fds[fd].pread_active = 0;
            lpr_fds[fd].flags = (uint32_t)flags;
            lpr_fds[fd].handle = handle;
            lpr_fds[fd].offset = 0;
            return (int)fd;
        }
    }
    return -LPR_LINUX_ENOMEM;
}

static int lpr_fd_slot_alloc(void)
{
    for (uint64_t fd = 3; fd < LPR_FD_TABLE_SIZE; fd += 1) {
        if (fd == LPR_FILED_ENDPOINT_FD) {
            continue;
        }
        if (lpr_fds[fd].active == 0 && lpr_pipe_fds[fd].active == 0) {
            return (int)fd;
        }
    }
    return -LPR_LINUX_ENOMEM;
}

static int lpr_pipe_fd_is_active(uint64_t fd)
{
    return fd < LPR_FD_TABLE_SIZE && lpr_pipe_fds[fd].active != 0;
}

static lpr_pipe_entry_t *lpr_pipe_for_fd(uint64_t fd)
{
    if (!lpr_pipe_fd_is_active(fd) || lpr_pipe_fds[fd].pipe_id >= LPR_LINUX_PIPE_COUNT) {
        return 0;
    }
    lpr_pipe_entry_t *pipe = lpr_pipes[lpr_pipe_fds[fd].pipe_id];
    return pipe->active != 0 ? pipe : 0;
}

static void lpr_pipe_close_fd(uint64_t fd)
{
    lpr_pipe_entry_t *pipe = lpr_pipe_for_fd(fd);
    if (pipe != 0) {
        if (lpr_pipe_fds[fd].readable && pipe->read_refs != 0) {
            pipe->read_refs--;
        }
        if (lpr_pipe_fds[fd].writable && pipe->write_refs != 0) {
            pipe->write_refs--;
        }
        if (pipe->read_refs == 0 && pipe->write_refs == 0) {
            const int vmo_fd = pipe->vmo_fd;
            lpr_memset(pipe, 0, sizeof(*pipe));
            lpr_pipes[lpr_pipe_fds[fd].pipe_id] = 0;
            (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)pipe, LPR_LINUX_PIPE_MAP_BYTES);
            if (vmo_fd >= 16) {
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
            }
        }
    }
    lpr_memset(&lpr_pipe_fds[fd], 0, sizeof(lpr_pipe_fds[fd]));
}

static void lpr_pipe_after_fork_child(void)
{
    for (uint64_t fd = 0; fd < LPR_FD_TABLE_SIZE; fd += 1) {
        if (!lpr_pipe_fd_is_active(fd)) {
            continue;
        }
        lpr_pipe_entry_t *pipe = lpr_pipe_for_fd(fd);
        if (pipe == 0) {
            continue;
        }
        if (lpr_pipe_fds[fd].readable && pipe->read_refs != UINT8_MAX) {
            pipe->read_refs++;
        }
        if (lpr_pipe_fds[fd].writable && pipe->write_refs != UINT8_MAX) {
            pipe->write_refs++;
        }
    }
}

static void lpr_close_cloexec_fds(void)
{
    for (uint64_t fd = 3; fd < LPR_FD_TABLE_SIZE; fd += 1) {
        if (lpr_pipe_fd_is_active(fd) &&
            (lpr_pipe_fds[fd].flags & LPR_LINUX_O_CLOEXEC) != 0)
        {
            lpr_pipe_close_fd(fd);
        }
        if (lpr_fd_is_filed(fd) &&
            (lpr_fds[fd].flags & LPR_LINUX_O_CLOEXEC) != 0)
        {
            const uint64_t handle = lpr_fds[fd].handle;
            lpr_memset(&lpr_fds[fd], 0, sizeof(lpr_fds[fd]));
            if (handle != 0) {
                (void)lpr_filed_close_handle(handle);
            }
        }
    }
}

int64_t lpr_linux_pipe2(uint64_t fds_raw, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_O_CLOEXEC | LPR_LINUX_O_NONBLOCK;
    if (fds_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t pipe_id = LPR_LINUX_PIPE_COUNT;
    for (uint64_t i = 0; i < LPR_LINUX_PIPE_COUNT; i += 1) {
        if (lpr_pipes[i] == 0 || lpr_pipes[i]->active == 0) {
            pipe_id = i;
            break;
        }
    }
    if (pipe_id == LPR_LINUX_PIPE_COUNT) {
        return -LPR_LINUX_ENOMEM;
    }
    const int read_fd = lpr_fd_slot_alloc();
    if (read_fd < 0) {
        return read_fd;
    }
    lpr_pipe_fds[read_fd].active = 1;
    const int write_fd = lpr_fd_slot_alloc();
    if (write_fd < 0) {
        lpr_memset(&lpr_pipe_fds[read_fd], 0, sizeof(lpr_pipe_fds[read_fd]));
        return write_fd;
    }

    const int64_t vmo_fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        LPR_LINUX_PIPE_MAP_BYTES,
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE,
        0);
    if (vmo_fd < 16) {
        lpr_memset(&lpr_pipe_fds[read_fd], 0, sizeof(lpr_pipe_fds[read_fd]));
        return lpr_pacha_status_to_errno(vmo_fd);
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)vmo_fd,
        0,
        LPR_LINUX_PIPE_MAP_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (mapped == 0 || (uint64_t)mapped > UINT64_MAX - LPR_LINUX_PIPE_MAP_BYTES) {
        lpr_memset(&lpr_pipe_fds[read_fd], 0, sizeof(lpr_pipe_fds[read_fd]));
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
        return mapped == 0 ? -LPR_LINUX_ENOMEM : lpr_pacha_status_to_errno(mapped);
    }
    lpr_pipe_entry_t *pipe = (lpr_pipe_entry_t *)(uintptr_t)mapped;
    lpr_memset(pipe, 0, sizeof(*pipe));
    pipe->active = 1;
    pipe->read_refs = 1;
    pipe->write_refs = 1;
    pipe->vmo_fd = (int32_t)vmo_fd;
    lpr_pipes[pipe_id] = pipe;
    lpr_pipe_fds[read_fd].active = 1;
    lpr_pipe_fds[read_fd].pipe_id = (uint8_t)pipe_id;
    lpr_pipe_fds[read_fd].readable = 1;
    lpr_pipe_fds[read_fd].flags = (uint32_t)flags;
    lpr_pipe_fds[write_fd].active = 1;
    lpr_pipe_fds[write_fd].pipe_id = (uint8_t)pipe_id;
    lpr_pipe_fds[write_fd].writable = 1;
    lpr_pipe_fds[write_fd].flags = (uint32_t)flags;

    int *fds = (int *)(uintptr_t)fds_raw;
    fds[0] = read_fd;
    fds[1] = write_fd;
    return 0;
}

int64_t lpr_linux_dup(uint64_t fd, uint64_t min_fd, uint64_t cloexec)
{
    (void)min_fd;
    if (lpr_pipe_fd_is_active(fd)) {
        const int dup_fd = lpr_fd_slot_alloc();
        if (dup_fd < 0) {
            return dup_fd;
        }
        lpr_pipe_fds[dup_fd] = lpr_pipe_fds[fd];
        if (cloexec) {
            lpr_pipe_fds[dup_fd].flags |= LPR_LINUX_O_CLOEXEC;
        }
        lpr_pipe_entry_t *pipe = lpr_pipe_for_fd(dup_fd);
        if (pipe != 0) {
            if (lpr_pipe_fds[dup_fd].readable) {
                pipe->read_refs++;
            }
            if (lpr_pipe_fds[dup_fd].writable) {
                pipe->write_refs++;
            }
        }
        return dup_fd;
    }
    if (lpr_fd_is_filed(fd)) {
        void *page = 0;
        const int page_fd = lpr_create_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        filed_wire_handle_flags_t *flags = (filed_wire_handle_flags_t *)page;
        lpr_memset(flags, 0, sizeof(*flags));
        flags->handle = lpr_fds[fd].handle;
        flags->fd_flags = cloexec ? FILED_WIRE_FD_CLOEXEC : 0;
        uint64_t dup_handle = 0;
        const int64_t status = lpr_filed_call(FILED_WIRE_OP_DUP, page_fd, 0, &dup_handle);
        lpr_destroy_wire_page(page_fd, page);
        if (status != 0) {
            return status;
        }
        const int dup_fd = lpr_fd_alloc(dup_handle, lpr_fds[fd].flags | (cloexec ? LPR_LINUX_O_CLOEXEC : 0));
        if (dup_fd < 0) {
            (void)lpr_filed_close_handle(dup_handle);
            return dup_fd;
        }
        return dup_fd;
    }
    return lpr_pacha_syscall4(PACHAOS_SYSCALL_FD_FCNTL, fd, PACHA_FD_FCNTL_DUP, 0, 0);
}

static void lpr_readlink_cache_clear(void)
{
    lpr_memset(lpr_readlink_cache, 0, sizeof(lpr_readlink_cache));
    lpr_readlink_cache_clock = 0;
}

static int lpr_readlink_cache_lookup(const char *path, uint64_t length, int64_t *out_status)
{
    if (path == 0 || out_status == 0 || length == 0 || length >= FILED_WIRE_PATH_BYTES) {
        return 0;
    }
    for (uint64_t i = 0; i < LPR_READLINK_CACHE_ENTRIES; i += 1) {
        const lpr_readlink_cache_entry_t *entry = &lpr_readlink_cache[i];
        if (entry->active &&
            entry->length == length &&
            lpr_memcmp(entry->path, path, (size_t)length) == 0)
        {
            *out_status = entry->status;
            return 1;
        }
    }
    return 0;
}

static void lpr_readlink_cache_store(const char *path, uint64_t length, int64_t status)
{
    if (path == 0 || length == 0 || length >= FILED_WIRE_PATH_BYTES || status >= 0) {
        return;
    }
    const uint64_t slot = lpr_readlink_cache_clock++ % LPR_READLINK_CACHE_ENTRIES;
    lpr_readlink_cache_entry_t *entry = &lpr_readlink_cache[slot];
    lpr_memset(entry, 0, sizeof(*entry));
    entry->active = 1;
    entry->length = (uint16_t)length;
    entry->status = status;
    lpr_memcpy(entry->path, path, (size_t)length);
    entry->path[length] = '\0';
}

static void lpr_page_cache_clear(void)
{
    lpr_memset(lpr_page_cache, 0, sizeof(lpr_page_cache));
    lpr_page_cache_clock = 0;
}

static void lpr_page_cache_invalidate_handle(uint64_t handle)
{
    if (handle == 0) {
        return;
    }
    for (uint64_t i = 0; i < LPR_FILED_PAGE_CACHE_ENTRIES; i += 1) {
        if (lpr_page_cache[i].active && lpr_page_cache[i].handle == handle) {
            lpr_memset(&lpr_page_cache[i], 0, offsetof(lpr_filed_page_cache_entry_t, data));
        }
    }
}

static int64_t lpr_filed_endpoint_ready(void)
{
    if (lpr_filed_endpoint_checked > 0) {
        return 0;
    }
    if (lpr_filed_endpoint_checked < 0) {
        return -LPR_LINUX_ENOSYS;
    }
    struct pacha_fd_info info;
    lpr_memset(&info, 0, sizeof(info));
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_FD_GET_INFO,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&info);
    if (status != 0 ||
        (info.kind != PACHA_FD_KIND_ENDPOINT && info.kind != PACHA_FD_KIND_CHANNEL))
    {
        lpr_filed_endpoint_checked = -1;
        return -LPR_LINUX_ENOSYS;
    }
    lpr_filed_endpoint_checked = 1;
    return 0;
}

static int64_t lpr_filed_session_connect(void)
{
    if (lpr_session_fd >= 16 &&
        lpr_session_page_fd >= 16 &&
        lpr_session_page != 0)
    {
        return 0;
    }
    if (lpr_session_checked < 0) {
        return -LPR_LINUX_ENOSYS;
    }
    const int64_t ready = lpr_filed_endpoint_ready();
    if (ready != 0) {
        lpr_session_checked = -1;
        return ready;
    }

    const uint64_t channel_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_SEND |
        PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_TRANSFER;
    const uint64_t page_rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    uint64_t pair[2] = {0, 0};
    int64_t status = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_IPC_CHANNEL_CREATE,
        (uint64_t)(uintptr_t)pair,
        channel_rights,
        PACHA_FD_FLAG_CLOEXEC);
    if (status != 0 || pair[0] < 16 || pair[1] < 16) {
        if (pair[0] >= 16) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        }
        if (pair[1] >= 16) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        }
        lpr_session_checked = -1;
        return status != 0 ? lpr_pacha_status_to_errno(status) : -LPR_LINUX_EINVAL;
    }

    const int64_t page_fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        FILED_WIRE_SESSION_PAGE_BYTES,
        page_rights,
        0);
    if (page_fd < 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        lpr_session_checked = -1;
        return lpr_pacha_status_to_errno(page_fd);
    }

    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)page_fd,
        0,
        FILED_WIRE_SESSION_PAGE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)page_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        lpr_session_checked = -1;
        return lpr_pacha_status_to_errno(mapped);
    }

    void *page = (void *)(uintptr_t)mapped;
    lpr_zero_bytes(page, FILED_WIRE_SESSION_PAGE_BYTES);
    filed_wire_fast_header_t *header = (filed_wire_fast_header_t *)page;
    header->magic = FILED_WIRE_FAST_MAGIC;
    header->version = FILED_WIRE_FAST_VERSION;
    header->request_capacity = FILED_WIRE_FAST_REQUEST_CAPACITY;
    header->completion_capacity = FILED_WIRE_FAST_COMPLETION_CAPACITY;
    header->payload_slot_count = FILED_WIRE_FAST_PAYLOAD_SLOT_COUNT;
    header->payload_slot_size = FILED_WIRE_PAGE_BYTES;
    header->payload_offset = FILED_WIRE_FAST_PAYLOAD_OFFSET;
    header->generation_offset = FILED_WIRE_FAST_GENERATION_OFFSET;
    header->generation_capacity = FILED_WIRE_FAST_GENERATION_CAPACITY;

    struct pacha_ipc_fd fds[2];
    struct pacha_ipc_msg request;
    struct pacha_ipc_msg reply;
    lpr_zero_bytes(fds, sizeof(fds));
    lpr_zero_bytes(&request, sizeof(request));
    lpr_zero_bytes(&reply, sizeof(reply));
    fds[0].fd = pair[1];
    fds[0].rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_SEND |
        PACHA_FD_RIGHT_RECV;
    fds[1].fd = (uint64_t)(uint32_t)page_fd;
    fds[1].rights = page_rights;
    request.word0 = FILED_WIRE_REQUEST_MAGIC;
    request.word1 = FILED_WIRE_OP_CONNECT;
    request.word3 = ++lpr_request_id;
    request.fds = fds;
    request.fd_count = 2;
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
    if (reply_fd < 16) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_WIRE_SESSION_PAGE_BYTES);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)page_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        lpr_session_checked = -1;
        return lpr_pacha_status_to_errno(reply_fd);
    }
    status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)reply_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    if (status != 0 ||
        reply.word0 != FILED_WIRE_REPLY_MAGIC ||
        reply.word1 != 0 ||
        reply.word3 != request.word3)
    {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_WIRE_SESSION_PAGE_BYTES);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)page_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        lpr_session_checked = -1;
        return status != 0 ? lpr_pacha_status_to_errno(status) : -LPR_LINUX_EIO;
    }

    lpr_session_fd = (int)(uint32_t)pair[0];
    lpr_session_page_fd = (int)(uint32_t)page_fd;
    lpr_session_page = page;
    lpr_session_checked = 1;
    return 0;
}

static int64_t lpr_filed_fast_call(uint64_t op, uint64_t word2, uint64_t *out_result)
{
    if (lpr_filed_session_connect() != 0 ||
        lpr_session_fd < 16 ||
        lpr_session_page == 0)
    {
        return -LPR_LINUX_ENOSYS;
    }
    filed_wire_fast_header_t *header = (filed_wire_fast_header_t *)lpr_session_page;
    if (header->magic != FILED_WIRE_FAST_MAGIC ||
        header->version != FILED_WIRE_FAST_VERSION ||
        header->request_capacity != FILED_WIRE_FAST_REQUEST_CAPACITY ||
        header->completion_capacity != FILED_WIRE_FAST_COMPLETION_CAPACITY ||
        header->payload_offset != FILED_WIRE_FAST_PAYLOAD_OFFSET ||
        header->generation_offset != FILED_WIRE_FAST_GENERATION_OFFSET ||
        header->generation_capacity != FILED_WIRE_FAST_GENERATION_CAPACITY)
    {
        return -LPR_LINUX_EIO;
    }
    if (header->request_tail - header->request_head >= header->request_capacity) {
        return -LPR_LINUX_EAGAIN;
    }

    filed_wire_fast_request_t *requests =
        (filed_wire_fast_request_t *)((uintptr_t)lpr_session_page + sizeof(*header));
    filed_wire_fast_completion_t *completions =
        (filed_wire_fast_completion_t *)((uintptr_t)requests +
            sizeof(*requests) * FILED_WIRE_FAST_REQUEST_CAPACITY);
    const uint64_t request_id = ++lpr_request_id;
    const uint64_t tail = header->request_tail;
    filed_wire_fast_request_t *fast_request =
        &requests[tail % header->request_capacity];
    lpr_zero_bytes(fast_request, sizeof(*fast_request));
    fast_request->request_id = request_id;
    fast_request->opcode = op;
    fast_request->word2 = word2;
    fast_request->payload_slot = 0;
    fast_request->payload_length = FILED_WIRE_PAGE_BYTES;
    __sync_synchronize();
    header->request_tail = tail + 1u;

    struct pacha_ipc_msg request;
    struct pacha_ipc_msg reply;
    lpr_zero_bytes(&request, sizeof(request));
    lpr_zero_bytes(&reply, sizeof(reply));
    request.word0 = FILED_WIRE_REQUEST_MAGIC;
    request.word1 = FILED_WIRE_OP_FAST_DOORBELL;
    request.word2 = ++header->doorbell_seq;
    request.word3 = request_id;
    int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_SEND,
        (uint64_t)(uint32_t)lpr_session_fd,
        (uint64_t)(uintptr_t)&request);
    if (status != 0) {
        return lpr_pacha_status_to_errno(status);
    }
    status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)lpr_session_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    if (status != 0) {
        return lpr_pacha_status_to_errno(status);
    }
    if (reply.word0 != FILED_WIRE_REPLY_MAGIC ||
        reply.word1 != 0 ||
        reply.word3 != request_id)
    {
        return -LPR_LINUX_EIO;
    }
    if (header->completion_head == header->completion_tail) {
        return -LPR_LINUX_EIO;
    }
    __sync_synchronize();
    filed_wire_fast_completion_t *completion =
        &completions[header->completion_head % header->completion_capacity];
    if (completion->request_id != request_id) {
        return -LPR_LINUX_EIO;
    }
    header->completion_head++;
    if (completion->status < 0) {
        return completion->status;
    }
    if (out_result != 0) {
        *out_result = completion->result;
    }
    return 0;
}

static int lpr_create_wire_page(void **out_page)
{
    if (out_page == 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_filed_session_connect() == 0 &&
        lpr_session_page_fd >= 16 &&
        lpr_session_page != 0 &&
        !lpr_session_payload_busy)
    {
        void *payload = lpr_session_payload_slot(0);
        if (payload != 0) {
            lpr_session_payload_busy = 1;
            *out_page = payload;
            return lpr_session_page_fd;
        }
    }
    if (lpr_wire_page_fd >= 16 && lpr_wire_page != 0 && !lpr_wire_page_busy) {
        lpr_wire_page_busy = 1;
        *out_page = lpr_wire_page;
        return lpr_wire_page_fd;
    }
    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        FILED_WIRE_PAGE_BYTES,
        rights,
        0);
    if (fd < 16) {
        return (int)lpr_pacha_status_to_errno(fd);
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)fd,
        0,
        FILED_WIRE_PAGE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
        return (int)lpr_pacha_status_to_errno(mapped);
    }
    *out_page = (void *)(uintptr_t)mapped;
    if (!lpr_wire_page_busy && lpr_wire_page_fd < 16 && lpr_wire_page == 0) {
        lpr_wire_page_fd = (int)fd;
        lpr_wire_page = *out_page;
        lpr_wire_page_busy = 1;
    }
    return (int)fd;
}

static void lpr_destroy_wire_page(int fd, void *page)
{
    if (fd == lpr_session_page_fd && page == lpr_session_payload_slot(0)) {
        lpr_session_payload_busy = 0;
        return;
    }
    if (fd == lpr_wire_page_fd && page == lpr_wire_page) {
        lpr_wire_page_busy = 0;
        return;
    }
    if (page != 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_WIRE_PAGE_BYTES);
    }
    if (fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
    }
}

static int lpr_create_standalone_wire_page(void **out_page)
{
    if (out_page == 0) {
        return -LPR_LINUX_EINVAL;
    }
    *out_page = 0;
    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        FILED_WIRE_PAGE_BYTES,
        rights,
        0);
    if (fd < 16) {
        return (int)lpr_pacha_status_to_errno(fd);
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)fd,
        0,
        FILED_WIRE_PAGE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
        return (int)lpr_pacha_status_to_errno(mapped);
    }
    *out_page = (void *)(uintptr_t)mapped;
    return (int)fd;
}

static void lpr_destroy_standalone_wire_page(int fd, void *page)
{
    if (page != 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, FILED_WIRE_PAGE_BYTES);
    }
    if (fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
    }
}

static int lpr_create_pread_vmo_wire_page(void **out_page)
{
    if (out_page == 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_pread_vmo_page_fd >= 16 &&
        lpr_pread_vmo_page != 0 &&
        !lpr_pread_vmo_page_busy)
    {
        lpr_pread_vmo_page_busy = 1;
        *out_page = lpr_pread_vmo_page;
        return lpr_pread_vmo_page_fd;
    }
    if (lpr_pread_vmo_page_busy) {
        return lpr_create_standalone_wire_page(out_page);
    }

    void *page = 0;
    const int fd = lpr_create_standalone_wire_page(&page);
    if (fd < 0) {
        return fd;
    }
    lpr_pread_vmo_page_fd = fd;
    lpr_pread_vmo_page = page;
    lpr_pread_vmo_page_busy = 1;
    *out_page = page;
    return fd;
}

static void lpr_destroy_pread_vmo_wire_page(int fd, void *page)
{
    if (fd == lpr_pread_vmo_page_fd && page == lpr_pread_vmo_page) {
        lpr_pread_vmo_page_busy = 0;
        return;
    }
    lpr_destroy_standalone_wire_page(fd, page);
}

static uint64_t lpr_page_align_up(uint64_t value)
{
    const uint64_t mask = 4095ull;
    if (value > UINT64_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

static int64_t lpr_readv_scratch_vmo(uint64_t requested, int *out_fd, unsigned char **out_map, uint64_t *out_len)
{
    if (out_fd == 0 || out_map == 0 || out_len == 0) {
        return -LPR_LINUX_EINVAL;
    }
    *out_fd = -1;
    *out_map = 0;
    *out_len = 0;

    const uint64_t map_len = lpr_page_align_up(requested);
    if (map_len == 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_readv_vmo_fd >= 16 &&
        lpr_readv_vmo_map != 0 &&
        lpr_readv_vmo_len >= map_len)
    {
        *out_fd = lpr_readv_vmo_fd;
        *out_map = (unsigned char *)lpr_readv_vmo_map;
        *out_len = lpr_readv_vmo_len;
        return 0;
    }

    if (lpr_readv_vmo_map != 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)lpr_readv_vmo_map, lpr_readv_vmo_len);
        lpr_readv_vmo_map = 0;
        lpr_readv_vmo_len = 0;
    }
    if (lpr_readv_vmo_fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)lpr_readv_vmo_fd);
        lpr_readv_vmo_fd = -1;
    }

    const uint64_t rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t vmo_fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        map_len,
        rights,
        0);
    if (vmo_fd < 16) {
        return lpr_pacha_status_to_errno(vmo_fd);
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)vmo_fd,
        0,
        map_len,
        PACHAOS_PROT_READ,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)vmo_fd);
        return lpr_pacha_status_to_errno(mapped);
    }

    lpr_readv_vmo_fd = (int)(uint32_t)vmo_fd;
    lpr_readv_vmo_map = (void *)(uintptr_t)mapped;
    lpr_readv_vmo_len = map_len;
    *out_fd = lpr_readv_vmo_fd;
    *out_map = (unsigned char *)lpr_readv_vmo_map;
    *out_len = lpr_readv_vmo_len;
    return 0;
}

static uint64_t lpr_scatter_iov(
    const lpr_linux_iovec_t *iov,
    uint64_t iov_count,
    const unsigned char *src,
    uint64_t length)
{
    uint64_t copied = 0;
    for (uint64_t i = 0; i < iov_count && copied < length; i += 1) {
        if (iov[i].len == 0) {
            continue;
        }
        uint64_t chunk = iov[i].len;
        if (chunk > length - copied) {
            chunk = length - copied;
        }
        lpr_memcpy((void *)(uintptr_t)iov[i].base, src + copied, (size_t)chunk);
        copied += chunk;
    }
    return copied;
}

static lpr_filed_page_cache_entry_t *lpr_page_cache_lookup(
    uint64_t handle,
    uint64_t offset,
    uint64_t requested)
{
    if (handle == 0 || requested == 0 || requested > LPR_FILED_PAGE_CACHE_BYTES) {
        return 0;
    }
    const uint64_t page_start = offset & ~(LPR_FILED_PAGE_CACHE_BYTES - 1ull);
    if (offset < page_start || requested > LPR_FILED_PAGE_CACHE_BYTES - (offset - page_start)) {
        return 0;
    }
    for (uint64_t i = 0; i < LPR_FILED_PAGE_CACHE_ENTRIES; i += 1) {
        lpr_filed_page_cache_entry_t *entry = &lpr_page_cache[i];
        if (!entry->active ||
            entry->handle != handle ||
            entry->page_start != page_start)
        {
            continue;
        }
        if (entry->length == 0) {
            return 0;
        }
        if (offset + requested < offset ||
            offset + requested > entry->page_start + entry->length)
        {
            continue;
        }
        entry->clock = ++lpr_page_cache_clock;
        return entry;
    }
    return 0;
}

static lpr_filed_page_cache_entry_t *lpr_page_cache_find_marker(uint64_t handle, uint64_t page_start)
{
    if (handle == 0) {
        return 0;
    }
    for (uint64_t i = 0; i < LPR_FILED_PAGE_CACHE_ENTRIES; i += 1) {
        lpr_filed_page_cache_entry_t *entry = &lpr_page_cache[i];
        if (entry->active &&
            entry->handle == handle &&
            entry->page_start == page_start &&
            entry->length == 0)
        {
            entry->clock = ++lpr_page_cache_clock;
            return entry;
        }
    }
    return 0;
}

static lpr_filed_page_cache_entry_t *lpr_page_cache_slot(void)
{
    uint64_t slot = 0;
    uint64_t oldest = UINT64_MAX;
    for (uint64_t i = 0; i < LPR_FILED_PAGE_CACHE_ENTRIES; i += 1) {
        if (!lpr_page_cache[i].active) {
            return &lpr_page_cache[i];
        }
        if (lpr_page_cache[i].clock < oldest) {
            oldest = lpr_page_cache[i].clock;
            slot = i;
        }
    }
    return &lpr_page_cache[slot];
}

static int64_t lpr_filed_call(uint64_t op, int page_fd, uint64_t word2, uint64_t *out_result)
{
    if (page_fd == lpr_session_page_fd && lpr_session_page != 0) {
        return lpr_filed_fast_call(op, word2, out_result);
    } else if (page_fd < 16 &&
        lpr_session_page != 0 &&
        !lpr_session_payload_busy)
    {
        const int64_t fast_status = lpr_filed_fast_call(op, word2, out_result);
        if (fast_status != -LPR_LINUX_ENOSYS && fast_status != -LPR_LINUX_EAGAIN) {
            return fast_status;
        }
    }

    const int64_t ready = lpr_filed_endpoint_ready();
    if (ready != 0) {
        return ready;
    }

    struct pacha_ipc_fd fd_item;
    lpr_memset(&fd_item, 0, sizeof(fd_item));
    if (page_fd >= 16) {
        fd_item.fd = (uint64_t)(uint32_t)page_fd;
        fd_item.rights =
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE;
    }

    const uint64_t request_id = ++lpr_request_id;
    const struct pacha_ipc_msg request = {
        .word0 = FILED_WIRE_REQUEST_MAGIC,
        .word1 = op,
        .word2 = word2,
        .word3 = request_id,
        .fds = page_fd >= 16 ? &fd_item : 0,
        .fd_count = page_fd >= 16 ? 1u : 0u,
    };
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16) {
        return lpr_pacha_status_to_errno(reply_fd);
    }

    struct pacha_ipc_msg reply;
    lpr_memset(&reply, 0, sizeof(reply));
    const int64_t recv_status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)reply_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    if (recv_status != 0) {
        return lpr_pacha_status_to_errno(recv_status);
    }
    if (reply.word0 != FILED_WIRE_REPLY_MAGIC || reply.word3 != request_id) {
        return -LPR_LINUX_EIO;
    }
    if ((int64_t)reply.word1 < 0) {
        return (int64_t)reply.word1;
    }
    if (out_result != 0) {
        *out_result = reply.word2;
    }
    return 0;
}

static uint64_t lpr_open_rights(uint64_t flags)
{
    uint64_t rights = FILED_WIRE_RIGHT_STAT;
    const uint64_t accmode = flags & LPR_LINUX_O_ACCMODE;
    if (accmode != LPR_LINUX_O_WRONLY) {
        rights |= FILED_WIRE_RIGHT_READ | FILED_WIRE_RIGHT_GETDENTS;
    }
    if (accmode == LPR_LINUX_O_WRONLY || accmode == LPR_LINUX_O_RDWR) {
        rights |= FILED_WIRE_RIGHT_WRITE;
    }
    if ((flags & LPR_LINUX_O_DIRECTORY) != 0) {
        rights |=
            FILED_WIRE_RIGHT_LOOKUP |
            FILED_WIRE_RIGHT_GETDENTS |
            FILED_WIRE_RIGHT_CREATE |
            FILED_WIRE_RIGHT_REMOVE |
            FILED_WIRE_RIGHT_RENAME;
    }
    if ((flags & LPR_LINUX_O_CREAT) != 0) {
        rights |= FILED_WIRE_RIGHT_CREATE | FILED_WIRE_RIGHT_WRITE;
    }
    return rights;
}

static uint64_t lpr_open_flags(uint64_t flags)
{
    uint64_t out = 0;
    if ((flags & LPR_LINUX_O_CREAT) != 0) {
        out |= FILED_WIRE_OPEN_CREATE;
    }
    if ((flags & LPR_LINUX_O_EXCL) != 0) {
        out |= FILED_WIRE_OPEN_EXCLUSIVE;
    }
    if ((flags & LPR_LINUX_O_TRUNC) != 0) {
        out |= FILED_WIRE_OPEN_TRUNCATE;
    }
    if ((flags & LPR_LINUX_O_DIRECTORY) != 0) {
        out |= FILED_WIRE_OPEN_DIRECTORY;
    }
    if ((flags & LPR_LINUX_O_NOFOLLOW) != 0) {
        out |= FILED_WIRE_OPEN_NOFOLLOW;
    }
    if ((flags & LPR_LINUX_O_CLOEXEC) != 0) {
        out |= FILED_WIRE_OPEN_CLOEXEC;
    }
    if ((flags & LPR_LINUX_O_APPEND) != 0) {
        out |= FILED_WIRE_OPEN_APPEND;
    }
    if ((flags & LPR_LINUX_O_NONBLOCK) != 0) {
        out |= FILED_WIRE_OPEN_NONBLOCK;
    }
    return out;
}

static int64_t lpr_copy_path(char *out, uint64_t capacity, const char *path)
{
    if (out == 0 || path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (capacity == 0) {
        return -LPR_LINUX_EINVAL;
    }
    const size_t len = lpr_strnlen(path, capacity);
    if (len == capacity) {
        return -LPR_LINUX_ENAMETOOLONG;
    }
    lpr_memset(out, 0, capacity);
    lpr_memcpy(out, path, len + 1u);
    return 0;
}

static int64_t lpr_dir_handle_for(uint64_t dirfd, const char *path, uint64_t *out)
{
    if (out == 0 || path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (path[0] == '/' || (int64_t)dirfd == LPR_LINUX_AT_FDCWD) {
        *out = 0;
        return 0;
    }
    if (!lpr_fd_is_filed(dirfd)) {
        return -LPR_LINUX_EBADF;
    }
    *out = lpr_fds[dirfd].handle;
    return 0;
}

static int64_t lpr_filed_close_handle(uint64_t handle)
{
    uint64_t ignored = 0;
    return lpr_filed_call(FILED_WIRE_OP_CLOSE, -1, handle, &ignored);
}

int64_t lpr_linux_fsync(uint64_t fd)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t ignored = 0;
    return lpr_filed_call(FILED_WIRE_OP_FSYNC, -1, lpr_fds[fd].handle, &ignored);
}

int64_t lpr_linux_mkdirat(uint64_t dirfd, uint64_t path_raw, uint64_t mode)
{
    lpr_readlink_cache_clear();
    lpr_page_cache_clear();
    void *page = 0;
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t dir_handle = 0;
    int64_t status = lpr_dir_handle_for(dirfd, path, &dir_handle);
    if (status != 0) {
        return status;
    }
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_mkdir_t *mkdir_req = (filed_wire_mkdir_t *)page;
    lpr_memset(mkdir_req, 0, sizeof(*mkdir_req));
    mkdir_req->dir_handle = dir_handle;
    mkdir_req->mode = mode;
    status = lpr_copy_path(mkdir_req->name, sizeof(mkdir_req->name), path);
    uint64_t ignored = 0;
    if (status == 0) {
        status = lpr_filed_call(FILED_WIRE_OP_MKDIR, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_unlinkat(uint64_t dirfd, uint64_t path_raw, uint64_t flags)
{
    lpr_readlink_cache_clear();
    lpr_page_cache_clear();
    const uint64_t known_flags = LPR_LINUX_AT_REMOVEDIR;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    void *page = 0;
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t dir_handle = 0;
    int64_t status = lpr_dir_handle_for(dirfd, path, &dir_handle);
    if (status != 0) {
        return status;
    }
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    uint64_t op = FILED_WIRE_OP_UNLINK;
    if ((flags & LPR_LINUX_AT_REMOVEDIR) != 0) {
        filed_wire_rmdir_t *rmdir_req = (filed_wire_rmdir_t *)page;
        lpr_memset(rmdir_req, 0, sizeof(*rmdir_req));
        rmdir_req->dir_handle = dir_handle;
        status = lpr_copy_path(rmdir_req->name, sizeof(rmdir_req->name), path);
        op = FILED_WIRE_OP_RMDIR;
    } else {
        filed_wire_unlink_t *unlink_req = (filed_wire_unlink_t *)page;
        lpr_memset(unlink_req, 0, sizeof(*unlink_req));
        unlink_req->dir_handle = dir_handle;
        status = lpr_copy_path(unlink_req->name, sizeof(unlink_req->name), path);
    }
    uint64_t ignored = 0;
    if (status == 0) {
        status = lpr_filed_call(op, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_renameat(uint64_t old_dirfd, uint64_t old_path_raw, uint64_t new_dirfd, uint64_t new_path_raw)
{
    lpr_readlink_cache_clear();
    lpr_page_cache_clear();
    void *page = 0;
    const char *old_path = (const char *)(uintptr_t)old_path_raw;
    const char *new_path = (const char *)(uintptr_t)new_path_raw;
    uint64_t old_dir_handle = 0;
    uint64_t new_dir_handle = 0;
    int64_t status = lpr_dir_handle_for(old_dirfd, old_path, &old_dir_handle);
    if (status != 0) {
        return status;
    }
    status = lpr_dir_handle_for(new_dirfd, new_path, &new_dir_handle);
    if (status != 0) {
        return status;
    }
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_rename_t *rename_req = (filed_wire_rename_t *)page;
    lpr_memset(rename_req, 0, sizeof(*rename_req));
    rename_req->old_dir_handle = old_dir_handle;
    rename_req->new_dir_handle = new_dir_handle;
    status = lpr_copy_path(rename_req->old_name, sizeof(rename_req->old_name), old_path);
    if (status == 0) {
        status = lpr_copy_path(rename_req->new_name, sizeof(rename_req->new_name), new_path);
    }
    uint64_t ignored = 0;
    if (status == 0) {
        status = lpr_filed_call(FILED_WIRE_OP_RENAME, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_fchownat(uint64_t dirfd, uint64_t path, uint64_t owner, uint64_t group, uint64_t flags)
{
    (void)owner;
    (void)group;
    const uint64_t known_flags = LPR_LINUX_AT_SYMLINK_NOFOLLOW | LPR_LINUX_AT_EMPTY_PATH;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    if ((flags & LPR_LINUX_AT_EMPTY_PATH) != 0 && path != 0 && ((const char *)(uintptr_t)path)[0] == 0) {
        return lpr_fd_is_filed(dirfd) || lpr_pipe_fd_is_active(dirfd) ? 0 : -LPR_LINUX_EBADF;
    }
    if ((flags & LPR_LINUX_AT_SYMLINK_NOFOLLOW) != 0) {
        return path != 0 ? 0 : -LPR_LINUX_EFAULT;
    }
    return lpr_linux_faccessat(dirfd, path, 0, flags & LPR_LINUX_AT_SYMLINK_NOFOLLOW);
}

int64_t lpr_linux_mknodat(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t dev)
{
    (void)mode;
    (void)dev;
    return lpr_linux_faccessat(dirfd, path, 0, 0) == 0 ? -LPR_LINUX_EEXIST : 0;
}

static int64_t lpr_linux_readlinkat_to_buffer(uint64_t dirfd, uint64_t path_raw, char *target, uint64_t capacity);
static int lpr_resolve_final_symlink_path(const char *path, const char *target, char *out, uint64_t capacity);

int64_t lpr_linux_symlinkat(uint64_t target_raw, uint64_t new_dirfd, uint64_t linkpath_raw)
{
    const char *target = (const char *)(uintptr_t)target_raw;
    const char *linkpath = (const char *)(uintptr_t)linkpath_raw;
    if (target == 0 || linkpath == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_readlink_cache_clear();
    lpr_page_cache_clear();
    uint64_t dir_handle = 0;
    int64_t status = lpr_dir_handle_for(new_dirfd, linkpath, &dir_handle);
    if (status != 0) {
        return status;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_symlink_t *symlink_req = (filed_wire_symlink_t *)page;
    lpr_memset(symlink_req, 0, sizeof(*symlink_req));
    symlink_req->dir_handle = dir_handle;
    status = lpr_copy_path(symlink_req->name, sizeof(symlink_req->name), linkpath);
    const uint64_t target_len = (uint64_t)lpr_strnlen(target, FILED_WIRE_SYMLINK_TARGET_BYTES);
    if (status == 0 && (target_len == 0 || target_len >= FILED_WIRE_SYMLINK_TARGET_BYTES)) {
        status = target_len == 0 ? -LPR_LINUX_EINVAL : -LPR_LINUX_ENAMETOOLONG;
    }
    if (status == 0) {
        symlink_req->target_length = target_len;
        lpr_memcpy(symlink_req->target, target, target_len + 1u);
        uint64_t ignored = 0;
        status = lpr_filed_call(FILED_WIRE_OP_SYMLINK, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_linkat(uint64_t old_dirfd, uint64_t old_path_raw, uint64_t new_dirfd, uint64_t new_path_raw, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_AT_SYMLINK_FOLLOW;
    const char *old_path = (const char *)(uintptr_t)old_path_raw;
    const char *new_path = (const char *)(uintptr_t)new_path_raw;
    char followed_old_path[FILED_WIRE_PATH_BYTES];
    if (old_path == 0 || new_path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    if ((flags & LPR_LINUX_AT_SYMLINK_FOLLOW) != 0) {
        char target[FILED_WIRE_SYMLINK_TARGET_BYTES];
        lpr_memset(target, 0, sizeof(target));
        const int64_t len = lpr_linux_readlinkat_to_buffer(old_dirfd, old_path_raw, target, sizeof(target) - 1u);
        if (len > 0) {
            target[(uint64_t)len < sizeof(target) ? (uint64_t)len : sizeof(target) - 1u] = 0;
            if (!lpr_resolve_final_symlink_path(old_path, target, followed_old_path, sizeof(followed_old_path))) {
                return -LPR_LINUX_ENAMETOOLONG;
            }
            old_dirfd = LPR_LINUX_AT_FDCWD;
            old_path_raw = (uint64_t)(uintptr_t)followed_old_path;
            old_path = followed_old_path;
        } else if (len != -LPR_LINUX_EINVAL) {
            return len == 0 ? -LPR_LINUX_EINVAL : len;
        }
    }
    lpr_readlink_cache_clear();
    lpr_page_cache_clear();

    uint64_t old_dir_handle = 0;
    int64_t status = lpr_dir_handle_for(old_dirfd, old_path, &old_dir_handle);
    if (status != 0) {
        return status;
    }
    uint64_t new_dir_handle = 0;
    status = lpr_dir_handle_for(new_dirfd, new_path, &new_dir_handle);
    if (status != 0) {
        return status;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_link_t *link_req = (filed_wire_link_t *)page;
    lpr_memset(link_req, 0, sizeof(*link_req));
    link_req->old_dir_handle = old_dir_handle;
    link_req->new_dir_handle = new_dir_handle;
    link_req->flags = flags;
    status = lpr_copy_path(link_req->old_name, sizeof(link_req->old_name), old_path);
    if (status == 0) {
        status = lpr_copy_path(link_req->new_name, sizeof(link_req->new_name), new_path);
    }
    if (status == 0) {
        uint64_t ignored = 0;
        status = lpr_filed_call(FILED_WIRE_OP_LINK, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

static int64_t lpr_linux_openat_once(uint64_t dirfd, uint64_t path_raw, uint64_t flags, uint64_t mode)
{
    (void)mode;
    if ((flags & (LPR_LINUX_O_CREAT | LPR_LINUX_O_TRUNC)) != 0) {
        lpr_readlink_cache_clear();
        lpr_page_cache_clear();
    }
    void *page = 0;
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t dir_handle = 0;
    int64_t status = lpr_dir_handle_for(dirfd, path, &dir_handle);
    if (status != 0) {
        return status;
    }
    status = lpr_filed_endpoint_ready();
    if (status != 0) {
        return status;
    }
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }

    filed_wire_openat_t *open_req = (filed_wire_openat_t *)page;
    lpr_memset(open_req, 0, sizeof(*open_req));
    open_req->dir_handle = dir_handle;
    open_req->rights = lpr_open_rights(flags);
    open_req->open_flags = lpr_open_flags(flags);
    status = lpr_copy_path(open_req->name, sizeof(open_req->name), path);
    uint64_t handle = 0;
    if (status == 0) {
        status = lpr_filed_call(FILED_WIRE_OP_OPENAT, page_fd, 0, &handle);
    }
    lpr_destroy_wire_page(page_fd, page);
    if (status != 0) {
        return status;
    }
    const int fd = lpr_fd_alloc(handle, flags);
    if (fd < 0) {
        (void)lpr_filed_close_handle(handle);
        return fd;
    }
    return fd;
}

static int64_t lpr_linux_readlinkat_to_buffer(uint64_t dirfd, uint64_t path_raw, char *target, uint64_t capacity)
{
    if (target == 0 || capacity == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const char *path = (const char *)(uintptr_t)path_raw;
    uint64_t dir_handle = 0;
    int64_t status = lpr_dir_handle_for(dirfd, path, &dir_handle);
    if (status != 0) {
        return status;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_readlink_t *readlink_req = (filed_wire_readlink_t *)page;
    lpr_memset(readlink_req, 0, sizeof(*readlink_req));
    readlink_req->dir_handle = dir_handle;
    status = lpr_copy_path(readlink_req->name, sizeof(readlink_req->name), path);
    uint64_t length = 0;
    if (status == 0) {
        status = lpr_filed_call(FILED_WIRE_OP_READLINK, page_fd, 0, &length);
    }
    if (status == 0) {
        if (length > capacity) {
            length = capacity;
        }
        lpr_memcpy(target, readlink_req->target, length);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status == 0 ? (int64_t)length : status;
}

static int lpr_resolve_final_symlink_path(const char *path, const char *target, char *out, uint64_t capacity)
{
    if (path == 0 || target == 0 || out == 0) {
        return 0;
    }
    if (capacity == 0) {
        return 0;
    }
    const uint64_t target_len = (uint64_t)lpr_strnlen(target, capacity);
    if (target_len == 0 || target_len >= capacity) {
        return 0;
    }
    lpr_memset(out, 0, capacity);
    if (target[0] == '/') {
        lpr_memcpy(out, target, target_len + 1u);
        return 1;
    }
    uint64_t prefix_len = 0;
    for (uint64_t i = 0; path[i] != 0 && i < capacity; i += 1) {
        if (path[i] == '/') {
            prefix_len = i + 1u;
        }
    }
    if (prefix_len + target_len >= capacity) {
        return 0;
    }
    if (prefix_len != 0) {
        lpr_memcpy(out, path, prefix_len);
    }
    lpr_memcpy(out + prefix_len, target, target_len + 1u);
    return 1;
}

int64_t lpr_linux_openat(uint64_t dirfd, uint64_t path_raw, uint64_t flags, uint64_t mode)
{
    int64_t fd = lpr_linux_openat_once(dirfd, path_raw, flags, mode);
    if (fd < 0 ||
        (flags & (LPR_LINUX_O_NOFOLLOW | LPR_LINUX_O_CREAT | LPR_LINUX_O_TRUNC)) != 0)
    {
        return fd;
    }
    lpr_linux_stat_t st;
    const int64_t stat_status = lpr_linux_fstat((uint64_t)fd, (uint64_t)(uintptr_t)&st);
    if (stat_status != 0 ||
        (((uint64_t)st.st_mode & LPR_LINUX_S_IFMT) != LPR_LINUX_S_IFLNK))
    {
        return fd;
    }
    char target[FILED_WIRE_SYMLINK_TARGET_BYTES];
    lpr_memset(target, 0, sizeof(target));
    const int64_t len = lpr_linux_readlinkat_to_buffer(dirfd, path_raw, target, sizeof(target) - 1u);
    (void)lpr_linux_close((uint64_t)fd);
    if (len <= 0) {
        return len == 0 ? -LPR_LINUX_EINVAL : len;
    }
    target[len < (int64_t)(sizeof(target) - 1u) ? (uint64_t)len : sizeof(target) - 1u] = 0;
    const char *path = (const char *)(uintptr_t)path_raw;
    char resolved[FILED_WIRE_PATH_BYTES];
    if (!lpr_resolve_final_symlink_path(path, target, resolved, sizeof(resolved))) {
        return -LPR_LINUX_ENAMETOOLONG;
    }
    return lpr_linux_openat_once(dirfd, (uint64_t)(uintptr_t)resolved, flags, mode);
}

static int64_t lpr_filed_io(uint64_t op, uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (buf == 0 && count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_io_t *io = (filed_wire_io_t *)page;
    lpr_memset(io, 0, sizeof(*io));
    io->handle = lpr_fds[fd].handle;
    io->offset = offset;
    io->length = count > FILED_WIRE_IO_BYTES ? FILED_WIRE_IO_BYTES : count;
    if (op == FILED_WIRE_OP_WRITE && io->length != 0) {
        lpr_memcpy(io->data, (const void *)(uintptr_t)buf, (size_t)io->length);
    }
    uint64_t result = 0;
    const int64_t status = lpr_filed_call(op, page_fd, 0, &result);
    if (status == 0 && op != FILED_WIRE_OP_WRITE && result != 0) {
        lpr_memcpy((void *)(uintptr_t)buf, io->data, (size_t)result);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status == 0 ? (int64_t)result : status;
}

static lpr_filed_page_cache_entry_t *lpr_page_cache_fill(uint64_t fd, uint64_t offset, uint64_t requested)
{
    if (!lpr_fd_shadow_offset_eligible(fd) ||
        requested == 0 ||
        requested > LPR_FILED_PAGE_CACHE_BYTES)
    {
        return 0;
    }
    const uint64_t page_start = offset & ~(LPR_FILED_PAGE_CACHE_BYTES - 1ull);
    if (requested > LPR_FILED_PAGE_CACHE_BYTES - (offset - page_start)) {
        return 0;
    }
    lpr_filed_page_cache_entry_t *entry =
        lpr_page_cache_find_marker(lpr_fds[fd].handle, page_start);
    if (entry == 0) {
        entry = lpr_page_cache_slot();
    }
    const int64_t n = lpr_filed_io(
        FILED_WIRE_OP_PREAD,
        fd,
        (uint64_t)(uintptr_t)entry->data,
        LPR_FILED_PAGE_CACHE_BYTES,
        page_start);
    if (n <= 0) {
        return 0;
    }
    lpr_memset(entry, 0, offsetof(lpr_filed_page_cache_entry_t, data));
    entry->active = 1;
    entry->handle = lpr_fds[fd].handle;
    entry->page_start = page_start;
    entry->length = (uint64_t)n;
    entry->clock = ++lpr_page_cache_clock;
    if (offset + requested < offset ||
        offset + requested > entry->page_start + entry->length)
    {
        return 0;
    }
    return entry;
}

static lpr_filed_page_cache_entry_t *lpr_page_cache_get(uint64_t fd, uint64_t offset, uint64_t requested, int *out_hit)
{
    lpr_filed_page_cache_entry_t *entry =
        lpr_page_cache_lookup(lpr_fds[fd].handle, offset, requested);
    if (entry != 0) {
        if (out_hit != 0) {
            *out_hit = 1;
        }
        return entry;
    }
    if (out_hit != 0) {
        *out_hit = 0;
    }
    return lpr_page_cache_fill(fd, offset, requested);
}

static int64_t lpr_read_from_page_cache(uint64_t fd, uint64_t buf, uint64_t requested, uint64_t offset)
{
    if (requested == 0) {
        return 0;
    }
    if (buf == 0 || requested > LPR_FILED_PAGE_CACHE_BYTES) {
        return -1;
    }
    const uint64_t page_start = offset & ~(LPR_FILED_PAGE_CACHE_BYTES - 1ull);
    const uint64_t page_offset = offset - page_start;
    if (requested <= LPR_FILED_PAGE_CACHE_BYTES - page_offset) {
        lpr_filed_page_cache_entry_t *entry = lpr_page_cache_get(fd, offset, requested, 0);
        if (entry == 0) {
            return -1;
        }
        lpr_memcpy((void *)(uintptr_t)buf, entry->data + page_offset, (size_t)requested);
        return (int64_t)requested;
    }

    const uint64_t first_len = LPR_FILED_PAGE_CACHE_BYTES - page_offset;
    const uint64_t second_len = requested - first_len;
    lpr_filed_page_cache_entry_t *first = lpr_page_cache_get(fd, offset, first_len, 0);
    lpr_filed_page_cache_entry_t *second =
        first != 0 ? lpr_page_cache_get(fd, offset + first_len, second_len, 0) : 0;
    if (first == 0 || second == 0) {
        return -1;
    }
    unsigned char *dst = (unsigned char *)(uintptr_t)buf;
    lpr_memcpy(dst, first->data + page_offset, (size_t)first_len);
    lpr_memcpy(dst + first_len, second->data, (size_t)second_len);
    return (int64_t)requested;
}

int64_t lpr_linux_read(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (lpr_pipe_fd_is_active(fd)) {
        lpr_pipe_entry_t *pipe = lpr_pipe_for_fd(fd);
        if (pipe == 0 || !lpr_pipe_fds[fd].readable) {
            return -LPR_LINUX_EBADF;
        }
        if (count == 0) {
            return 0;
        }
        if (buf == 0) {
            return -LPR_LINUX_EFAULT;
        }
        if (pipe->used == 0) {
            return pipe->write_refs == 0 ? 0 : -LPR_LINUX_EAGAIN;
        }
        const uint64_t n = count < pipe->used ? count : pipe->used;
        uint8_t *dst = (uint8_t *)(uintptr_t)buf;
        for (uint64_t i = 0; i < n; i += 1) {
            dst[i] = pipe->data[pipe->head];
            pipe->head = (pipe->head + 1u) % LPR_LINUX_PIPE_BUF_BYTES;
        }
        pipe->used -= (uint32_t)n;
        return (int64_t)n;
    }
    if (lpr_fd_is_filed(fd)) {
        if (count == 0) {
            return 0;
        }
        if (lpr_fd_shadow_offset_eligible(fd) &&
            lpr_fds[fd].offset_valid &&
            count <= LPR_FILED_PAGE_CACHE_BYTES)
        {
            const uint64_t offset = lpr_fds[fd].offset;
            const int64_t cached = lpr_read_from_page_cache(fd, buf, count, offset);
            if (cached >= 0) {
                lpr_fds[fd].offset = offset + (uint64_t)cached;
                lpr_fds[fd].pread_active = 1;
                if (lpr_fds[fd].offset < offset) {
                    lpr_fds[fd].offset_valid = 0;
                }
                return cached;
            }
        }
        if (lpr_fd_shadow_offset_eligible(fd) &&
            lpr_fds[fd].offset_valid &&
            lpr_fds[fd].pread_active)
        {
            const uint64_t offset = lpr_fds[fd].offset;
            const int64_t n = lpr_filed_io(FILED_WIRE_OP_PREAD, fd, buf, count, offset);
            if (n > 0) {
                lpr_fds[fd].offset = offset + (uint64_t)n;
                if (lpr_fds[fd].offset < offset) {
                    lpr_fds[fd].offset_valid = 0;
                }
            }
            return n;
        }
        const int64_t n = lpr_filed_io(FILED_WIRE_OP_READ, fd, buf, count, 0);
        if (n >= 0 && lpr_fd_shadow_offset_eligible(fd)) {
            const uint64_t old_offset = lpr_fds[fd].offset;
            lpr_fds[fd].offset = old_offset + (uint64_t)n;
            if (lpr_fds[fd].offset < old_offset) {
                lpr_fds[fd].offset_valid = 0;
            }
        }
        return n;
    }
    return lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_READ, fd, buf, count);
}

int64_t lpr_linux_readv(uint64_t fd, uint64_t iov_raw, uint64_t iov_count)
{
#if LPR_TRACE_READV_CACHE_STATS
    lpr_readv_cache_total++;
#endif
    if (iov_raw == 0 && iov_count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (lpr_pipe_fd_is_active(fd)) {
        int64_t total = 0;
        const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
        for (uint64_t i = 0; i < iov_count; i += 1) {
            const int64_t n = lpr_linux_read(fd, iov[i].base, iov[i].len);
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
    if (!lpr_fd_is_filed(fd)) {
        return lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_READV, fd, iov_raw, iov_count);
    }
    const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
#if LPR_TRACE_READV_SIZES
    uint64_t trace_requested = 0;
    for (uint64_t i = 0; i < iov_count; i += 1) {
        if (iov[i].len != 0 && trace_requested <= UINT64_MAX - iov[i].len) {
            trace_requested += iov[i].len;
        }
    }
#endif
    if (iov_count > 1 &&
        lpr_fd_shadow_offset_eligible(fd) &&
        lpr_fds[fd].offset_valid)
    {
        uint64_t requested = 0;
        for (uint64_t i = 0; i < iov_count; i += 1) {
            if (iov[i].len == 0) {
                continue;
            }
            if (iov[i].base == 0 || requested > UINT64_MAX - iov[i].len) {
                return -LPR_LINUX_EFAULT;
            }
            requested += iov[i].len;
        }
        if (requested != 0 && requested <= FILED_WIRE_IO_BYTES) {
            const uint64_t offset = lpr_fds[fd].offset;
#if LPR_TRACE_READV_CACHE_STATS
            lpr_readv_cache_coalesced++;
            lpr_readv_cache_bytes += requested;
#endif
#if LPR_TRACE_READV_SIZES
            lpr_trace_readv_size(fd, iov_count, requested, 1, offset);
#endif
            if (requested <= LPR_FILED_PAGE_CACHE_BYTES) {
                const uint64_t page_start = offset & ~(LPR_FILED_PAGE_CACHE_BYTES - 1ull);
                const uint64_t page_offset = offset - page_start;
                if (requested <= LPR_FILED_PAGE_CACHE_BYTES - page_offset) {
                    int cache_hit = 0;
                    lpr_filed_page_cache_entry_t *entry =
                        lpr_page_cache_get(fd, offset, requested, &cache_hit);
                    if (entry != 0) {
#if LPR_TRACE_READV_CACHE_STATS
                        if (cache_hit) {
                            lpr_readv_cache_hit++;
                        } else {
                            lpr_readv_cache_fill++;
                        }
#endif
                        (void)lpr_scatter_iov(iov, iov_count, entry->data + page_offset, requested);
                        lpr_fds[fd].offset = offset + requested;
                        lpr_fds[fd].pread_active = 1;
                        if (lpr_fds[fd].offset < offset) {
                            lpr_fds[fd].offset_valid = 0;
                        }
                        return (int64_t)requested;
                    }
                } else {
#if LPR_TRACE_READV_CACHE_STATS
                    lpr_readv_cache_cross_page++;
#endif
                    const uint64_t first_len = LPR_FILED_PAGE_CACHE_BYTES - page_offset;
                    const uint64_t second_len = requested - first_len;
                    int first_hit = 0;
                    int second_hit = 0;
                    lpr_filed_page_cache_entry_t *first =
                        lpr_page_cache_get(fd, offset, first_len, &first_hit);
                    lpr_filed_page_cache_entry_t *second =
                        first != 0 ? lpr_page_cache_get(fd, offset + first_len, second_len, &second_hit) : 0;
                    if (first != 0 && second != 0) {
                        uint8_t cache_scratch[LPR_FILED_PAGE_CACHE_BYTES];
                        lpr_memcpy(cache_scratch, first->data + page_offset, (size_t)first_len);
                        lpr_memcpy(cache_scratch + first_len, second->data, (size_t)second_len);
#if LPR_TRACE_READV_CACHE_STATS
                        if (first_hit && second_hit) {
                            lpr_readv_cache_hit++;
                        } else {
                            lpr_readv_cache_fill++;
                        }
#endif
                        (void)lpr_scatter_iov(iov, iov_count, cache_scratch, requested);
                        lpr_fds[fd].offset = offset + requested;
                        lpr_fds[fd].pread_active = 1;
                        if (lpr_fds[fd].offset < offset) {
                            lpr_fds[fd].offset_valid = 0;
                        }
                        return (int64_t)requested;
                    }
                }
            }
#if LPR_TRACE_READV_CACHE_STATS
            lpr_readv_cache_fallback++;
#endif
            uint8_t scratch[FILED_WIRE_IO_BYTES];
            const int64_t n = lpr_filed_io(FILED_WIRE_OP_PREAD, fd, (uint64_t)(uintptr_t)scratch, requested, offset);
            if (n < 0) {
                return n;
            }
            const uint64_t got = (uint64_t)n;
            (void)lpr_scatter_iov(iov, iov_count, scratch, got);
            lpr_fds[fd].offset = offset + got;
            lpr_fds[fd].pread_active = 1;
            if (lpr_fds[fd].offset < offset) {
                lpr_fds[fd].offset_valid = 0;
            }
            return (int64_t)got;
        }
        if (requested >= LPR_FILED_READV_TO_VMO_MIN && requested <= LPR_FILED_READV_TO_VMO_MAX) {
            int vmo_fd = -1;
            unsigned char *mapped = 0;
            uint64_t map_len = 0;
            if (lpr_readv_scratch_vmo(requested, &vmo_fd, &mapped, &map_len) == 0) {
#if LPR_TRACE_READV_CACHE_STATS
                lpr_readv_cache_to_vmo++;
#endif
#if LPR_TRACE_READV_SIZES
                lpr_trace_readv_size(fd, iov_count, requested, 2, lpr_fds[fd].offset);
#endif
                const uint64_t offset = lpr_fds[fd].offset;
                const int64_t n = lpr_linux_pread_to_vmo(
                    fd,
                    (uint64_t)(uint32_t)vmo_fd,
                    0,
                    requested,
                    offset);
                if (n >= 0) {
                    const uint64_t got = (uint64_t)n;
                    const unsigned char *src = mapped;
                    (void)lpr_scatter_iov(iov, iov_count, src, got);
                    if (lpr_fd_shadow_offset_eligible(fd)) {
                        lpr_fds[fd].offset = offset + got;
                        lpr_fds[fd].pread_active = 1;
                        if (lpr_fds[fd].offset < offset) {
                            lpr_fds[fd].offset_valid = 0;
                        }
                    }
                    return (int64_t)got;
                }
#if LPR_TRACE_READV_SIZES
                lpr_trace_readv_to_vmo_status(fd, requested, n);
#endif
            }
        }
    }
#if LPR_TRACE_READV_SIZES
    lpr_trace_readv_size(fd, iov_count, trace_requested, 0, lpr_fd_is_filed(fd) ? lpr_fds[fd].offset : 0);
#endif
    int64_t total = 0;
    for (uint64_t i = 0; i < iov_count; i += 1) {
        if (iov[i].len == 0) {
            continue;
        }
        const int64_t n = lpr_linux_read(fd, iov[i].base, iov[i].len);
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

int64_t lpr_linux_pread64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset)
{
    return lpr_filed_io(FILED_WIRE_OP_PREAD, fd, buf, count, offset);
}

int64_t lpr_linux_pread_to_vmo(
    uint64_t fd,
    uint64_t vmo_fd,
    uint64_t vmo_offset,
    uint64_t count,
    uint64_t file_offset)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (vmo_fd < 16) {
        return -LPR_LINUX_EINVAL;
    }
    if (vmo_offset + count < vmo_offset) {
        return -LPR_LINUX_EINVAL;
    }

    void *page = 0;
    const int page_fd = lpr_create_pread_vmo_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }

    filed_wire_pread_vmo_t *pread_vmo = (filed_wire_pread_vmo_t *)page;
    lpr_memset(pread_vmo, 0, sizeof(*pread_vmo));
    pread_vmo->handle = lpr_fds[fd].handle;
    pread_vmo->file_offset = file_offset;
    pread_vmo->vmo_offset = vmo_offset;
    pread_vmo->length = count;

    const int64_t ready = lpr_filed_endpoint_ready();
    if (ready != 0) {
        lpr_destroy_pread_vmo_wire_page(page_fd, page);
        return ready;
    }

    struct pacha_ipc_fd fds[2];
    struct pacha_ipc_msg request;
    struct pacha_ipc_msg reply;
    lpr_zero_bytes(fds, sizeof(fds));
    lpr_zero_bytes(&request, sizeof(request));
    lpr_zero_bytes(&reply, sizeof(reply));

    fds[0].fd = (uint64_t)(uint32_t)page_fd;
    fds[0].rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    fds[1].fd = vmo_fd;
    fds[1].rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;

    const uint64_t request_id = ++lpr_request_id;
    request.word0 = FILED_WIRE_REQUEST_MAGIC;
    request.word1 = FILED_WIRE_OP_PREAD_TO_VMO;
    request.word3 = request_id;
    request.fds = fds;
    request.fd_count = 2;

    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16) {
        lpr_destroy_pread_vmo_wire_page(page_fd, page);
        return lpr_pacha_status_to_errno(reply_fd);
    }

    const int64_t recv_status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)reply_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    lpr_destroy_pread_vmo_wire_page(page_fd, page);
    if (recv_status != 0) {
        return lpr_pacha_status_to_errno(recv_status);
    }
    if (reply.word0 != FILED_WIRE_REPLY_MAGIC || reply.word3 != request_id) {
        return -LPR_LINUX_EIO;
    }
    if ((int64_t)reply.word1 < 0) {
        return (int64_t)reply.word1;
    }
    return (int64_t)reply.word2;
}

typedef struct lpr_pacha_process_status {
    uint64_t state;
    uint64_t exit_code;
    uint64_t id;
    uint64_t generation;
} lpr_pacha_process_status_t;

static int64_t lpr_linux_wait_process_fd(uint64_t process_fd, uint64_t *out_exit_code)
{
    if (process_fd < 16) {
        return -LPR_LINUX_ECHILD;
    }
    lpr_pacha_process_status_t st;
    for (;;) {
        lpr_memset(&st, 0, sizeof(st));
        const int64_t wait_status = lpr_pacha_syscall2(
            PACHA_PROCESS_SYSCALL_WAIT,
            process_fd,
            (uint64_t)(uintptr_t)&st);
        if (wait_status == 0) {
            if (out_exit_code != 0) {
                *out_exit_code = st.exit_code & 0xffu;
            }
            return 0;
        }
        const int64_t errno_status = lpr_pacha_status_to_errno(wait_status);
        if (errno_status != -LPR_LINUX_EAGAIN) {
            return errno_status;
        }
        struct pacha_pollfd pollfd;
        lpr_memset(&pollfd, 0, sizeof(pollfd));
        pollfd.fd = (int)(uint32_t)process_fd;
        pollfd.events = PACHA_FD_EVENT_READABLE;
        (void)lpr_pacha_syscall4(
            PACHA_FD_SYSCALL_WAIT_MANY,
            (uint64_t)(uintptr_t)&pollfd,
            1,
            PACHA_FD_WAIT_FOREVER,
            0);
    }
}

static int lpr_exec_add_string(filed_wire_exec_path_t *exec, filed_wire_exec_string_ref_t *ref, const char *value)
{
    if (exec == 0 || ref == 0 || value == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const uint64_t length = (uint64_t)lpr_strnlen(value, FILED_WIRE_EXEC_STRING_BYTES) + 1u;
    if (length == 0 || length > UINT16_MAX) {
        return -LPR_LINUX_E2BIG;
    }
    if (exec->string_bytes + length > FILED_WIRE_EXEC_STRING_BYTES) {
        return -LPR_LINUX_E2BIG;
    }
    ref->offset = (uint16_t)exec->string_bytes;
    ref->length = (uint16_t)length;
    lpr_memcpy(exec->strings + exec->string_bytes, value, (size_t)length);
    exec->string_bytes += length;
    return 0;
}

static int lpr_exec_copy_string_vector(
    filed_wire_exec_path_t *exec,
    filed_wire_exec_string_ref_t *refs,
    uint64_t max_refs,
    uint64_t vector_raw,
    uint64_t *out_count)
{
    if (out_count == 0) {
        return -LPR_LINUX_EFAULT;
    }
    *out_count = 0;
    if (vector_raw == 0) {
        return 0;
    }
    const char *const *vector = (const char *const *)(uintptr_t)vector_raw;
    uint64_t count = 0;
    while (count < max_refs) {
        const char *value = vector[count];
        if (value == 0) {
            *out_count = count;
            return 0;
        }
        const int status = lpr_exec_add_string(exec, &refs[count], value);
        if (status != 0) {
            return status;
        }
        count++;
    }
    if (vector[count] != 0) {
        return -LPR_LINUX_E2BIG;
    }
    *out_count = count;
    return 0;
}

static int64_t lpr_filed_exec_path(filed_wire_exec_path_t *exec, int *out_process_fd, int *out_thread_fd)
{
    if (exec == 0 || out_process_fd == 0 || out_thread_fd == 0) {
        return -LPR_LINUX_EFAULT;
    }
    *out_process_fd = -1;
    *out_thread_fd = -1;

    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memcpy(page, exec, sizeof(*exec));

    struct pacha_ipc_fd request_fd;
    lpr_memset(&request_fd, 0, sizeof(request_fd));
    request_fd.fd = (uint64_t)(uint32_t)page_fd;
    request_fd.rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;

    const uint64_t request_id = ++lpr_request_id;
    const struct pacha_ipc_msg request = {
        .word0 = FILED_WIRE_REQUEST_MAGIC,
        .word1 = FILED_WIRE_OP_EXEC_PATH,
        .word2 = 0,
        .word3 = request_id,
        .fds = &request_fd,
        .fd_count = 1,
    };
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16) {
        lpr_destroy_wire_page(page_fd, page);
        return lpr_pacha_status_to_errno(reply_fd);
    }

    struct pacha_ipc_fd reply_fds[2];
    struct pacha_ipc_msg reply;
    lpr_memset(reply_fds, 0, sizeof(reply_fds));
    lpr_memset(&reply, 0, sizeof(reply));
    reply.fds = reply_fds;
    reply.fd_capacity = 2;
    const int64_t recv_status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)reply_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    lpr_destroy_wire_page(page_fd, page);
    if (recv_status != 0) {
        return lpr_pacha_status_to_errno(recv_status);
    }
    if (reply.word0 != FILED_WIRE_REPLY_MAGIC || reply.word3 != request_id) {
        return -LPR_LINUX_EIO;
    }
    if ((int64_t)reply.word1 < 0) {
        return (int64_t)reply.word1;
    }
    if (reply.fd_count < 2 || reply_fds[0].fd < 16 || reply_fds[1].fd < 16) {
        return -LPR_LINUX_EIO;
    }
    *out_process_fd = (int)(uint32_t)reply_fds[0].fd;
    *out_thread_fd = (int)(uint32_t)reply_fds[1].fd;
    return 0;
}

int64_t lpr_linux_clone(uint64_t flags, uint64_t child_stack, uint64_t parent_tid, uint64_t child_tid, uint64_t tls)
{
    (void)parent_tid;
    (void)child_tid;
    (void)tls;
    lpr_trace_clone_args(flags, child_stack, parent_tid, child_tid);
    const uint64_t signal = flags & 0xffu;
    const uint64_t clone_vm = 0x100ull;
    const uint64_t clone_vfork = 0x4000ull;
    const uint64_t clone_thread = 0x10000ull;
    const uint64_t known_process_flags = clone_vm | clone_vfork | 0x00100000ull | 0x01000000ull | 0x00200000ull;
    if (signal != 0 && signal != 17u) {
        return -LPR_LINUX_EINVAL;
    }
    if ((flags & clone_thread) != 0) {
        return -LPR_LINUX_ENOSYS;
    }
    if ((flags & ~(known_process_flags | 0xffull)) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const struct lpr_linux_user_frame *user_frame = lpr_current_linux_user_frame();
    if (user_frame == 0) {
        return -LPR_LINUX_ENOSYS;
    }
    const int64_t ret = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_PROCESS_CLONE,
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_KILL,
        PACHA_PROCESS_CLONE_CURRENT_THREAD | PACHA_PROCESS_CLONE_USER_FRAME,
        (uint64_t)(uintptr_t)user_frame);
    if (ret == 0) {
        lpr_pipe_after_fork_child();
        return 0;
    }
    if (ret >= 16) {
        return ret;
    }
    return lpr_pacha_status_to_errno(ret);
}

int64_t lpr_linux_fork(void)
{
    return lpr_linux_clone(17u, 0, 0, 0, 0);
}

int64_t lpr_linux_vfork(void)
{
    return lpr_linux_clone(0x4000ull | 0x100ull | 17u, 0, 0, 0, 0);
}

int64_t lpr_linux_wait4(uint64_t pid, uint64_t status_raw, uint64_t options, uint64_t rusage)
{
    (void)rusage;
    if (options != 0) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t exit_code = 0;
    const int64_t status = lpr_linux_wait_process_fd(pid, &exit_code);
    if (status != 0) {
        return status;
    }
    if (status_raw != 0) {
        int *out_status = (int *)(uintptr_t)status_raw;
        *out_status = (int)((exit_code & 0xffu) << 8);
    }
    return (int64_t)pid;
}

int64_t lpr_linux_execve(uint64_t path_raw, uint64_t argv_raw, uint64_t envp_raw)
{
    const char *path = (const char *)(uintptr_t)path_raw;
    if (path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const uint64_t path_len = (uint64_t)lpr_strnlen(path, FILED_WIRE_PATH_BYTES);
    if (path_len == 0 || path_len >= FILED_WIRE_PATH_BYTES) {
        return -LPR_LINUX_ENAMETOOLONG;
    }

    filed_wire_exec_path_t exec;
    lpr_memset(&exec, 0, sizeof(exec));
    exec.dir_handle = 0;
    exec.flags = FILED_WIRE_EXEC_LINUX_LPR;
    lpr_memcpy(exec.path, path, (size_t)path_len + 1u);

    int status = lpr_exec_copy_string_vector(
        &exec,
        exec.argv,
        FILED_WIRE_EXEC_MAX_ARGS,
        argv_raw,
        &exec.argc);
    if (status != 0) {
        return status;
    }
    if (exec.argc == 0) {
        status = lpr_exec_add_string(&exec, &exec.argv[0], path);
        if (status != 0) {
            return status;
        }
        exec.argc = 1;
    }
    status = lpr_exec_copy_string_vector(
        &exec,
        exec.envp,
        FILED_WIRE_EXEC_MAX_ENVS,
        envp_raw,
        &exec.envc);
    if (status != 0) {
        return status;
    }

    int process_fd = -1;
    int thread_fd = -1;
    lpr_close_cloexec_fds();
    const int64_t exec_status = lpr_filed_exec_path(&exec, &process_fd, &thread_fd);
    if (exec_status != 0) {
        return exec_status;
    }
    uint64_t exit_code = 127;
    const int64_t wait_status = lpr_linux_wait_process_fd((uint64_t)(uint32_t)process_fd, &exit_code);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)thread_fd);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)process_fd);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, wait_status == 0 ? exit_code : 127u);
    for (;;) {
    }
}

int64_t lpr_linux_file_vmo(uint64_t fd, uint64_t file_offset, uint64_t length, uint64_t *out_loaded)
{
    if (out_loaded != 0) {
        *out_loaded = 0;
    }
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (length == 0) {
        return -LPR_LINUX_EINVAL;
    }

    void *page = 0;
    const int page_fd = lpr_create_pread_vmo_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }

    filed_wire_file_vmo_t *file_vmo = (filed_wire_file_vmo_t *)page;
    lpr_memset(file_vmo, 0, sizeof(*file_vmo));
    file_vmo->handle = lpr_fds[fd].handle;
    file_vmo->file_offset = file_offset;
    file_vmo->length = length;

    struct pacha_ipc_fd request_fd;
    struct pacha_ipc_fd reply_fd_items[1];
    struct pacha_ipc_msg request;
    struct pacha_ipc_msg reply;
    lpr_zero_bytes(&request_fd, sizeof(request_fd));
    lpr_zero_bytes(reply_fd_items, sizeof(reply_fd_items));
    lpr_zero_bytes(&request, sizeof(request));
    lpr_zero_bytes(&reply, sizeof(reply));

    request_fd.fd = (uint64_t)(uint32_t)page_fd;
    request_fd.rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;

    const uint64_t request_id = ++lpr_request_id;
    request.word0 = FILED_WIRE_REQUEST_MAGIC;
    request.word1 = FILED_WIRE_OP_FILE_VMO;
    request.word3 = request_id;
    request.fds = &request_fd;
    request.fd_count = 1;

    const int64_t call_reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (call_reply_fd < 16) {
        lpr_destroy_pread_vmo_wire_page(page_fd, page);
        return lpr_pacha_status_to_errno(call_reply_fd);
    }

    reply.fds = reply_fd_items;
    reply.fd_capacity = 1;
    const int64_t recv_status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)call_reply_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)call_reply_fd);
    lpr_destroy_pread_vmo_wire_page(page_fd, page);
    if (recv_status != 0) {
        return lpr_pacha_status_to_errno(recv_status);
    }
    if (reply.word0 != FILED_WIRE_REPLY_MAGIC || reply.word3 != request_id) {
        return -LPR_LINUX_EIO;
    }
    if ((int64_t)reply.word1 < 0) {
        return (int64_t)reply.word1;
    }
    if (reply.fd_count != 1 || reply_fd_items[0].fd < 16) {
        return -LPR_LINUX_EIO;
    }
    if (out_loaded != 0) {
        *out_loaded = reply.word2;
    }
    return (int64_t)reply_fd_items[0].fd;
}

int64_t lpr_linux_write(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (lpr_pipe_fd_is_active(fd)) {
        lpr_pipe_entry_t *pipe = lpr_pipe_for_fd(fd);
        if (pipe == 0 || !lpr_pipe_fds[fd].writable) {
            return -LPR_LINUX_EBADF;
        }
        if (pipe->read_refs == 0) {
            return -LPR_LINUX_EPIPE;
        }
        if (count == 0) {
            return 0;
        }
        if (buf == 0) {
            return -LPR_LINUX_EFAULT;
        }
        const uint64_t space = LPR_LINUX_PIPE_BUF_BYTES - pipe->used;
        if (space == 0) {
            return -LPR_LINUX_EAGAIN;
        }
        const uint64_t n = count < space ? count : space;
        const uint8_t *src = (const uint8_t *)(uintptr_t)buf;
        for (uint64_t i = 0; i < n; i += 1) {
            pipe->data[pipe->tail] = src[i];
            pipe->tail = (pipe->tail + 1u) % LPR_LINUX_PIPE_BUF_BYTES;
        }
        pipe->used += (uint32_t)n;
        return (int64_t)n;
    }
    if (lpr_fd_is_filed(fd)) {
        if (count != 0) {
            lpr_page_cache_invalidate_handle(lpr_fds[fd].handle);
        }
        return lpr_filed_io(FILED_WIRE_OP_WRITE, fd, buf, count, 0);
    }
    if (fd == 1) {
        return (int64_t)count;
    }
    return lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, fd, buf, count);
}

int64_t lpr_linux_writev(uint64_t fd, uint64_t iov_raw, uint64_t iov_count)
{
    if (iov_raw == 0 && iov_count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (!lpr_fd_is_filed(fd)) {
        if (lpr_pipe_fd_is_active(fd)) {
            int64_t total = 0;
            const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
            for (uint64_t i = 0; i < iov_count; i += 1) {
                const int64_t n = lpr_linux_write(fd, iov[i].base, iov[i].len);
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
        if (fd == 1) {
            const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
            uint64_t total = 0;
            for (uint64_t i = 0; i < iov_count; i += 1) {
                if (total > UINT64_MAX - iov[i].len) {
                    return -LPR_LINUX_EINVAL;
                }
                total += iov[i].len;
            }
            return (int64_t)total;
        }
        return lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITEV, fd, iov_raw, iov_count);
    }
    const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
    lpr_page_cache_invalidate_handle(lpr_fds[fd].handle);
    int64_t total = 0;
    for (uint64_t i = 0; i < iov_count; i += 1) {
        if (iov[i].len == 0) {
            continue;
        }
        const int64_t n = lpr_linux_write(fd, iov[i].base, iov[i].len);
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

int64_t lpr_linux_close(uint64_t fd)
{
    if (lpr_pipe_fd_is_active(fd)) {
        lpr_pipe_close_fd(fd);
        return 0;
    }
    if (lpr_fd_is_filed(fd)) {
        const uint64_t handle = lpr_fds[fd].handle;
        lpr_memset(&lpr_fds[fd], 0, sizeof(lpr_fds[fd]));
        return lpr_filed_close_handle(handle);
    }
    if (fd >= 3 && fd < LPR_FD_TABLE_SIZE) {
        return -LPR_LINUX_EBADF;
    }
    return lpr_pacha_status_to_errno(lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, fd));
}

int64_t lpr_linux_lseek(uint64_t fd, uint64_t offset, uint64_t whence)
{
    if (lpr_pipe_fd_is_active(fd)) {
        return -LPR_LINUX_ESPIPE;
    }
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_ESPIPE;
    }
    if (lpr_fd_shadow_offset_eligible(fd) &&
        lpr_fds[fd].offset_valid &&
        whence <= 1)
    {
        const int64_t signed_offset = (int64_t)offset;
        uint64_t new_offset = 0;
        if (whence == 0) {
            if (signed_offset < 0) {
                return -LPR_LINUX_EINVAL;
            }
            new_offset = (uint64_t)signed_offset;
        } else {
            if (signed_offset >= 0) {
                const uint64_t delta = (uint64_t)signed_offset;
                if (lpr_fds[fd].offset > UINT64_MAX - delta) {
                    return -LPR_LINUX_EINVAL;
                }
                new_offset = lpr_fds[fd].offset + delta;
            } else {
                const uint64_t delta = (uint64_t)(-signed_offset);
                if (delta > lpr_fds[fd].offset) {
                    return -LPR_LINUX_EINVAL;
                }
                new_offset = lpr_fds[fd].offset - delta;
            }
        }
        lpr_fds[fd].offset = new_offset;
        lpr_fds[fd].pread_active = 1;
        return (int64_t)new_offset;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_seek_t *seek = (filed_wire_seek_t *)page;
    lpr_memset(seek, 0, sizeof(*seek));
    seek->handle = lpr_fds[fd].handle;
    seek->offset = (int64_t)offset;
    seek->whence = whence;
    uint64_t result = 0;
    const int64_t status = lpr_filed_call(FILED_WIRE_OP_SEEK, page_fd, 0, &result);
    lpr_destroy_wire_page(page_fd, page);
    if (status == 0 && lpr_fd_shadow_offset_eligible(fd)) {
        lpr_fds[fd].offset = result;
        lpr_fds[fd].offset_valid = 1;
        lpr_fds[fd].pread_active = 1;
    }
    return status == 0 ? (int64_t)result : status;
}

int64_t lpr_linux_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg)
{
    if (!lpr_fd_is_filed(fd)) {
        switch (cmd) {
        case LPR_LINUX_F_GETFD:
            return 0;
        case LPR_LINUX_F_SETFD:
            return 0;
        case LPR_LINUX_F_GETFL:
            return 0;
        case LPR_LINUX_F_SETFL:
            return 0;
        default:
            return -LPR_LINUX_EINVAL;
        }
    }
    switch (cmd) {
    case LPR_LINUX_F_GETFD:
        return (lpr_fds[fd].flags & LPR_LINUX_O_CLOEXEC) != 0 ? LPR_LINUX_FD_CLOEXEC : 0;
    case LPR_LINUX_F_SETFD:
        if ((arg & LPR_LINUX_FD_CLOEXEC) != 0) {
            lpr_fds[fd].flags |= LPR_LINUX_O_CLOEXEC;
        } else {
            lpr_fds[fd].flags &= ~LPR_LINUX_O_CLOEXEC;
        }
        return 0;
    case LPR_LINUX_F_GETFL:
        return lpr_fds[fd].flags;
    case LPR_LINUX_F_SETFL:
        lpr_fds[fd].flags = (uint32_t)arg;
        return 0;
    case LPR_LINUX_F_DUPFD:
    case LPR_LINUX_F_DUPFD_CLOEXEC:
        return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
    default:
        return -LPR_LINUX_EINVAL;
    }
}

int64_t lpr_linux_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
    (void)fd;
    (void)arg;
    switch (request) {
    case LPR_LINUX_TCGETS:
    case LPR_LINUX_TIOCGWINSZ:
        return -LPR_LINUX_ENOTTY;
    default:
        return -LPR_LINUX_ENOTTY;
    }
}

static uint8_t lpr_dtype_from_mode(uint64_t mode)
{
    switch (mode & LPR_LINUX_S_IFMT) {
    case LPR_LINUX_S_IFIFO:
        return LPR_LINUX_DT_FIFO;
    case LPR_LINUX_S_IFCHR:
        return LPR_LINUX_DT_CHR;
    case LPR_LINUX_S_IFDIR:
        return LPR_LINUX_DT_DIR;
    case LPR_LINUX_S_IFBLK:
        return LPR_LINUX_DT_BLK;
    case LPR_LINUX_S_IFREG:
        return LPR_LINUX_DT_REG;
    case LPR_LINUX_S_IFLNK:
        return LPR_LINUX_DT_LNK;
    case LPR_LINUX_S_IFSOCK:
        return LPR_LINUX_DT_SOCK;
    default:
        return LPR_LINUX_DT_UNKNOWN;
    }
}

static void lpr_write_linux_stat(void *statbuf, const filed_wire_statx_t *wire)
{
    lpr_linux_stat_t *st = (lpr_linux_stat_t *)statbuf;
    lpr_memset(st, 0, sizeof(*st));
    st->st_dev = 1;
    st->st_ino = wire->handle != 0 ? wire->handle : 1;
    st->st_nlink = wire->nlink != 0 ? wire->nlink : 1;
    st->st_mode = (uint32_t)wire->mode;
    st->st_size = (int64_t)wire->size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)wire->blocks;
    st->st_atime_sec = wire->atime_sec;
    st->st_atime_nsec = wire->atime_nsec;
    st->st_mtime_sec = wire->mtime_sec;
    st->st_mtime_nsec = wire->mtime_nsec;
    st->st_ctime_sec = wire->ctime_sec;
    st->st_ctime_nsec = wire->ctime_nsec;
}

int64_t lpr_linux_fstat(uint64_t fd, uint64_t statbuf)
{
    if (statbuf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (!lpr_fd_is_filed(fd)) {
        return lpr_pacha_status_to_errno(lpr_pacha_syscall2(PACHAOS_SYSCALL_FD_STAT, fd, statbuf));
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_statx_t *stat = (filed_wire_statx_t *)page;
    lpr_memset(stat, 0, sizeof(*stat));
    stat->handle = lpr_fds[fd].handle;
    uint64_t ignored = 0;
    const int64_t status = lpr_filed_call(FILED_WIRE_OP_STAT, page_fd, 0, &ignored);
    if (status == 0) {
        lpr_write_linux_stat((void *)(uintptr_t)statbuf, stat);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_newfstatat(uint64_t dirfd, uint64_t path_raw, uint64_t statbuf, uint64_t flags)
{
    if (statbuf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const char *path = (const char *)(uintptr_t)path_raw;
    if ((flags & LPR_LINUX_AT_EMPTY_PATH) != 0 && path != 0 && path[0] == 0) {
        const uint64_t empty_known_flags = LPR_LINUX_AT_EMPTY_PATH | LPR_LINUX_AT_SYMLINK_NOFOLLOW;
        if ((flags & ~empty_known_flags) != 0) {
            return -LPR_LINUX_EINVAL;
        }
        return lpr_linux_fstat(dirfd, statbuf);
    }
    const int64_t fd = lpr_linux_openat(dirfd, path_raw, LPR_LINUX_O_RDONLY, 0);
    if (fd < 0) {
        return fd;
    }
    const int64_t status = lpr_linux_fstat((uint64_t)fd, statbuf);
    (void)lpr_linux_close((uint64_t)fd);
    return status;
}

int64_t lpr_linux_access(uint64_t path, uint64_t mode)
{
    return lpr_linux_faccessat((uint64_t)(int64_t)LPR_LINUX_AT_FDCWD, path, mode, 0);
}

int64_t lpr_linux_faccessat(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t flags)
{
    const uint64_t known_mode = 0x7ull;
    const uint64_t known_flags = LPR_LINUX_AT_SYMLINK_NOFOLLOW | LPR_LINUX_AT_EMPTY_PATH;
    if ((mode & ~known_mode) != 0 || (flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    struct lpr_linux_stat statbuf;
    lpr_memset(&statbuf, 0, sizeof(statbuf));
    return lpr_linux_newfstatat(dirfd, path, (uint64_t)(uintptr_t)&statbuf, flags);
}

static int64_t lpr_linux_open_metadata(uint64_t dirfd, uint64_t path_raw)
{
    int64_t fd = lpr_linux_openat(dirfd, path_raw, LPR_LINUX_O_RDWR, 0);
    if (fd == -LPR_LINUX_EISDIR) {
        fd = lpr_linux_openat(dirfd, path_raw, LPR_LINUX_O_RDWR | LPR_LINUX_O_DIRECTORY, 0);
    }
    return fd;
}

int64_t lpr_linux_fchmod(uint64_t fd, uint64_t mode)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_chmod_t *chmod_req = (filed_wire_chmod_t *)page;
    lpr_memset(chmod_req, 0, sizeof(*chmod_req));
    chmod_req->handle = lpr_fds[fd].handle;
    chmod_req->mode = mode & 07777ull;
    uint64_t ignored = 0;
    const int64_t status = lpr_filed_call(FILED_WIRE_OP_CHMOD, page_fd, 0, &ignored);
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_fchmodat(uint64_t dirfd, uint64_t path_raw, uint64_t mode, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_AT_SYMLINK_NOFOLLOW | LPR_LINUX_AT_EMPTY_PATH;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const char *path = (const char *)(uintptr_t)path_raw;
    if ((flags & LPR_LINUX_AT_EMPTY_PATH) != 0 && path != 0 && path[0] == 0) {
        return lpr_linux_fchmod(dirfd, mode);
    }
    if (path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const int64_t fd = lpr_linux_open_metadata(dirfd, path_raw);
    if (fd < 0) {
        return fd;
    }
    const int64_t status = lpr_linux_fchmod((uint64_t)fd, mode);
    (void)lpr_linux_close((uint64_t)fd);
    return status;
}

static int64_t lpr_linux_now(lpr_linux_timespec_t *out)
{
    if (out == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_memset(out, 0, sizeof(*out));
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_CLOCK_GETTIME,
        0,
        (uint64_t)(uintptr_t)out);
    return lpr_pacha_status_to_errno(status);
}

static int64_t lpr_linux_resolve_utime(
    const lpr_linux_timespec_t *input,
    const lpr_linux_timespec_t *now,
    uint64_t wire_bit,
    uint64_t *mask,
    int64_t *out_sec,
    int64_t *out_nsec)
{
    if (mask == 0 || out_sec == 0 || out_nsec == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (input == 0) {
        if (now == 0) {
            return -LPR_LINUX_EFAULT;
        }
        *mask |= wire_bit;
        *out_sec = now->tv_sec;
        *out_nsec = now->tv_nsec;
        return 0;
    }
    if (input->tv_nsec == LPR_LINUX_UTIME_OMIT) {
        return 0;
    }
    *mask |= wire_bit;
    if (input->tv_nsec == LPR_LINUX_UTIME_NOW) {
        if (now == 0) {
            return -LPR_LINUX_EFAULT;
        }
        *out_sec = now->tv_sec;
        *out_nsec = now->tv_nsec;
        return 0;
    }
    if (input->tv_nsec < 0 || input->tv_nsec >= 1000000000ll) {
        return -LPR_LINUX_EINVAL;
    }
    *out_sec = input->tv_sec;
    *out_nsec = input->tv_nsec;
    return 0;
}

static int64_t lpr_filed_utimens_handle(uint64_t handle, uint64_t times_raw)
{
    const lpr_linux_timespec_t *times = (const lpr_linux_timespec_t *)(uintptr_t)times_raw;
    lpr_linux_timespec_t now;
    uint64_t mask = 0;
    int64_t status = lpr_linux_now(&now);
    if (status != 0) {
        return status;
    }

    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_utimens_t *utimens = (filed_wire_utimens_t *)page;
    lpr_memset(utimens, 0, sizeof(*utimens));
    utimens->handle = handle;

    status = lpr_linux_resolve_utime(
        times_raw == 0 ? 0 : &times[0],
        &now,
        FILED_WIRE_UTIMENS_ATIME,
        &mask,
        &utimens->atime_sec,
        &utimens->atime_nsec);
    if (status == 0) {
        status = lpr_linux_resolve_utime(
            times_raw == 0 ? 0 : &times[1],
            &now,
            FILED_WIRE_UTIMENS_MTIME,
            &mask,
            &utimens->mtime_sec,
            &utimens->mtime_nsec);
    }
    uint64_t ignored = 0;
    if (status == 0 && mask != 0) {
        utimens->mask = mask;
        status = lpr_filed_call(FILED_WIRE_OP_UTIMENS, page_fd, 0, &ignored);
    }
    lpr_destroy_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_utimensat(uint64_t dirfd, uint64_t path_raw, uint64_t times, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_AT_SYMLINK_NOFOLLOW | LPR_LINUX_AT_EMPTY_PATH;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const char *path = (const char *)(uintptr_t)path_raw;
    if ((flags & LPR_LINUX_AT_EMPTY_PATH) != 0 && path != 0 && path[0] == 0) {
        if (!lpr_fd_is_filed(dirfd)) {
            return -LPR_LINUX_EBADF;
        }
        return lpr_filed_utimens_handle(lpr_fds[dirfd].handle, times);
    }
    if (path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const int64_t fd = lpr_linux_openat(dirfd, path_raw, LPR_LINUX_O_RDWR, 0);
    if (fd < 0) {
        return fd;
    }
    const int64_t status = lpr_filed_utimens_handle(lpr_fds[(uint64_t)fd].handle, times);
    (void)lpr_linux_close((uint64_t)fd);
    return status;
}

int64_t lpr_linux_readlink(uint64_t path, uint64_t buf, uint64_t bufsiz)
{
    if (buf == 0 && bufsiz != 0) {
        return -LPR_LINUX_EFAULT;
    }
    const char *path_string = (const char *)(uintptr_t)path;
    const uint64_t path_len = path_string != 0 ?
        (uint64_t)lpr_strnlen(path_string, FILED_WIRE_PATH_BYTES) :
        0;
    int64_t cached_status = 0;
    if (lpr_readlink_cache_lookup(path_string, path_len, &cached_status)) {
        return cached_status;
    }
#if LPR_TRACE_READLINK_PATHS
    const char *trace_path = path_string;
    if (trace_path != 0) {
        const char prefix[] = "[lpr_runtime] readlink path=";
        const char suffix[] = "\n";
        (void)lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, 2, (uint64_t)(uintptr_t)prefix, sizeof(prefix) - 1u);
        (void)lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, 2, (uint64_t)(uintptr_t)trace_path, (uint64_t)lpr_strnlen(trace_path, FILED_WIRE_PATH_BYTES));
        (void)lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, 2, (uint64_t)(uintptr_t)suffix, sizeof(suffix) - 1u);
    }
#endif
    char target[FILED_WIRE_SYMLINK_TARGET_BYTES];
    lpr_memset(target, 0, sizeof(target));
    const int64_t status = lpr_linux_readlinkat_to_buffer(
        (uint64_t)(int64_t)LPR_LINUX_AT_FDCWD,
        path,
        target,
        sizeof(target));
    if (status < 0) {
        lpr_readlink_cache_store(path_string, path_len, status);
        return status;
    }
    uint64_t copy_len = (uint64_t)status;
    if (copy_len > bufsiz) {
        copy_len = bufsiz;
    }
    if (copy_len != 0) {
        lpr_memcpy((void *)(uintptr_t)buf, target, copy_len);
    }
    return (int64_t)copy_len;
}

static uint16_t lpr_dirent_reclen(uint64_t name_len)
{
    const uint64_t raw = 19u + name_len + 1u;
    return (uint16_t)((raw + 7u) & ~7ull);
}

int64_t lpr_linux_getdents64(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (buf == 0 && count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_getdents_t *gd = (filed_wire_getdents_t *)page;
    lpr_memset(gd, 0, sizeof(*gd));
    gd->dir_handle = lpr_fds[fd].handle;
    gd->capacity = FILED_WIRE_DIRENT_CAPACITY;
    uint64_t ignored = 0;
    int64_t status = lpr_filed_call(FILED_WIRE_OP_GETDENTS, page_fd, 0, &ignored);
    if (status != 0) {
        lpr_destroy_wire_page(page_fd, page);
        return status;
    }

    uint8_t *out = (uint8_t *)(uintptr_t)buf;
    uint64_t written = 0;
    for (uint64_t i = 0; i < gd->count && i < FILED_WIRE_DIRENT_CAPACITY; i += 1) {
        const filed_wire_dirent_t *entry = &gd->entries[i];
        const uint64_t name_len = entry->name_len < FILED_WIRE_DIRENT_NAME_BYTES ?
            entry->name_len :
            FILED_WIRE_DIRENT_NAME_BYTES - 1u;
        const uint16_t reclen = lpr_dirent_reclen(name_len);
        if (written + reclen > count) {
            break;
        }
        lpr_memset(out + written, 0, reclen);
        *(uint64_t *)(void *)(out + written + 0u) = entry->handle != 0 ? entry->handle : (i + 1u);
        *(int64_t *)(void *)(out + written + 8u) = (int64_t)(i + 1u);
        *(uint16_t *)(void *)(out + written + 16u) = reclen;
        *(uint8_t *)(void *)(out + written + 18u) = lpr_dtype_from_mode(entry->kind);
        lpr_memcpy(out + written + 19u, entry->name, (size_t)name_len);
        out[written + 19u + name_len] = 0;
        written += reclen;
    }
    lpr_destroy_wire_page(page_fd, page);
    return (int64_t)written;
}

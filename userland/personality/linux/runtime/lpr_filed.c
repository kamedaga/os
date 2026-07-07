#include "lpr_filed.h"
#include "lpr_linux_syscall.h"
#include "lpr_socket.h"
#include "support/string.h"
#include "support/syscall.h"
#include <filed/ipc_protocol.h>
#include <lpr_supervisor/ipc_protocol.h>
#include <pacha/error_conveyor.h>
#include <pacha/ipc.h>
#include <pachaos/abi.h>
#include <personality/linux_lpr.h>
#include <termd/ipc_protocol.h>
#include <stddef.h>
#include <stdint.h>

#ifndef LPR_TRACE_PROCESS_CALLS
#define LPR_TRACE_PROCESS_CALLS 0
#endif

__attribute__((visibility("hidden")))
void *memset(void *dst, int c, size_t n)
{
    return lpr_memset(dst, c, n);
}

enum {
    LPR_ERRCONV_LAST_FRAMES = 32,
};

static uint64_t lpr_errconv_token = 0x4c50524500000001ull;
static int64_t lpr_errconv_root_status;
static uint64_t lpr_errconv_frame_count;
static uint64_t lpr_errconv_lost_frames;
static pacha_errconv_frame_t lpr_errconv_frames[LPR_ERRCONV_LAST_FRAMES];

static void lpr_errconv_copy_text(char dst[PACHA_ERRCONV_TEXT_BYTES], const char *text)
{
    lpr_memset(dst, 0, PACHA_ERRCONV_TEXT_BYTES);
    if (text == 0) {
        return;
    }
    for (size_t i = 0; i + 1u < PACHA_ERRCONV_TEXT_BYTES && text[i] != '\0'; ++i) {
        dst[i] = text[i];
    }
}

static void lpr_errconv_reset(int64_t root_status)
{
    lpr_errconv_token++;
    if (lpr_errconv_token == 0) {
        lpr_errconv_token = 0x4c50524500000001ull;
    }
    lpr_errconv_root_status = root_status;
    lpr_errconv_frame_count = 0;
    lpr_errconv_lost_frames = 0;
    lpr_memset(lpr_errconv_frames, 0, sizeof(lpr_errconv_frames));
}

static void lpr_errconv_record(
    uint64_t domain,
    uint64_t op,
    uint64_t stage,
    int64_t status,
    int64_t raw_status,
    uint64_t request_id,
    uint64_t fd_count,
    uint64_t subject,
    uint64_t child_token,
    const char *text)
{
    if (lpr_errconv_frame_count == 0) {
        lpr_errconv_reset(status);
    }
    if (lpr_errconv_frame_count >= LPR_ERRCONV_LAST_FRAMES) {
        lpr_errconv_lost_frames++;
        return;
    }
    pacha_errconv_frame_t *frame = &lpr_errconv_frames[lpr_errconv_frame_count++];
    lpr_memset(frame, 0, sizeof(*frame));
    frame->domain = domain;
    frame->component = PACHA_ERRCONV_COMPONENT_LPR_RUNTIME;
    frame->op = op;
    frame->stage = stage;
    frame->status = status;
    frame->raw_status = raw_status;
    frame->request_id = request_id;
    frame->fd_count = fd_count;
    frame->subject = subject;
    frame->child_token = child_token;
    lpr_errconv_copy_text(frame->text, text);
}

#define LPR_FD_TABLE_INITIAL_SIZE 256ull
#define LPR_FD_TABLE_MAX_SIZE (LPR_LINUX_FD_MAX + 1ull)
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
#define LPR_LINUX_CLOSE_RANGE_UNSHARE (1ull << 1u)
#define LPR_LINUX_CLOSE_RANGE_CLOEXEC (1ull << 2u)
#define LPR_LINUX_F_DUPFD 0ull
#define LPR_LINUX_F_GETFD 1ull
#define LPR_LINUX_F_SETFD 2ull
#define LPR_LINUX_F_GETFL 3ull
#define LPR_LINUX_F_SETFL 4ull
#define LPR_LINUX_F_GETLK 5ull
#define LPR_LINUX_F_SETLK 6ull
#define LPR_LINUX_F_SETLKW 7ull
#define LPR_LINUX_F_DUPFD_CLOEXEC 1030ull
#define LPR_LINUX_FD_CLOEXEC 1ull
#define LPR_LINUX_F_UNLCK 2
#define LPR_LINUX_LOCK_SH 1ull
#define LPR_LINUX_LOCK_EX 2ull
#define LPR_LINUX_LOCK_NB 4ull
#define LPR_LINUX_LOCK_UN 8ull
#define LPR_LINUX_TCGETS 0x5401ull
#define LPR_LINUX_TCSETS 0x5402ull
#define LPR_LINUX_TCSETSW 0x5403ull
#define LPR_LINUX_TCSETSF 0x5404ull
#define LPR_LINUX_TERMIOS_BYTES 60u
#define LPR_LINUX_TIOCSCTTY 0x540eull
#define LPR_LINUX_TIOCGPGRP 0x540full
#define LPR_LINUX_TIOCSPGRP 0x5410ull
#define LPR_LINUX_TIOCGWINSZ 0x5413ull
#define LPR_LINUX_TIOCSWINSZ 0x5414ull
#define LPR_LINUX_FIONREAD 0x541bull
#define LPR_LINUX_TIOCNOTTY 0x5422ull
#define LPR_LINUX_TIOCSPTLCK 0x40045431ull
#define LPR_LINUX_UTIME_NOW 1073741823ll
#define LPR_LINUX_UTIME_OMIT 1073741822ll
#define LPR_FILED_PAGE_CACHE_ENTRIES 64u
#define LPR_FILED_PAGE_CACHE_BYTES 4096ull
#define LPR_FILED_READV_TO_VMO_MIN (16ull * 1024ull)
#define LPR_FILED_READV_TO_VMO_MAX (256ull * 1024ull)
#define LPR_LINUX_PIPE_BUF_BYTES 4096ull
#define LPR_EXEC_LOCAL_FD_TABLE_INITIAL_FDS 16ull
#define LPR_LINUX_PROCESS_TABLE_SIZE 64u
#define LPR_LINUX_SIGNAL_MAX 64u
#define LPR_LINUX_WNOHANG 1ull
#define LPR_LINUX_WUNTRACED 2ull
#define LPR_LINUX_WCONTINUED 8ull
#define LPR_LINUX_SIG_BLOCK 0ull
#define LPR_LINUX_SIG_UNBLOCK 1ull
#define LPR_LINUX_SIG_SETMASK 2ull
#define LPR_LINUX_SIG_DFL 0ull
#define LPR_LINUX_SIGKILL 9u
#define LPR_LINUX_SIGCHLD 17u
#define LPR_LINUX_SIGCONT 18u
#define LPR_LINUX_SIGSTOP 19u
#define LPR_LINUX_SIGTSTP 20u
#define LPR_LINUX_SIGTTIN 21u
#define LPR_LINUX_SIGTTOU 22u
#define LPR_LINUX_SIGURG 23u
#define LPR_LINUX_SIGWINCH 28u
#define LPR_LINUX_SIG_IGN 1ull
#define LPR_LINUX_SA_RESTART 0x10000000ull
#define LPR_LINUX_CLOCK_REALTIME 0ull
#define LPR_LINUX_CLOCK_MONOTONIC 1ull
#define LPR_LINUX_TIMER_ABSTIME 1ull

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

typedef struct lpr_event_fd {
    uint8_t active;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t flags;
    uint64_t counter;
} lpr_event_fd_t;

typedef struct lpr_tty_fd {
    uint8_t active;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t flags;
    uint64_t handle;
} lpr_tty_fd_t;

typedef struct lpr_exec_local_fd_table {
    int fd;
    uint64_t map_bytes;
    filed_wire_exec_lpr_fd_table_t *table;
} lpr_exec_local_fd_table_t;

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

typedef struct lpr_linux_process_entry {
    uint8_t active;
    uint8_t waited;
    uint16_t reserved0;
    int32_t linux_pid;
    int32_t linux_ppid;
    int32_t linux_sid;
    int32_t linux_pgrp;
    int32_t process_fd;
    uint32_t reserved1;
} lpr_linux_process_entry_t;

typedef struct lpr_linux_sigaction_record {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
} lpr_linux_sigaction_record_t;

enum {
    LPR_READLINK_CACHE_ENTRIES = 8,
};

static lpr_filed_fd_t lpr_fds_initial[LPR_FD_TABLE_INITIAL_SIZE];
static lpr_pipe_fd_t lpr_pipe_fds_initial[LPR_FD_TABLE_INITIAL_SIZE];
static lpr_event_fd_t lpr_event_fds_initial[LPR_FD_TABLE_INITIAL_SIZE];
static lpr_tty_fd_t lpr_tty_fds_initial[LPR_FD_TABLE_INITIAL_SIZE];
static lpr_filed_fd_t *lpr_fds;
static lpr_pipe_fd_t *lpr_pipe_fds;
static lpr_event_fd_t *lpr_event_fds;
static lpr_tty_fd_t *lpr_tty_fds;
static uint64_t lpr_fd_table_capacity;
static void *lpr_fd_table_dynamic_base;
static uint64_t lpr_fd_table_dynamic_bytes;
static lpr_readlink_cache_entry_t lpr_readlink_cache[LPR_READLINK_CACHE_ENTRIES];
static lpr_filed_page_cache_entry_t lpr_page_cache[LPR_FILED_PAGE_CACHE_ENTRIES];
static uint64_t lpr_readlink_cache_clock;
static uint64_t lpr_page_cache_clock;
static uint64_t lpr_request_id = 0x4c505246494c4501ull;
static int lpr_filed_endpoint_checked;
static int lpr_wire_page_fd = -1;
static void *lpr_wire_page;
static int lpr_wire_page_busy;
static int lpr_tty_wire_page_fd = -1;
static void *lpr_tty_wire_page;
static int lpr_tty_wire_page_busy;
static int lpr_session_fd = -1;
static int lpr_session_page_fd = -1;
static void *lpr_session_page;
static int lpr_session_checked;
static int lpr_session_payload_busy;
static uint64_t lpr_termd_request_id = 0x4c50525445524d01ull;
static int lpr_readv_vmo_fd = -1;
static void *lpr_readv_vmo_map;
static uint64_t lpr_readv_vmo_len;
static int lpr_pread_vmo_page_fd = -1;
static void *lpr_pread_vmo_page;
static int lpr_pread_vmo_page_busy;
static int lpr_default_stdio_checked;
static int lpr_bootstrap_checked;
static int lpr_bootstrap_valid;
static int lpr_bootstrap_local_fds_installed;
static struct lpr_bootstrap lpr_bootstrap;
static int lpr_linux_process_state_checked;
static int32_t lpr_linux_current_pid;
static int32_t lpr_linux_current_ppid;
static int32_t lpr_linux_current_sid;
static int32_t lpr_linux_current_pgrp;
static int32_t lpr_linux_next_pid;
static int32_t lpr_linux_pending_child_pid;
static int32_t lpr_linux_pending_child_ppid;
static int32_t lpr_linux_pending_child_sid;
static int32_t lpr_linux_pending_child_pgrp;
static uint64_t lpr_supervisor_token;
static uint64_t lpr_supervisor_pending_child_token;
static int lpr_supervisor_enabled;
static lpr_linux_process_entry_t lpr_linux_processes[LPR_LINUX_PROCESS_TABLE_SIZE];
static lpr_linux_sigaction_record_t lpr_linux_sigactions[LPR_LINUX_SIGNAL_MAX + 1u];
static uint64_t lpr_linux_signal_mask;
static uint64_t lpr_linux_pending_signal_mask;
static int lpr_linux_signal_dispatching;
static int lpr_cwd_checked;
static uint64_t lpr_cwd_handle;
static char lpr_cwd_path[FILED_WIRE_PATH_BYTES];

static int lpr_pipe_fd_is_active(uint64_t fd);
static int lpr_native_pipe_fd_info(uint64_t fd, struct pacha_fd_info *out);
static int lpr_fd_slot_available(uint64_t fd);
static int lpr_fd_slot_alloc_from(uint64_t min_fd);
static int lpr_fd_table_ensure_capacity(uint64_t required_capacity);
static int lpr_fd_table_ensure_fd(uint64_t fd);
static int64_t lpr_pacha_status_to_errno(int64_t status);

static int lpr_create_wire_page(void **out_page);
static void lpr_destroy_wire_page(int page_fd, void *page);
static int lpr_create_tty_wire_page(void **out_page);
static void lpr_destroy_tty_wire_page(int page_fd, void *page);
static void lpr_linux_process_state_init(void);
static void lpr_linux_pump_tty_signals(void);
static uint64_t lpr_linux_unblockable_signal_mask(void);
static int64_t lpr_supervisor_call(uint64_t op, int page_fd, uint64_t word2, int transfer_fd, uint64_t *out_result);
static int64_t lpr_supervisor_kill_pid(int32_t pid, uint32_t sig, uint64_t *out_delivered);
static int lpr_supervisor_get_state(lprs_wire_process_state_t *out_state);
static int lpr_supervisor_fd_table_replace(void);
static int lpr_supervisor_fd_table_restore(uint64_t token);
static int64_t lpr_tty_wait(uint64_t fd, uint32_t events);
static void lpr_pipe_after_fork_child(void);
static void lpr_cwd_init(void);
static int64_t lpr_filed_dup_handle(uint64_t handle, uint64_t fd_flags, uint64_t *out_handle);
static int lpr_prepare_exec_local_fds(
    filed_wire_exec_path_t *exec,
    lpr_exec_local_fd_table_t *local_table);
static void lpr_destroy_exec_local_fd_table(lpr_exec_local_fd_table_t *local_table);
static int lpr_install_bootstrap_local_fds(const lpr_bootstrap_fd_t *descs, uint64_t count);

static void lpr_filed_session_drop(void)
{
    if (lpr_session_page != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)lpr_session_page,
            FILED_WIRE_SESSION_PAGE_BYTES);
    }
    if (lpr_session_page_fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)lpr_session_page_fd);
    }
    if (lpr_session_fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)lpr_session_fd);
    }
    lpr_session_fd = -1;
    lpr_session_page_fd = -1;
    lpr_session_page = 0;
    lpr_session_checked = 0;
    lpr_session_payload_busy = 0;
}
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

static void lpr_fd_table_init(void)
{
    if (lpr_fd_table_capacity != 0) {
        return;
    }
    lpr_fds = lpr_fds_initial;
    lpr_pipe_fds = lpr_pipe_fds_initial;
    lpr_event_fds = lpr_event_fds_initial;
    lpr_tty_fds = lpr_tty_fds_initial;
    lpr_fd_table_capacity = LPR_FD_TABLE_INITIAL_SIZE;
}

static uint64_t lpr_align_up_pow2(uint64_t value, uint64_t align)
{
    const uint64_t mask = align - 1u;
    if (align == 0 || (align & mask) != 0 || value > UINT64_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

static int lpr_fd_table_segment_bytes(uint64_t capacity, uint64_t element_size, uint64_t *out)
{
    if (out == 0 || capacity > UINT64_MAX / element_size) {
        return 0;
    }
    const uint64_t bytes = lpr_align_up_pow2(capacity * element_size, 4096ull);
    if (bytes == 0) {
        return 0;
    }
    *out = bytes;
    return 1;
}

static int lpr_fd_table_layout(
    uint64_t capacity,
    uint64_t *filed_offset,
    uint64_t *pipe_offset,
    uint64_t *event_offset,
    uint64_t *tty_offset,
    uint64_t *total_bytes)
{
    if (capacity < LPR_FD_TABLE_INITIAL_SIZE ||
        capacity > LPR_FD_TABLE_MAX_SIZE ||
        filed_offset == 0 ||
        pipe_offset == 0 ||
        event_offset == 0 ||
        tty_offset == 0 ||
        total_bytes == 0)
    {
        return 0;
    }
    uint64_t filed_bytes = 0;
    uint64_t pipe_bytes = 0;
    uint64_t event_bytes = 0;
    uint64_t tty_bytes = 0;
    if (!lpr_fd_table_segment_bytes(capacity, sizeof(lpr_filed_fd_t), &filed_bytes) ||
        !lpr_fd_table_segment_bytes(capacity, sizeof(lpr_pipe_fd_t), &pipe_bytes) ||
        !lpr_fd_table_segment_bytes(capacity, sizeof(lpr_event_fd_t), &event_bytes) ||
        !lpr_fd_table_segment_bytes(capacity, sizeof(lpr_tty_fd_t), &tty_bytes))
    {
        return 0;
    }
    if (filed_bytes > UINT64_MAX - pipe_bytes ||
        filed_bytes + pipe_bytes > UINT64_MAX - event_bytes ||
        filed_bytes + pipe_bytes + event_bytes > UINT64_MAX - tty_bytes)
    {
        return 0;
    }
    *filed_offset = 0;
    *pipe_offset = filed_bytes;
    *event_offset = filed_bytes + pipe_bytes;
    *tty_offset = filed_bytes + pipe_bytes + event_bytes;
    *total_bytes = filed_bytes + pipe_bytes + event_bytes + tty_bytes;
    return 1;
}

static uint64_t lpr_fd_table_next_capacity(uint64_t required_capacity)
{
    lpr_fd_table_init();
    uint64_t capacity = lpr_fd_table_capacity;
    if (capacity < LPR_FD_TABLE_INITIAL_SIZE) {
        capacity = LPR_FD_TABLE_INITIAL_SIZE;
    }
    while (capacity < required_capacity) {
        if (capacity >= LPR_FD_TABLE_MAX_SIZE) {
            return 0;
        }
        if (capacity > LPR_FD_TABLE_MAX_SIZE / 2u) {
            capacity = LPR_FD_TABLE_MAX_SIZE;
            continue;
        }
        capacity *= 2u;
    }
    return capacity;
}

static int lpr_fd_table_ensure_capacity(uint64_t required_capacity)
{
    lpr_fd_table_init();
    if (required_capacity <= lpr_fd_table_capacity) {
        return 0;
    }
    if (required_capacity > LPR_FD_TABLE_MAX_SIZE) {
        return -LPR_LINUX_EMFILE;
    }
    const uint64_t new_capacity = lpr_fd_table_next_capacity(required_capacity);
    if (new_capacity == 0) {
        return -LPR_LINUX_EMFILE;
    }
    uint64_t filed_offset = 0;
    uint64_t pipe_offset = 0;
    uint64_t event_offset = 0;
    uint64_t tty_offset = 0;
    uint64_t total_bytes = 0;
    if (!lpr_fd_table_layout(
            new_capacity,
            &filed_offset,
            &pipe_offset,
            &event_offset,
            &tty_offset,
            &total_bytes))
    {
        return -LPR_LINUX_ENOMEM;
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        0,
        0,
        total_bytes,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_PRIVATE | PACHAOS_MMAP_ANONYMOUS,
        0);
    if (mapped < 4096) {
        return (int)lpr_pacha_status_to_errno(mapped);
    }
    unsigned char *base = (unsigned char *)(uintptr_t)mapped;
    lpr_filed_fd_t *new_filed = (lpr_filed_fd_t *)(void *)(base + filed_offset);
    lpr_pipe_fd_t *new_pipe = (lpr_pipe_fd_t *)(void *)(base + pipe_offset);
    lpr_event_fd_t *new_event = (lpr_event_fd_t *)(void *)(base + event_offset);
    lpr_tty_fd_t *new_tty = (lpr_tty_fd_t *)(void *)(base + tty_offset);
    lpr_memcpy(new_filed, lpr_fds, (size_t)(lpr_fd_table_capacity * sizeof(*lpr_fds)));
    lpr_memcpy(new_pipe, lpr_pipe_fds, (size_t)(lpr_fd_table_capacity * sizeof(*lpr_pipe_fds)));
    lpr_memcpy(new_event, lpr_event_fds, (size_t)(lpr_fd_table_capacity * sizeof(*lpr_event_fds)));
    lpr_memcpy(new_tty, lpr_tty_fds, (size_t)(lpr_fd_table_capacity * sizeof(*lpr_tty_fds)));
    if (lpr_fd_table_dynamic_base != 0 && lpr_fd_table_dynamic_bytes != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)lpr_fd_table_dynamic_base,
            lpr_fd_table_dynamic_bytes);
    }
    lpr_fds = new_filed;
    lpr_pipe_fds = new_pipe;
    lpr_event_fds = new_event;
    lpr_tty_fds = new_tty;
    lpr_fd_table_capacity = new_capacity;
    lpr_fd_table_dynamic_base = (void *)(uintptr_t)mapped;
    lpr_fd_table_dynamic_bytes = total_bytes;
    return 0;
}

static int lpr_fd_table_ensure_fd(uint64_t fd)
{
    if (fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    return lpr_fd_table_ensure_capacity(fd + 1u);
}

#if LPR_TRACE_PROCESS_CALLS
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

static char *lpr_clone_trace_append_hex64(char *out, const char *end, uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    out = lpr_clone_trace_append_literal(out, end, "0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        if (out < end) {
            *out++ = digits[(value >> (uint64_t)shift) & 0xfu];
        }
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
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_LOG, (uint64_t)(uintptr_t)line, (uint64_t)(out - line));
}

static void lpr_trace_clone_frame(const char *event, const struct lpr_linux_user_frame *frame, int64_t status)
{
    if (frame == 0) {
        return;
    }
    char line[384];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_clone_trace_append_literal(out, end, "[lpr_runtime] clone_frame ");
    out = lpr_clone_trace_append_literal(out, end, event);
    out = lpr_clone_trace_append_literal(out, end, " frame=");
    out = lpr_clone_trace_append_hex64(out, end, (uint64_t)(uintptr_t)frame);
    out = lpr_clone_trace_append_literal(out, end, " rip=");
    out = lpr_clone_trace_append_hex64(out, end, frame->rip);
    out = lpr_clone_trace_append_literal(out, end, " rsp=");
    out = lpr_clone_trace_append_hex64(out, end, frame->rsp);
    out = lpr_clone_trace_append_literal(out, end, " rcx=");
    out = lpr_clone_trace_append_hex64(out, end, frame->rcx);
    out = lpr_clone_trace_append_literal(out, end, " rax=");
    out = lpr_clone_trace_append_hex64(out, end, frame->rax);
    out = lpr_clone_trace_append_literal(out, end, " rdx=");
    out = lpr_clone_trace_append_hex64(out, end, frame->rdx);
    out = lpr_clone_trace_append_literal(out, end, " status=");
    if (status < 0) {
        out = lpr_clone_trace_append_literal(out, end, "-");
        status = -status;
    }
    out = lpr_clone_trace_append_u64(out, end, (uint64_t)status);
    out = lpr_clone_trace_append_literal(out, end, "\n");
    (void)lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, 2, (uint64_t)(uintptr_t)line, (uint64_t)(out - line));
}

static void lpr_trace_process_event(const char *event, uint64_t a, uint64_t b, int64_t status)
{
    char line[224];
    char *out = line;
    const char *end = line + sizeof(line);
    out = lpr_clone_trace_append_literal(out, end, "[lpr_runtime] process ");
    out = lpr_clone_trace_append_literal(out, end, event);
    out = lpr_clone_trace_append_literal(out, end, " pid=");
    if (lpr_linux_current_pid < 0) {
        out = lpr_clone_trace_append_literal(out, end, "-");
        out = lpr_clone_trace_append_u64(out, end, (uint64_t)(-lpr_linux_current_pid));
    } else {
        out = lpr_clone_trace_append_u64(out, end, (uint64_t)lpr_linux_current_pid);
    }
    out = lpr_clone_trace_append_literal(out, end, " a=");
    out = lpr_clone_trace_append_u64(out, end, a);
    out = lpr_clone_trace_append_literal(out, end, " b=");
    out = lpr_clone_trace_append_u64(out, end, b);
    out = lpr_clone_trace_append_literal(out, end, " status=");
    if (status < 0) {
        out = lpr_clone_trace_append_literal(out, end, "-");
        status = -status;
    }
    out = lpr_clone_trace_append_u64(out, end, (uint64_t)status);
    out = lpr_clone_trace_append_literal(out, end, "\n");
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_LOG, (uint64_t)(uintptr_t)line, (uint64_t)(out - line));
}
#else
static void lpr_trace_clone_args(uint64_t flags, uint64_t child_stack, uint64_t parent_tid, uint64_t child_tid)
{
    (void)flags;
    (void)child_stack;
    (void)parent_tid;
    (void)child_tid;
}

static void lpr_trace_clone_frame(const char *event, const struct lpr_linux_user_frame *frame, int64_t status)
{
    (void)event;
    (void)frame;
    (void)status;
}

static void lpr_trace_process_event(const char *event, uint64_t a, uint64_t b, int64_t status)
{
    (void)event;
    (void)a;
    (void)b;
    (void)status;
}
#endif

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
    if (status > PACHAOS_SYSCALL_ERR_CLOSED) {
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
    case PACHAOS_SYSCALL_ERR_CLOSED:
        return -LPR_LINUX_EPIPE;
    default:
        return -LPR_LINUX_EINVAL;
    }
}

static int lpr_fd_is_filed(uint64_t fd)
{
    lpr_fd_table_init();
    return fd < lpr_fd_table_capacity && lpr_fds[fd].active != 0;
}

int lpr_linux_tty_fd_active(uint64_t fd)
{
    lpr_fd_table_init();
    return fd < lpr_fd_table_capacity && lpr_tty_fds[fd].active != 0;
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

static int lpr_runtime_reserved_fd(uint64_t fd)
{
    return fd == LPR_FILED_ENDPOINT_FD ||
        fd == LPR_NETD_ENDPOINT_FD ||
        fd == LPR_TERMD_TTY_ENDPOINT_FD ||
        fd == LPR_BOOTSTRAP_FD ||
        fd == LPR_SUPERVISOR_ENDPOINT_FD;
}

static int lpr_native_fd_info(uint64_t fd, struct pacha_fd_info *out)
{
    if (fd > LPR_LINUX_FD_MAX || out == 0) {
        return 0;
    }
    lpr_memset(out, 0, sizeof(*out));
    return lpr_pacha_syscall2(
        PACHAOS_SYSCALL_FD_GET_INFO,
        fd,
        (uint64_t)(uintptr_t)out) == 0;
}

static int64_t lpr_close_native_fd_if_open(uint64_t fd)
{
    struct pacha_fd_info info;
    if (fd > LPR_LINUX_FD_MAX || lpr_runtime_reserved_fd(fd)) {
        return 0;
    }
    if (!lpr_native_fd_info(fd, &info)) {
        return 0;
    }
    return lpr_pacha_status_to_errno(
        lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, fd));
}

static int lpr_fd_local_active(uint64_t fd)
{
    return lpr_fd_is_filed(fd) ||
        lpr_pipe_fd_is_active(fd) ||
        lpr_linux_eventfd_active(fd) ||
        lpr_linux_tty_fd_active(fd);
}

static uint32_t lpr_pipe_flags_from_info(const struct pacha_fd_info *info)
{
    uint32_t flags = 0;
    const int readable = (info->rights & PACHA_FD_RIGHT_READ) != 0;
    const int writable = (info->rights & PACHA_FD_RIGHT_WRITE) != 0;
    if (readable && writable) {
        flags |= LPR_LINUX_O_RDWR;
    } else if (writable) {
        flags |= LPR_LINUX_O_WRONLY;
    }
    if ((info->flags & PACHA_FD_FLAG_NONBLOCK) != 0) {
        flags |= LPR_LINUX_O_NONBLOCK;
    }
    if ((info->flags & PACHA_FD_FLAG_CLOEXEC) != 0) {
        flags |= LPR_LINUX_O_CLOEXEC;
    }
    return flags;
}

static int lpr_pipe_track_native_fd(uint64_t fd, const struct pacha_fd_info *info)
{
    if (fd > LPR_LINUX_FD_MAX || info == 0) {
        return -LPR_LINUX_EMFILE;
    }
    const int ensure_status = lpr_fd_table_ensure_fd(fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    lpr_memset(&lpr_pipe_fds[fd], 0, sizeof(lpr_pipe_fds[fd]));
    lpr_pipe_fds[fd].active = 1;
    lpr_pipe_fds[fd].readable = (info->rights & PACHA_FD_RIGHT_READ) != 0 ? 1u : 0u;
    lpr_pipe_fds[fd].writable = (info->rights & PACHA_FD_RIGHT_WRITE) != 0 ? 1u : 0u;
    lpr_pipe_fds[fd].flags = lpr_pipe_flags_from_info(info);
    return 0;
}

static int lpr_fd_linux_visible_active(uint64_t fd)
{
    if (lpr_fd_local_active(fd) || lpr_linux_socket_fd_active(fd)) {
        return 1;
    }
    struct pacha_fd_info info;
    return lpr_native_fd_info(fd, &info) && info.kind == PACHA_FD_KIND_PIPE;
}

static int lpr_fd_alloc(uint64_t handle, uint64_t flags)
{
    const int fd = lpr_fd_slot_alloc_from(3);
    if (fd < 0) {
        return fd;
    }
    lpr_fds[(uint64_t)fd].active = 1;
    lpr_fds[(uint64_t)fd].offset_valid =
        ((flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY) ? 1u : 0u;
    lpr_fds[(uint64_t)fd].pread_active = 0;
    lpr_fds[(uint64_t)fd].flags = (uint32_t)flags;
    lpr_fds[(uint64_t)fd].handle = handle;
    lpr_fds[(uint64_t)fd].offset = 0;
    return fd;
}

static int lpr_fd_slot_alloc(void)
{
    return lpr_fd_slot_alloc_from(3);
}

static int lpr_fd_slot_available(uint64_t fd)
{
    lpr_fd_table_init();
    struct pacha_fd_info native_info;
    return fd < lpr_fd_table_capacity &&
        !lpr_runtime_reserved_fd(fd) &&
        !lpr_fd_local_active(fd) &&
        !lpr_linux_socket_fd_active(fd) &&
        !lpr_native_fd_info(fd, &native_info);
}

static int lpr_fd_slot_alloc_from(uint64_t min_fd)
{
    if (min_fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t fd = min_fd;
    for (;;) {
        if (fd >= lpr_fd_table_capacity) {
            const int status = lpr_fd_table_ensure_fd(fd);
            if (status != 0) {
                return status == -LPR_LINUX_EINVAL ? -LPR_LINUX_EMFILE : status;
            }
        }
        while (fd < lpr_fd_table_capacity) {
            if (lpr_fd_slot_available(fd)) {
                return (int)fd;
            }
            fd++;
        }
        if (lpr_fd_table_capacity >= LPR_FD_TABLE_MAX_SIZE) {
            return -LPR_LINUX_EMFILE;
        }
    }
}

static int lpr_install_local_fd_descs(const lpr_bootstrap_fd_t *descs, uint64_t count)
{
    if (count != 0 && descs == 0) {
        return 0;
    }
    for (uint64_t i = 0; i < count; ++i) {
        const lpr_bootstrap_fd_t *desc = &descs[i];
        const uint64_t fd = desc->fd;
        if (fd > LPR_LINUX_FD_MAX || lpr_runtime_reserved_fd(fd)) {
            return 0;
        }
        if (lpr_fd_table_ensure_fd(fd) != 0 || !lpr_fd_slot_available(fd)) {
            return 0;
        }
        switch (desc->kind) {
        case LPR_BOOTSTRAP_FD_FILED:
            if (desc->handle == 0) {
                return 0;
            }
            lpr_fds[fd].active = 1;
            lpr_fds[fd].offset_valid =
                ((desc->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY) ? 1u : 0u;
            lpr_fds[fd].pread_active = 0;
            lpr_fds[fd].flags = desc->flags;
            lpr_fds[fd].handle = desc->handle;
            lpr_fds[fd].offset = desc->offset_or_counter;
            break;
        case LPR_BOOTSTRAP_FD_TTY:
            if (desc->handle == 0) {
                return 0;
            }
            lpr_tty_fds[fd].active = 1;
            lpr_tty_fds[fd].flags = desc->flags;
            lpr_tty_fds[fd].handle = desc->handle;
            break;
        case LPR_BOOTSTRAP_FD_EVENT:
            lpr_event_fds[fd].active = 1;
            lpr_event_fds[fd].flags = desc->flags;
            lpr_event_fds[fd].counter = desc->offset_or_counter;
            break;
        default:
            return 0;
        }
    }
    return 1;
}

static int lpr_install_bootstrap_local_fds(const lpr_bootstrap_fd_t *descs, uint64_t count)
{
    if (lpr_bootstrap_local_fds_installed) {
        return 1;
    }
    lpr_bootstrap_local_fds_installed = 1;
    return lpr_install_local_fd_descs(descs, count);
}

static int lpr_pipe_fd_is_active(uint64_t fd)
{
    lpr_fd_table_init();
    return fd < lpr_fd_table_capacity && lpr_pipe_fds[fd].active != 0;
}

int lpr_linux_pipe_fd_active(uint64_t fd)
{
    return lpr_pipe_fd_is_active(fd);
}

static int lpr_native_pipe_fd_info(uint64_t fd, struct pacha_fd_info *out)
{
    if (fd > LPR_LINUX_FD_MAX || out == 0) {
        return 0;
    }
    if (lpr_fd_local_active(fd) || lpr_linux_socket_fd_active(fd)) {
        return 0;
    }
    return lpr_native_fd_info(fd, out) && out->kind == PACHA_FD_KIND_PIPE;
}

static int lpr_native_pipe_slot_claimable(uint64_t fd, struct pacha_fd_info *out)
{
    if (fd > LPR_LINUX_FD_MAX ||
        out == 0 ||
        lpr_runtime_reserved_fd(fd) ||
        lpr_fd_local_active(fd) ||
        lpr_linux_socket_fd_active(fd))
    {
        return 0;
    }
    return lpr_native_fd_info(fd, out) && out->kind == PACHA_FD_KIND_PIPE;
}

static uint64_t lpr_pipe_flags_to_pacha(uint64_t flags)
{
    uint64_t out = 0;
    if ((flags & LPR_LINUX_O_CLOEXEC) != 0) {
        out |= PACHA_FD_FLAG_CLOEXEC;
    }
    if ((flags & LPR_LINUX_O_NONBLOCK) != 0) {
        out |= PACHA_FD_FLAG_NONBLOCK;
    }
    return out;
}

static uint64_t lpr_pipe_rights(int readable)
{
    return PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_CLOSE |
        (readable ? PACHA_FD_RIGHT_READ : PACHA_FD_RIGHT_WRITE);
}

static uint64_t lpr_pipe_poll_events_to_pacha(uint32_t events)
{
    uint64_t out = 0;
    if ((events & 0x0001u) != 0) {
        out |= PACHA_FD_EVENT_READABLE;
    }
    if ((events & 0x0004u) != 0) {
        out |= PACHA_FD_EVENT_WRITABLE;
    }
    if ((events & 0x0008u) != 0) {
        out |= PACHA_FD_EVENT_ERROR;
    }
    return out;
}

static uint32_t lpr_pipe_poll_events_from_pacha(uint64_t events)
{
    uint32_t out = 0;
    if ((events & PACHA_FD_EVENT_READABLE) != 0) {
        out |= 0x0001u;
    }
    if ((events & PACHA_FD_EVENT_WRITABLE) != 0) {
        out |= 0x0004u;
    }
    if ((events & PACHA_FD_EVENT_ERROR) != 0) {
        out |= 0x0008u;
    }
    if ((events & PACHA_FD_EVENT_HANGUP) != 0) {
        out |= 0x0010u;
    }
    return out;
}

uint32_t lpr_linux_pipe_poll_events(uint64_t fd, uint32_t events)
{
    if (!lpr_pipe_fd_is_active(fd)) {
        return 0x0020u;
    }
    struct pacha_pollfd pollfd;
    lpr_memset(&pollfd, 0, sizeof(pollfd));
    pollfd.fd = (int)(uint32_t)fd;
    pollfd.events = lpr_pipe_poll_events_to_pacha(events);
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_FD_POLL,
        (uint64_t)(uintptr_t)&pollfd,
        1);
    if (status != 0 && pollfd.revents == 0) {
        return 0x0020u;
    }
    return lpr_pipe_poll_events_from_pacha(pollfd.revents);
}

uint32_t lpr_linux_native_fd_poll_events(uint64_t fd, uint32_t events)
{
    struct pacha_fd_info info;
    if (!lpr_native_pipe_fd_info(fd, &info)) {
        return 0x0020u;
    }
    struct pacha_pollfd pollfd;
    lpr_memset(&pollfd, 0, sizeof(pollfd));
    pollfd.fd = (int)(uint32_t)fd;
    pollfd.events = lpr_pipe_poll_events_to_pacha(events);
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_FD_POLL,
        (uint64_t)(uintptr_t)&pollfd,
        1);
    if (status != 0 && pollfd.revents == 0) {
        return 0x0020u;
    }
    return lpr_pipe_poll_events_from_pacha(pollfd.revents);
}

static int64_t lpr_pipe_wait(uint64_t fd, uint32_t events)
{
    for (;;) {
        struct pacha_pollfd pollfd;
        lpr_memset(&pollfd, 0, sizeof(pollfd));
        pollfd.fd = (int)(uint32_t)fd;
        pollfd.events = lpr_pipe_poll_events_to_pacha(events | 0x0008u);
        const int64_t poll_status = lpr_pacha_syscall2(
            PACHAOS_SYSCALL_FD_POLL,
            (uint64_t)(uintptr_t)&pollfd,
            1);
        if (poll_status != 0 && pollfd.revents == 0) {
            return lpr_pacha_status_to_errno(poll_status);
        }
        const uint32_t revents = lpr_pipe_poll_events_from_pacha(pollfd.revents);
        if ((revents & 0x0008u) != 0) {
            return -LPR_LINUX_EPIPE;
        }
        if ((revents & (events | 0x0010u)) != 0) {
            return 0;
        }
        const int64_t wait_status = lpr_pacha_syscall4(
            PACHAOS_SYSCALL_FD_WAIT_MANY,
            (uint64_t)(uintptr_t)&pollfd,
            1,
            PACHA_FD_WAIT_FOREVER,
            0);
        if (wait_status == PACHA_SYSCALL_ERR_NOT_READY ||
            wait_status == -PACHA_SYSCALL_ERR_NOT_READY)
        {
            continue;
        }
        if (wait_status < 0) {
            const int64_t errno_status = lpr_pacha_status_to_errno(wait_status);
            if (errno_status == -LPR_LINUX_EAGAIN) {
                continue;
            }
            return errno_status;
        }
        if (wait_status > 0) {
            return 0;
        }
    }
}

static int64_t lpr_native_pipe_read(uint64_t fd, uint64_t buf, uint64_t count)
{
    struct pacha_fd_info info;
    if (!lpr_native_pipe_fd_info(fd, &info)) {
        return -LPR_LINUX_EBADF;
    }
    if ((info.rights & PACHA_FD_RIGHT_READ) == 0) {
        return -LPR_LINUX_EBADF;
    }
    if (count == 0) {
        return 0;
    }
    if (buf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    for (;;) {
        const int64_t n = lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_READ, fd, buf, count);
        if (n >= 0) {
            return n;
        }
        const int64_t err = lpr_pacha_status_to_errno(n);
        if (err != -LPR_LINUX_EAGAIN ||
            (info.flags & PACHA_FD_FLAG_NONBLOCK) != 0)
        {
            return err;
        }
        const int64_t wait_status = lpr_pipe_wait(fd, 0x0001u);
        if (wait_status != 0) {
            return wait_status;
        }
    }
}

static int64_t lpr_native_pipe_readv(uint64_t fd, uint64_t iov_raw, uint64_t iov_count)
{
    struct pacha_fd_info info;
    if (!lpr_native_pipe_fd_info(fd, &info)) {
        return -LPR_LINUX_EBADF;
    }
    if ((info.rights & PACHA_FD_RIGHT_READ) == 0) {
        return -LPR_LINUX_EBADF;
    }
    for (;;) {
        const int64_t n = lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_READV, fd, iov_raw, iov_count);
        if (n >= 0) {
            return n;
        }
        const int64_t err = lpr_pacha_status_to_errno(n);
        if (err != -LPR_LINUX_EAGAIN ||
            (info.flags & PACHA_FD_FLAG_NONBLOCK) != 0)
        {
            return err;
        }
        const int64_t wait_status = lpr_pipe_wait(fd, 0x0001u);
        if (wait_status != 0) {
            return wait_status;
        }
    }
}

static int64_t lpr_native_pipe_write(uint64_t fd, uint64_t buf, uint64_t count)
{
    struct pacha_fd_info info;
    if (!lpr_native_pipe_fd_info(fd, &info)) {
        return -LPR_LINUX_EBADF;
    }
    if ((info.rights & PACHA_FD_RIGHT_WRITE) == 0) {
        return -LPR_LINUX_EBADF;
    }
    if (count == 0) {
        return 0;
    }
    if (buf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    for (;;) {
        const int64_t n = lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, fd, buf, count);
        if (n >= 0) {
            return n;
        }
        const int64_t err = lpr_pacha_status_to_errno(n);
        if (err == -LPR_LINUX_EPIPE) {
            return err;
        }
        if (err != -LPR_LINUX_EAGAIN ||
            (info.flags & PACHA_FD_FLAG_NONBLOCK) != 0)
        {
            return err;
        }
        const int64_t wait_status = lpr_pipe_wait(fd, 0x0004u);
        if (wait_status != 0) {
            return wait_status;
        }
    }
}

static int64_t lpr_native_pipe_writev(uint64_t fd, uint64_t iov_raw, uint64_t iov_count)
{
    struct pacha_fd_info info;
    if (!lpr_native_pipe_fd_info(fd, &info)) {
        return -LPR_LINUX_EBADF;
    }
    if ((info.rights & PACHA_FD_RIGHT_WRITE) == 0) {
        return -LPR_LINUX_EBADF;
    }
    for (;;) {
        const int64_t n = lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITEV, fd, iov_raw, iov_count);
        if (n >= 0) {
            return n;
        }
        const int64_t err = lpr_pacha_status_to_errno(n);
        if (err == -LPR_LINUX_EPIPE) {
            return err;
        }
        if (err != -LPR_LINUX_EAGAIN ||
            (info.flags & PACHA_FD_FLAG_NONBLOCK) != 0)
        {
            return err;
        }
        const int64_t wait_status = lpr_pipe_wait(fd, 0x0004u);
        if (wait_status != 0) {
            return wait_status;
        }
    }
}

int lpr_linux_eventfd_active(uint64_t fd)
{
    lpr_fd_table_init();
    return fd < lpr_fd_table_capacity && lpr_event_fds[fd].active != 0;
}

static int64_t lpr_termd_call(uint64_t op, int page_fd, uint64_t word2, uint64_t *out_result)
{
    if (LPR_TERMD_TTY_ENDPOINT_FD < 16) {
        return -LPR_LINUX_ENOTTY;
    }
    struct pacha_ipc_fd fds[2];
    struct pacha_ipc_msg request;
    struct pacha_ipc_msg reply;
    lpr_zero_bytes(fds, sizeof(fds));
    lpr_zero_bytes(&request, sizeof(request));
    lpr_zero_bytes(&reply, sizeof(reply));

    uint64_t fd_count = 0;
    if (page_fd >= 16) {
        fds[fd_count].fd = (uint64_t)(uint32_t)page_fd;
        fds[fd_count].rights =
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE;
        fd_count++;
    }

    request.word0 = TERMD_WIRE_REQUEST_MAGIC;
    request.word1 = op;
    request.word2 = word2;
    request.word3 = ++lpr_termd_request_id;
    request.fds = fds;
    request.fd_count = fd_count;
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_TERMD_TTY_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16) {
        const int64_t err = lpr_pacha_status_to_errno(reply_fd);
        lpr_errconv_record(
            PACHA_ERRCONV_DOMAIN_KERNEL_STATUS,
            op,
            PACHA_ERRCONV_STAGE_CHILD_RPC_CALL,
            err,
            reply_fd,
            request.word3,
            fd_count,
            LPR_TERMD_TTY_ENDPOINT_FD,
            0,
            "termd ipc_call failed");
        return lpr_pacha_status_to_errno(reply_fd);
    }
    const int64_t recv_status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)reply_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    if (recv_status != 0) {
        const int64_t err = lpr_pacha_status_to_errno(recv_status);
        lpr_errconv_record(
            PACHA_ERRCONV_DOMAIN_KERNEL_STATUS,
            op,
            PACHA_ERRCONV_STAGE_CHILD_RPC_RECV,
            err,
            recv_status,
            request.word3,
            fd_count,
            (uint64_t)(uint32_t)reply_fd,
            0,
            "termd reply recv failed");
        return err;
    }
    if (reply.word0 != TERMD_WIRE_REPLY_MAGIC || reply.word3 != request.word3) {
        lpr_errconv_record(
            PACHA_ERRCONV_DOMAIN_TERMD_STATUS,
            op,
            PACHA_ERRCONV_STAGE_REPLY_MAGIC,
            -LPR_LINUX_EIO,
            (int64_t)reply.word0,
            request.word3,
            fd_count,
            reply.word3,
            reply.word2,
            "termd reply mismatch");
        return -LPR_LINUX_EIO;
    }
    if ((int64_t)reply.word1 < 0) {
        lpr_errconv_record(
            PACHA_ERRCONV_DOMAIN_TERMD_STATUS,
            op,
            PACHA_ERRCONV_STAGE_CHILD_STATUS,
            (int64_t)reply.word1,
            (int64_t)reply.word1,
            request.word3,
            fd_count,
            0,
            reply.word2,
            "termd returned error");
    }
    if (out_result != 0) {
        *out_result = reply.word2;
    }
    return (int64_t)reply.word1;
}

static int lpr_tty_fd_alloc(uint64_t handle, uint64_t flags)
{
    const int fd = lpr_fd_slot_alloc();
    if (fd < 0) {
        return fd;
    }
    lpr_tty_fds[fd].active = 1;
    lpr_tty_fds[fd].flags = (uint32_t)flags;
    lpr_tty_fds[fd].handle = handle;
    return fd;
}

static uint64_t lpr_parse_pts_index(const char *path)
{
    const char prefix[] = "/dev/pts/";
    for (uint64_t i = 0; prefix[i] != 0; i++) {
        if (path[i] != prefix[i]) {
            return UINT64_MAX;
        }
    }
    uint64_t value = 0;
    uint64_t pos = sizeof(prefix) - 1u;
    if (path[pos] == 0) {
        return UINT64_MAX;
    }
    while (path[pos] != 0) {
        if (path[pos] < '0' || path[pos] > '9') {
            return UINT64_MAX;
        }
        value = value * 10u + (uint64_t)(path[pos] - '0');
        pos++;
    }
    return value;
}

static uint64_t lpr_parse_hvc_index(const char *path)
{
    const char prefix[] = "/dev/hvc";
    for (uint64_t i = 0; prefix[i] != 0; i++) {
        if (path[i] != prefix[i]) {
            return UINT64_MAX;
        }
    }
    uint64_t value = 0;
    uint64_t pos = sizeof(prefix) - 1u;
    if (path[pos] == 0) {
        return UINT64_MAX;
    }
    while (path[pos] != 0) {
        if (path[pos] < '0' || path[pos] > '9') {
            return UINT64_MAX;
        }
        value = value * 10u + (uint64_t)(path[pos] - '0');
        pos++;
    }
    return value;
}

static void lpr_fill_termd_caller(uint64_t *session_id, uint64_t *process_id, uint64_t *pgrp_id)
{
    lpr_linux_process_state_init();
    if (session_id != 0) {
        *session_id = (uint64_t)(uint32_t)lpr_linux_current_sid;
    }
    if (process_id != 0) {
        *process_id = (uint64_t)(uint32_t)lpr_linux_current_pid;
    }
    if (pgrp_id != 0) {
        *pgrp_id = (uint64_t)(uint32_t)lpr_linux_current_pgrp;
    }
}

static uint64_t lpr_linux_signal_bit(uint32_t sig)
{
    if (sig == 0 || sig > LPR_LINUX_SIGNAL_MAX) {
        return 0;
    }
    return 1ull << (sig - 1u);
}

static void lpr_linux_queue_signal(uint32_t sig)
{
    const uint64_t bit = lpr_linux_signal_bit(sig);
    if (bit != 0) {
        lpr_linux_pending_signal_mask |= bit;
    }
}

static int lpr_linux_default_signal_ignored(uint32_t sig)
{
    return sig == LPR_LINUX_SIGCHLD ||
        sig == LPR_LINUX_SIGURG ||
        sig == LPR_LINUX_SIGWINCH ||
        sig == LPR_LINUX_SIGCONT;
}

static int lpr_linux_default_signal_stops(uint32_t sig)
{
    return sig == LPR_LINUX_SIGSTOP ||
        sig == LPR_LINUX_SIGTSTP ||
        sig == LPR_LINUX_SIGTTIN ||
        sig == LPR_LINUX_SIGTTOU;
}

static void lpr_linux_exit_for_signal(uint32_t sig)
{
    const uint64_t exit_code = 128u + (uint64_t)sig;
    lpr_linux_prepare_process_exit(exit_code);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, exit_code);
    for (;;) {
    }
}

static uint32_t lpr_linux_first_pending_signal(uint64_t mask)
{
    for (uint32_t sig = 1; sig <= LPR_LINUX_SIGNAL_MAX; sig += 1) {
        if ((mask & lpr_linux_signal_bit(sig)) != 0) {
            return sig;
        }
    }
    return 0;
}

int64_t lpr_linux_dispatch_pending_signals(void)
{
    if (lpr_linux_signal_dispatching) {
        return 0;
    }
    lpr_linux_signal_dispatching = 1;

    for (uint64_t i = 0; i < LPR_LINUX_SIGNAL_MAX; i += 1) {
        const int64_t sig = lpr_pacha_syscall0(PACHAOS_SYSCALL_PROCESS_CONSUME_SIGNAL);
        if (sig <= 0 || sig > (int64_t)LPR_LINUX_SIGNAL_MAX) {
            break;
        }
        lpr_linux_queue_signal((uint32_t)sig);
    }

    int64_t result = 0;
    for (;;) {
        const uint64_t deliverable = lpr_linux_pending_signal_mask & ~lpr_linux_signal_mask;
        const uint32_t sig = lpr_linux_first_pending_signal(deliverable);
        if (sig == 0) {
            break;
        }
        const uint64_t bit = lpr_linux_signal_bit(sig);
        lpr_linux_pending_signal_mask &= ~bit;

        const lpr_linux_sigaction_record_t *action = &lpr_linux_sigactions[sig];
        if (action->handler == LPR_LINUX_SIG_IGN ||
            (action->handler == LPR_LINUX_SIG_DFL && lpr_linux_default_signal_ignored(sig)))
        {
            continue;
        }
        if (action->handler == LPR_LINUX_SIG_DFL) {
            if (lpr_linux_default_signal_stops(sig)) {
                result = -LPR_LINUX_EINTR;
                break;
            }
            lpr_linux_exit_for_signal(sig);
        }
        result = -LPR_LINUX_EINTR;
        break;
    }

    lpr_linux_signal_dispatching = 0;
    return result;
}

static uint64_t lpr_linux_unblockable_signal_mask(void)
{
    return lpr_linux_signal_bit(LPR_LINUX_SIGKILL) |
        lpr_linux_signal_bit(LPR_LINUX_SIGSTOP);
}

static uint64_t lpr_linux_ignored_signal_mask(void)
{
    uint64_t ignored = 0;
    for (uint32_t sig = 1; sig <= LPR_LINUX_SIGNAL_MAX; sig += 1) {
        if (lpr_linux_sigactions[sig].handler == LPR_LINUX_SIG_IGN) {
            ignored |= lpr_linux_signal_bit(sig);
        }
    }
    return ignored;
}

static void lpr_fill_termd_signal_state(uint64_t *signal_mask, uint64_t *signal_ignored)
{
    if (signal_mask != 0) {
        *signal_mask = lpr_linux_signal_mask & ~lpr_linux_unblockable_signal_mask();
    }
    if (signal_ignored != 0) {
        *signal_ignored = lpr_linux_ignored_signal_mask();
    }
}

static int64_t lpr_tty_open_path(const char *path, uint64_t flags)
{
    if (path == 0 || LPR_TERMD_TTY_ENDPOINT_FD < 16) {
        return -LPR_LINUX_ENOENT;
    }
    uint64_t op = 0;
    uint64_t pts_index = 0;
    if (lpr_strcmp(path, "/dev/ptmx") == 0) {
        op = TERMD_WIRE_OP_OPEN_PTMX;
    } else if (lpr_strcmp(path, "/dev/tty") == 0) {
        op = TERMD_WIRE_OP_OPEN_CTTY;
    } else if (lpr_strcmp(path, "/dev/console") == 0) {
        op = TERMD_WIRE_OP_OPEN_HVC;
        pts_index = 0;
    } else {
        pts_index = lpr_parse_pts_index(path);
        if (pts_index != UINT64_MAX) {
            op = TERMD_WIRE_OP_OPEN_PTS;
        } else {
            pts_index = lpr_parse_hvc_index(path);
            if (pts_index == UINT64_MAX) {
                return -LPR_LINUX_ENOENT;
            }
            op = TERMD_WIRE_OP_OPEN_HVC;
        }
    }

    void *page = 0;
    int page_fd = -1;
    if (op == TERMD_WIRE_OP_OPEN_PTS ||
        op == TERMD_WIRE_OP_OPEN_HVC ||
        op == TERMD_WIRE_OP_OPEN_CTTY)
    {
        page_fd = lpr_create_tty_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        termd_wire_open_t *open_req = (termd_wire_open_t *)page;
        lpr_memset(open_req, 0, sizeof(*open_req));
        open_req->flags = flags;
        open_req->pts_index = pts_index;
        lpr_fill_termd_caller(
            &open_req->session_id,
            &open_req->process_id,
            &open_req->pgrp_id);
        lpr_fill_termd_signal_state(
            &open_req->signal_mask,
            &open_req->signal_ignored);
    }
    uint64_t handle = 0;
    const uint64_t word2 = op == TERMD_WIRE_OP_OPEN_PTMX ? flags : 0;
    const int64_t status = lpr_termd_call(op, page_fd, word2, &handle);
    if (page_fd >= 16) {
        lpr_destroy_tty_wire_page(page_fd, page);
    }
    if (status != 0) {
        return status;
    }
    const int fd = lpr_tty_fd_alloc(handle, flags);
    if (fd < 0) {
        (void)lpr_termd_call(TERMD_WIRE_OP_CLOSE, -1, handle, 0);
        return fd;
    }
    return fd;
}

static int lpr_load_bootstrap(void)
{
    if (lpr_bootstrap_checked) {
        return lpr_bootstrap_valid;
    }
    lpr_bootstrap_checked = 1;
    lpr_memset(&lpr_bootstrap, 0, sizeof(lpr_bootstrap));
    const int64_t got = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_FD_READ,
        LPR_BOOTSTRAP_FD,
        (uint64_t)(uintptr_t)&lpr_bootstrap,
        sizeof(lpr_bootstrap));
    if (got != (int64_t)sizeof(lpr_bootstrap) ||
        lpr_bootstrap.magic != LPR_BOOTSTRAP_MAGIC ||
        lpr_bootstrap.version != LPR_BOOTSTRAP_VERSION ||
        lpr_bootstrap.byte_size < sizeof(lpr_bootstrap) ||
        lpr_bootstrap.local_fd_count > LPR_FD_TABLE_MAX_SIZE ||
        lpr_bootstrap.local_fd_count > UINT64_MAX / sizeof(lpr_bootstrap_fd_t))
    {
        goto invalid;
    }

    const uint64_t expected_table_bytes =
        lpr_bootstrap.local_fd_count * sizeof(lpr_bootstrap_fd_t);
    if (lpr_bootstrap.local_fd_table_bytes != expected_table_bytes ||
        lpr_bootstrap.byte_size != sizeof(lpr_bootstrap) + expected_table_bytes)
    {
        goto invalid;
    }
    if (lpr_bootstrap.local_fd_count != 0) {
        if (lpr_bootstrap.local_fd_table_offset != sizeof(lpr_bootstrap) ||
            lpr_bootstrap.local_fd_table_offset > lpr_bootstrap.byte_size ||
            lpr_bootstrap.local_fd_table_bytes >
                lpr_bootstrap.byte_size - lpr_bootstrap.local_fd_table_offset)
        {
            goto invalid;
        }
        const int64_t mapped = lpr_pacha_syscall6(
            PACHAOS_SYSCALL_MMAP,
            LPR_BOOTSTRAP_FD,
            0,
            lpr_bootstrap.byte_size,
            PACHAOS_PROT_READ,
            PACHAOS_MMAP_SHARED,
            0);
        if (mapped < 4096) {
            goto invalid;
        }
        const struct lpr_bootstrap *mapped_bootstrap =
            (const struct lpr_bootstrap *)(uintptr_t)mapped;
        const lpr_bootstrap_fd_t *descs =
            (const lpr_bootstrap_fd_t *)((uintptr_t)mapped +
                lpr_bootstrap.local_fd_table_offset);
        const int install_ok =
            mapped_bootstrap->magic == LPR_BOOTSTRAP_MAGIC &&
            mapped_bootstrap->version == LPR_BOOTSTRAP_VERSION &&
            mapped_bootstrap->byte_size == lpr_bootstrap.byte_size &&
            mapped_bootstrap->local_fd_count == lpr_bootstrap.local_fd_count &&
            lpr_install_bootstrap_local_fds(descs, lpr_bootstrap.local_fd_count);
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)mapped,
            lpr_bootstrap.byte_size);
        if (!install_ok) {
            goto invalid;
        }
    } else if (!lpr_install_bootstrap_local_fds(0, 0)) {
        goto invalid;
    }
    if ((lpr_bootstrap.flags & LPR_BOOTSTRAP_FLAG_SUPERVISOR) != 0 &&
        lpr_bootstrap.supervisor_token != 0)
    {
        lpr_supervisor_token = lpr_bootstrap.supervisor_token;
        lpr_supervisor_enabled = 1;
        const int restore_status = lpr_supervisor_fd_table_restore(lpr_supervisor_token);
        if (restore_status != 0) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, 127);
            for (;;) {
            }
        }
    }
    lpr_bootstrap_valid = 1;
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, LPR_BOOTSTRAP_FD);
    return 1;

invalid:
    lpr_memset(&lpr_bootstrap, 0, sizeof(lpr_bootstrap));
    lpr_supervisor_enabled = 0;
    lpr_supervisor_token = 0;
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, LPR_BOOTSTRAP_FD);
    return 0;
}

static int lpr_path_is_terminated(const char *path, uint64_t capacity)
{
    return path != 0 && lpr_strnlen(path, capacity) < capacity;
}

static void lpr_cwd_set_root(void)
{
    lpr_cwd_handle = 0;
    lpr_memset(lpr_cwd_path, 0, sizeof(lpr_cwd_path));
    lpr_cwd_path[0] = '/';
}

static void lpr_cwd_init(void)
{
    if (lpr_cwd_checked) {
        return;
    }
    lpr_cwd_checked = 1;
    lpr_cwd_set_root();
    if (!lpr_load_bootstrap()) {
        return;
    }
    if (lpr_supervisor_enabled) {
        lprs_wire_process_state_t state;
        if (lpr_supervisor_get_state(&state) == 0 &&
            state.cwd[0] == '/' &&
            lpr_path_is_terminated(state.cwd, sizeof(state.cwd)))
        {
            const uint64_t len = (uint64_t)lpr_strnlen(state.cwd, sizeof(state.cwd));
            if (len < sizeof(lpr_cwd_path)) {
                lpr_memset(lpr_cwd_path, 0, sizeof(lpr_cwd_path));
                lpr_memcpy(lpr_cwd_path, state.cwd, (size_t)len + 1u);
                lpr_cwd_handle = state.cwd_handle;
            }
        }
        return;
    }
    if (lpr_bootstrap.cwd[0] == '/' &&
        lpr_path_is_terminated(lpr_bootstrap.cwd, sizeof(lpr_bootstrap.cwd)))
    {
        const uint64_t len = (uint64_t)lpr_strnlen(lpr_bootstrap.cwd, sizeof(lpr_bootstrap.cwd));
        if (len < sizeof(lpr_cwd_path)) {
            lpr_memset(lpr_cwd_path, 0, sizeof(lpr_cwd_path));
            lpr_memcpy(lpr_cwd_path, lpr_bootstrap.cwd, (size_t)len + 1u);
            lpr_cwd_handle = lpr_bootstrap.cwd_handle;
        }
    }
}

static void lpr_linux_process_state_init(void)
{
    if (lpr_linux_process_state_checked) {
        return;
    }
    lpr_linux_process_state_checked = 1;

    const int64_t kernel_pid_raw = lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID);
    int32_t fallback_pid = kernel_pid_raw > 0 && kernel_pid_raw <= INT32_MAX ?
        (int32_t)kernel_pid_raw :
        1;
    if (lpr_load_bootstrap()) {
        if (lpr_supervisor_enabled) {
            lprs_wire_process_state_t state;
            const int state_status = lpr_supervisor_get_state(&state);
            if (state_status != 0 ||
                state.pid == 0 ||
                state.pid > INT32_MAX ||
                state.ppid > INT32_MAX ||
                state.sid > INT32_MAX ||
                state.pgrp > INT32_MAX)
            {
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, 127);
                for (;;) {
                }
            } else {
                lpr_linux_current_pid = (int32_t)state.pid;
                lpr_linux_current_ppid = (int32_t)state.ppid;
                lpr_linux_current_sid = (int32_t)state.sid;
                lpr_linux_current_pgrp = (int32_t)state.pgrp;
                lpr_linux_next_pid = lpr_linux_current_pid + 1;
            }
        } else {
            lpr_linux_current_pid = (int32_t)lpr_bootstrap.linux_pid;
            if (lpr_linux_current_pid <= 0) {
                lpr_linux_current_pid = fallback_pid;
            }
            lpr_linux_current_ppid = (int32_t)lpr_bootstrap.linux_ppid;
            lpr_linux_current_sid = (int32_t)lpr_bootstrap.linux_sid;
            lpr_linux_current_pgrp = (int32_t)lpr_bootstrap.linux_pgrp;
            lpr_linux_next_pid = (int32_t)lpr_bootstrap.linux_next_pid;
        }
    } else {
        lpr_linux_current_pid = fallback_pid;
        lpr_linux_current_ppid = 0;
        lpr_linux_current_sid = lpr_linux_current_pid;
        lpr_linux_current_pgrp = lpr_linux_current_pid;
        lpr_linux_next_pid = lpr_linux_current_pid + 1;
    }
    if (lpr_linux_current_sid <= 0) {
        lpr_linux_current_sid = lpr_linux_current_pid;
    }
    if (lpr_linux_current_pgrp <= 0) {
        lpr_linux_current_pgrp = lpr_linux_current_pid;
    }
    if (lpr_linux_next_pid <= lpr_linux_current_pid) {
        lpr_linux_next_pid = lpr_linux_current_pid + 1;
    }
}

static lpr_linux_process_entry_t *lpr_linux_process_find(int32_t linux_pid)
{
    if (linux_pid <= 0) {
        return 0;
    }
    for (uint64_t i = 0; i < LPR_LINUX_PROCESS_TABLE_SIZE; i++) {
        if (lpr_linux_processes[i].active &&
            lpr_linux_processes[i].linux_pid == linux_pid)
        {
            return &lpr_linux_processes[i];
        }
    }
    return 0;
}

static lpr_linux_process_entry_t *lpr_linux_process_slot(void)
{
    for (uint64_t i = 0; i < LPR_LINUX_PROCESS_TABLE_SIZE; i++) {
        if (!lpr_linux_processes[i].active) {
            return &lpr_linux_processes[i];
        }
    }
    return 0;
}

static int32_t lpr_linux_alloc_child_pid(void)
{
    lpr_linux_process_state_init();
    for (uint64_t tries = 0; tries < 32768u; tries++) {
        int32_t pid = lpr_linux_next_pid++;
        if (pid <= 1) {
            lpr_linux_next_pid = 2;
            pid = lpr_linux_next_pid++;
        }
        if (pid == lpr_linux_current_pid || lpr_linux_process_find(pid) != 0) {
            continue;
        }
        return pid;
    }
    return -1;
}

static int lpr_linux_process_register(
    int32_t linux_pid,
    int32_t linux_ppid,
    int32_t linux_sid,
    int32_t linux_pgrp,
    int process_fd)
{
    if (linux_pid <= 0 || process_fd < 16) {
        return -LPR_LINUX_EINVAL;
    }
    lpr_linux_process_entry_t *entry = lpr_linux_process_find(linux_pid);
    if (entry == 0) {
        entry = lpr_linux_process_slot();
    }
    if (entry == 0) {
        return -LPR_LINUX_EAGAIN;
    }
    lpr_memset(entry, 0, sizeof(*entry));
    entry->active = 1;
    entry->linux_pid = linux_pid;
    entry->linux_ppid = linux_ppid;
    entry->linux_sid = linux_sid;
    entry->linux_pgrp = linux_pgrp;
    entry->process_fd = process_fd;
    lpr_trace_process_event("process_register", (uint64_t)(uint32_t)linux_pid, (uint64_t)(uint32_t)process_fd, 0);
    return 0;
}

static void lpr_linux_process_clear_children(void)
{
    lpr_memset(lpr_linux_processes, 0, sizeof(lpr_linux_processes));
}

static const char *lpr_take_boot_ctty_env(void)
{
    if (lpr_load_bootstrap() &&
        (lpr_bootstrap.flags & LPR_BOOTSTRAP_FLAG_DEFAULT_STDIO) != 0 &&
        lpr_bootstrap.ctty[0] != 0)
    {
        return lpr_bootstrap.ctty;
    }
    return 0;
}

static void lpr_close_non_linux_native_fd(uint64_t fd)
{
    if (fd > LPR_LINUX_FD_MAX ||
        lpr_runtime_reserved_fd(fd) ||
        lpr_fd_linux_visible_active(fd))
    {
        return;
    }
    struct pacha_fd_info info;
    if (lpr_native_fd_info(fd, &info)) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, fd);
    }
}

static int64_t lpr_install_stdio_fd_from_tty(uint64_t tty_fd, uint64_t target_fd)
{
    if (lpr_fd_linux_visible_active(target_fd)) {
        return 0;
    }
    lpr_close_non_linux_native_fd(target_fd);
    if (tty_fd == target_fd) {
        return 0;
    }
    return lpr_linux_dup2(tty_fd, target_fd, 0);
}

void lpr_linux_ensure_default_stdio(void)
{
    if (lpr_default_stdio_checked) {
        return;
    }
    lpr_default_stdio_checked = 1;

    const char *path = lpr_take_boot_ctty_env();
    if (path == 0 || path[0] == 0) {
        return;
    }
    const int need_stdin = !lpr_fd_linux_visible_active(0);
    const int need_stdout = !lpr_fd_linux_visible_active(1);
    const int need_stderr = !lpr_fd_linux_visible_active(2);
    if (!need_stdin && !need_stdout && !need_stderr) {
        return;
    }

    const int64_t tty_fd = lpr_tty_open_path(path, LPR_LINUX_O_RDWR);
    if (tty_fd < 0) {
        return;
    }
    if (need_stdin &&
        lpr_install_stdio_fd_from_tty((uint64_t)(uint32_t)tty_fd, 0) < 0)
    {
        (void)lpr_linux_close((uint64_t)(uint32_t)tty_fd);
        return;
    }
    if (need_stdout &&
        lpr_install_stdio_fd_from_tty((uint64_t)(uint32_t)tty_fd, 1) < 0)
    {
        (void)lpr_linux_close((uint64_t)(uint32_t)tty_fd);
        return;
    }
    if (need_stderr &&
        lpr_install_stdio_fd_from_tty((uint64_t)(uint32_t)tty_fd, 2) < 0)
    {
        (void)lpr_linux_close((uint64_t)(uint32_t)tty_fd);
        return;
    }
    if (tty_fd > 2) {
        (void)lpr_linux_close((uint64_t)(uint32_t)tty_fd);
    }
}

static int64_t lpr_tty_io(uint64_t op, uint64_t fd, uint64_t buf, uint64_t count)
{
    if (!lpr_linux_tty_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (buf == 0 && count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (count == 0) {
        return 0;
    }

    const uint32_t wait_events =
        op == TERMD_WIRE_OP_WRITE ? TERMD_WIRE_POLLOUT : TERMD_WIRE_POLLIN;
    for (;;) {
        void *page = 0;
        const int page_fd = lpr_create_tty_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        termd_wire_io_t *io = (termd_wire_io_t *)page;
        lpr_memset(io, 0, sizeof(*io));
        io->handle = lpr_tty_fds[fd].handle;
        io->length = count > TERMD_WIRE_IO_BYTES ? TERMD_WIRE_IO_BYTES : count;
        lpr_fill_termd_caller(&io->session_id, &io->process_id, &io->pgrp_id);
        lpr_fill_termd_signal_state(&io->signal_mask, &io->signal_ignored);
        if (op == TERMD_WIRE_OP_WRITE && io->length != 0) {
            lpr_memcpy(io->data, (const void *)(uintptr_t)buf, (size_t)io->length);
        }
        uint64_t result = 0;
        const int64_t status = lpr_termd_call(op, page_fd, 0, &result);
        if (status == 0 && op == TERMD_WIRE_OP_READ && result != 0) {
            lpr_memcpy((void *)(uintptr_t)buf, io->data, (size_t)result);
        }
        lpr_destroy_tty_wire_page(page_fd, page);
        if (status == 0) {
            return (int64_t)result;
        }
        if (status == -LPR_LINUX_EINTR) {
            lpr_linux_pump_tty_signals();
            return status;
        }
        if (status != -LPR_LINUX_EAGAIN ||
            (lpr_tty_fds[fd].flags & LPR_LINUX_O_NONBLOCK) != 0)
        {
            return status;
        }
        const int64_t wait_status = lpr_tty_wait(fd, wait_events);
        if (wait_status != 0) {
            return wait_status;
        }
    }
}

static int64_t lpr_tty_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
    if (!lpr_linux_tty_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    termd_wire_ioctl_t *ioctl_req = (termd_wire_ioctl_t *)page;
    lpr_memset(ioctl_req, 0, sizeof(*ioctl_req));
    ioctl_req->handle = lpr_tty_fds[fd].handle;
    ioctl_req->request = request;
    lpr_fill_termd_caller(
        &ioctl_req->session_id,
        &ioctl_req->process_id,
        &ioctl_req->pgrp_id);
    lpr_fill_termd_signal_state(
        &ioctl_req->signal_mask,
        &ioctl_req->signal_ignored);

    switch (request) {
    case LPR_LINUX_TCSETS:
    case LPR_LINUX_TCSETSW:
    case LPR_LINUX_TCSETSF:
        if (arg == 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EFAULT;
        }
        lpr_memcpy(ioctl_req->data, (const void *)(uintptr_t)arg, LPR_LINUX_TERMIOS_BYTES);
        break;
    case LPR_LINUX_TIOCSWINSZ:
        if (arg == 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EFAULT;
        }
        ioctl_req->arg0 = *(const uint16_t *)(uintptr_t)arg;
        ioctl_req->arg1 = *(const uint16_t *)((uintptr_t)arg + 2u);
        break;
    case LPR_LINUX_TIOCSPGRP:
    case LPR_LINUX_TIOCSPTLCK:
        if (arg == 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EFAULT;
        }
        ioctl_req->arg0 = (uint64_t)*(const int *)(uintptr_t)arg;
        lpr_memcpy(ioctl_req->data, (const void *)(uintptr_t)arg, sizeof(int));
        break;
    default:
        break;
    }

    uint64_t result = 0;
    const int64_t status = lpr_termd_call(TERMD_WIRE_OP_IOCTL, page_fd, 0, &result);
    if (status == 0) {
        switch (request) {
        case LPR_LINUX_TCGETS:
            if (arg == 0) {
                lpr_destroy_tty_wire_page(page_fd, page);
                return -LPR_LINUX_EFAULT;
            }
            lpr_memcpy((void *)(uintptr_t)arg, ioctl_req->data, LPR_LINUX_TERMIOS_BYTES);
            break;
        case LPR_LINUX_TIOCGWINSZ:
            if (arg == 0) {
                lpr_destroy_tty_wire_page(page_fd, page);
                return -LPR_LINUX_EFAULT;
            }
            *(uint16_t *)(uintptr_t)arg = (uint16_t)ioctl_req->result0;
            *(uint16_t *)((uintptr_t)arg + 2u) = (uint16_t)ioctl_req->result1;
            *(uint16_t *)((uintptr_t)arg + 4u) = 0;
            *(uint16_t *)((uintptr_t)arg + 6u) = 0;
            break;
        case LPR_LINUX_TIOCGPGRP:
        case LPR_LINUX_FIONREAD:
            if (arg == 0) {
                lpr_destroy_tty_wire_page(page_fd, page);
                return -LPR_LINUX_EFAULT;
            }
            *(int *)(uintptr_t)arg = (int)ioctl_req->result0;
            break;
        default:
            break;
        }
    }
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}

static int64_t lpr_iov_scalar_io(uint64_t fd, uint64_t iov_raw, uint64_t iov_count, int write)
{
    const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
    int64_t total = 0;
    for (uint64_t i = 0; i < iov_count; i += 1) {
        if (iov[i].len == 0) {
            continue;
        }
        const int64_t n = write ?
            lpr_linux_write(fd, iov[i].base, iov[i].len) :
            lpr_linux_read(fd, iov[i].base, iov[i].len);
        if (n < 0) {
            return total != 0 ? total : n;
        }
        if (n > INT64_MAX - total) {
            return -LPR_LINUX_EINVAL;
        }
        total += n;
        if ((uint64_t)n < iov[i].len) {
            break;
        }
    }
    return total;
}

uint32_t lpr_linux_tty_poll_events(uint64_t fd, uint32_t events)
{
    if (!lpr_linux_tty_fd_active(fd)) {
        return 0;
    }
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return 0;
    }
    termd_wire_poll_t *poll_req = (termd_wire_poll_t *)page;
    lpr_memset(poll_req, 0, sizeof(*poll_req));
    poll_req->handle = lpr_tty_fds[fd].handle;
    poll_req->events = events;
    lpr_fill_termd_caller(
        &poll_req->session_id,
        &poll_req->process_id,
        &poll_req->pgrp_id);
    lpr_fill_termd_signal_state(
        &poll_req->signal_mask,
        &poll_req->signal_ignored);
    uint64_t result = 0;
    const int64_t status = lpr_termd_call(TERMD_WIRE_OP_POLL, page_fd, 0, &result);
    uint32_t revents = status == 0 ? poll_req->revents : TERMD_WIRE_POLLERR;
    lpr_destroy_tty_wire_page(page_fd, page);
    return revents;
}

static int64_t lpr_tty_sleep_ms(uint64_t ms)
{
    if (ms == 0) {
        return 0;
    }
    struct pachaos_timespec ts;
    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (ms % 1000u) * 1000000ull;
    const int64_t status = lpr_pacha_syscall1(
        PACHAOS_SYSCALL_NANOSLEEP,
        (uint64_t)(uintptr_t)&ts);
    return status == 0 ? 0 : lpr_pacha_status_to_errno(status);
}

static int64_t lpr_tty_wait(uint64_t fd, uint32_t events)
{
    enum { LPR_TTY_WAIT_QUANTUM_MS = 10 };
    for (;;) {
        const uint32_t revents = lpr_linux_tty_poll_events(
            fd,
            events | TERMD_WIRE_POLLERR | TERMD_WIRE_POLLHUP);
        if ((revents & events) != 0) {
            return 0;
        }
        if ((revents & TERMD_WIRE_POLLERR) != 0) {
            return -LPR_LINUX_EIO;
        }
        if ((revents & TERMD_WIRE_POLLHUP) != 0) {
            return 0;
        }
        lpr_linux_pump_tty_signals();
        const int64_t sleep_status = lpr_tty_sleep_ms(LPR_TTY_WAIT_QUANTUM_MS);
        if (sleep_status != 0) {
            return sleep_status;
        }
    }
}

static int lpr_linux_signal_process_fd(int process_fd, uint32_t signo)
{
    if (process_fd < 16 || signo == 0 || signo > LPR_LINUX_SIGNAL_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    if (signo == LPR_LINUX_SIGKILL) {
        return lpr_pacha_status_to_errno(lpr_pacha_syscall2(
            PACHAOS_SYSCALL_PROCESS_KILL,
            (uint64_t)(uint32_t)process_fd,
            signo));
    }
    if (signo == LPR_LINUX_SIGSTOP) {
        return lpr_pacha_status_to_errno(lpr_pacha_syscall2(
            PACHAOS_SYSCALL_PROCESS_STOP,
            (uint64_t)(uint32_t)process_fd,
            signo));
    }
    return lpr_pacha_status_to_errno(lpr_pacha_syscall2(
        PACHAOS_SYSCALL_PROCESS_SIGNAL,
        (uint64_t)(uint32_t)process_fd,
        signo));
}

static int lpr_linux_signal_pgrp(int32_t pgrp, uint32_t signo)
{
    if (pgrp <= 0 || signo > LPR_LINUX_SIGNAL_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_supervisor_enabled) {
        const int64_t status = lpr_supervisor_kill_pid(-pgrp, signo, 0);
        return status == 0 ? 0 : (int)status;
    }
    int delivered = 0;
    if (lpr_linux_current_pgrp == pgrp) {
        if (signo != 0) {
            lpr_linux_queue_signal(signo);
        }
        delivered = 1;
    }
    for (uint64_t i = 0; i < LPR_LINUX_PROCESS_TABLE_SIZE; i++) {
        lpr_linux_process_entry_t *entry = &lpr_linux_processes[i];
        if (!entry->active || entry->process_fd < 16 || entry->linux_pgrp != pgrp) {
            continue;
        }
        if (signo != 0) {
            (void)lpr_linux_signal_process_fd(entry->process_fd, signo);
        }
        delivered = 1;
    }
    return delivered ? 0 : -LPR_LINUX_ESRCH;
}

static void lpr_linux_pump_tty_signals(void)
{
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return;
    }
    for (uint64_t i = 0; i < 8u; i++) {
        termd_wire_signal_t *signal_req = (termd_wire_signal_t *)page;
        lpr_memset(signal_req, 0, sizeof(*signal_req));
        uint64_t result = 0;
        const int64_t status = lpr_termd_call(TERMD_WIRE_OP_TAKE_SIGNAL, page_fd, 0, &result);
        if (status != 0 || result == 0 || signal_req->signo == 0) {
            break;
        }
        (void)lpr_linux_signal_pgrp((int32_t)signal_req->pgrp_id, signal_req->signo);
        (void)lpr_linux_dispatch_pending_signals();
    }
    lpr_destroy_tty_wire_page(page_fd, page);
}

uint32_t lpr_linux_eventfd_poll_events(uint64_t fd, uint32_t events)
{
    if (!lpr_linux_eventfd_active(fd)) {
        return 0;
    }
    uint32_t revents = 0;
    if ((events & 0x0001u) != 0 && lpr_event_fds[fd].counter != 0) {
        revents |= 0x0001u;
    }
    if ((events & 0x0004u) != 0 && lpr_event_fds[fd].counter != UINT64_MAX) {
        revents |= 0x0004u;
    }
    return revents;
}

static void lpr_pipe_close_fd(uint64_t fd)
{
    if (lpr_pipe_fd_is_active(fd)) {
        const uint64_t mode =
            (lpr_pipe_fds[fd].readable ? 1u : 0u) |
            (lpr_pipe_fds[fd].writable ? 2u : 0u) |
            ((uint64_t)lpr_pipe_fds[fd].flags << 8u);
        const int64_t status = lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, fd);
        lpr_trace_process_event("pipe_close", fd, mode, status);
    }
    lpr_memset(&lpr_pipe_fds[fd], 0, sizeof(lpr_pipe_fds[fd]));
}

static void lpr_pipe_after_fork_child(void)
{
    /* Kernel fd-table clone owns native pipe endpoint refcounts. */
    lpr_cwd_init();
    if (lpr_cwd_handle != 0) {
        uint64_t dup_handle = 0;
        const int64_t status = lpr_filed_dup_handle(lpr_cwd_handle, 0, &dup_handle);
        lpr_trace_process_event("fork_dup_cwd", lpr_cwd_handle, dup_handle, status);
        if (status == 0 && dup_handle != 0) {
            lpr_cwd_handle = dup_handle;
        }
    }
    lpr_fd_table_init();
    for (uint64_t fd = 0; fd < lpr_fd_table_capacity; fd += 1) {
        if (lpr_linux_tty_fd_active(fd)) {
            uint64_t dup_handle = 0;
            const int64_t status = lpr_termd_call(
                TERMD_WIRE_OP_DUP,
                -1,
                lpr_tty_fds[fd].handle,
                &dup_handle);
            lpr_trace_process_event("fork_dup_tty", fd, dup_handle, status);
            if (status == 0 && dup_handle != 0) {
                lpr_tty_fds[fd].handle = dup_handle;
            }
        }
        if (lpr_fd_is_filed(fd)) {
            void *page = 0;
            const int page_fd = lpr_create_wire_page(&page);
            if (page_fd < 0) {
                lpr_trace_process_event("fork_dup_filed_page", fd, 0, page_fd);
                continue;
            }
            filed_wire_handle_flags_t *flags = (filed_wire_handle_flags_t *)page;
            lpr_memset(flags, 0, sizeof(*flags));
            flags->handle = lpr_fds[fd].handle;
            flags->fd_flags =
                (lpr_fds[fd].flags & LPR_LINUX_O_CLOEXEC) != 0 ? FILED_WIRE_FD_CLOEXEC : 0;
            uint64_t dup_handle = 0;
            const int64_t status = lpr_filed_call(FILED_WIRE_OP_DUP, page_fd, 0, &dup_handle);
            lpr_destroy_wire_page(page_fd, page);
            lpr_trace_process_event("fork_dup_filed", fd, dup_handle, status);
            if (status == 0 && dup_handle != 0) {
                lpr_fds[fd].handle = dup_handle;
            }
        }
    }
}

void lpr_linux_apply_pending_fork_child(void)
{
    if (lpr_linux_pending_child_pid <= 0) {
        return;
    }
    const int32_t child_pid = lpr_linux_pending_child_pid;
    const int32_t child_ppid = lpr_linux_pending_child_ppid;
    const int32_t child_sid = lpr_linux_pending_child_sid;
    const int32_t child_pgrp = lpr_linux_pending_child_pgrp;
    const uint64_t child_token = lpr_supervisor_pending_child_token;
    lpr_linux_process_state_checked = 1;
    if (child_token != 0) {
        lpr_supervisor_token = child_token;
        lpr_supervisor_enabled = 1;
        (void)lpr_supervisor_call(
            LPRS_WIRE_OP_FORK_CHILD_READY,
            -1,
            lpr_supervisor_token,
            -1,
            0);
    }
    lpr_linux_current_pid = child_pid;
    lpr_linux_current_ppid = child_ppid;
    lpr_linux_current_sid = child_sid > 0 ? child_sid : child_pid;
    lpr_linux_current_pgrp = child_pgrp > 0 ? child_pgrp : child_pid;
    lpr_linux_pending_child_pid = 0;
    lpr_linux_pending_child_ppid = 0;
    lpr_linux_pending_child_sid = 0;
    lpr_linux_pending_child_pgrp = 0;
    lpr_supervisor_pending_child_token = 0;
    lpr_linux_process_clear_children();
    lpr_pipe_after_fork_child();
    lpr_trace_process_event("fork_child_state", (uint64_t)(uint32_t)child_pid, (uint64_t)(uint32_t)child_ppid, 0);
}

int64_t lpr_linux_pipe2(uint64_t fds_raw, uint64_t flags)
{
    lpr_trace_process_event("pipe2_begin", fds_raw, flags, 0);
    const uint64_t known_flags = LPR_LINUX_O_CLOEXEC | LPR_LINUX_O_NONBLOCK;
    if (fds_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t pair[2] = { 0, 0 };
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_PIPE_CREATE,
        (uint64_t)(uintptr_t)pair,
        lpr_pipe_flags_to_pacha(flags));
    if (status != 0) {
        return lpr_pacha_status_to_errno(status);
    }
    const uint64_t read_fd = pair[0];
    const uint64_t write_fd = pair[1];
    struct pacha_fd_info read_info;
    struct pacha_fd_info write_info;
    if (!lpr_native_pipe_slot_claimable(read_fd, &read_info) ||
        !lpr_native_pipe_slot_claimable(write_fd, &write_info))
    {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, read_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, write_fd);
        return -LPR_LINUX_EMFILE;
    }
    const int read_track = lpr_pipe_track_native_fd(read_fd, &read_info);
    const int write_track = read_track == 0 ? lpr_pipe_track_native_fd(write_fd, &write_info) : read_track;
    if (write_track != 0) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, read_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, write_fd);
        if (read_fd < lpr_fd_table_capacity) {
            lpr_memset(&lpr_pipe_fds[read_fd], 0, sizeof(lpr_pipe_fds[read_fd]));
        }
        if (write_fd < lpr_fd_table_capacity) {
            lpr_memset(&lpr_pipe_fds[write_fd], 0, sizeof(lpr_pipe_fds[write_fd]));
        }
        return write_track;
    }

    int *fds = (int *)(uintptr_t)fds_raw;
    fds[0] = (int)(uint32_t)read_fd;
    fds[1] = (int)(uint32_t)write_fd;
    lpr_trace_process_event("pipe2_end", read_fd, write_fd, 0);
    return 0;
}

int64_t lpr_linux_eventfd2(uint64_t initval, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_O_CLOEXEC | LPR_LINUX_O_NONBLOCK;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const int fd = lpr_fd_slot_alloc();
    if (fd < 0) {
        return fd;
    }
    lpr_event_fds[fd].active = 1;
    lpr_event_fds[fd].flags = (uint32_t)flags;
    lpr_event_fds[fd].counter = initval;
    return fd;
}

static int64_t lpr_linux_dup_into(uint64_t fd, int target_fd, uint64_t min_fd, uint64_t cloexec)
{
    int dup_fd = target_fd;
    lpr_trace_process_event(
        "dup_into_begin",
        fd,
        target_fd >= 0 ? (uint64_t)(uint32_t)target_fd : min_fd,
        target_fd >= 0 ? 1 : 0);
    if (dup_fd < 0) {
        dup_fd = lpr_fd_slot_alloc_from(min_fd);
        if (dup_fd < 0) {
            lpr_trace_process_event("dup_into_alloc_error", fd, min_fd, dup_fd);
            return dup_fd;
        }
    } else {
        if ((uint64_t)(uint32_t)dup_fd > LPR_LINUX_FD_MAX) {
            return -LPR_LINUX_EINVAL;
        }
        const int ensure_status = lpr_fd_table_ensure_fd((uint64_t)(uint32_t)dup_fd);
        if (ensure_status != 0) {
            return ensure_status;
        }
        if (!lpr_fd_slot_available((uint64_t)(uint32_t)dup_fd)) {
            lpr_trace_process_event("dup_into_target_busy", fd, (uint64_t)(uint32_t)dup_fd, -LPR_LINUX_EBADF);
            return -LPR_LINUX_EBADF;
        }
    }

    if (lpr_linux_eventfd_active(fd)) {
        lpr_event_fds[dup_fd] = lpr_event_fds[fd];
        if (cloexec) {
            lpr_event_fds[dup_fd].flags |= LPR_LINUX_O_CLOEXEC;
        } else {
            lpr_event_fds[dup_fd].flags &= ~LPR_LINUX_O_CLOEXEC;
        }
        return dup_fd;
    }
    if (lpr_pipe_fd_is_active(fd)) {
        uint64_t dup_flags = lpr_pipe_flags_to_pacha(lpr_pipe_fds[fd].flags & LPR_LINUX_O_NONBLOCK);
        if (cloexec) {
            dup_flags |= PACHA_FD_FLAG_CLOEXEC;
        }
        const uint64_t rights = lpr_pipe_rights(lpr_pipe_fds[fd].readable != 0);
        const int64_t native_dup = lpr_pacha_syscall4(
            PACHAOS_SYSCALL_FD_DUP,
            fd,
            (uint64_t)(uint32_t)dup_fd,
            rights,
            dup_flags);
        lpr_trace_process_event("dup_into_pipe_result", fd, (uint64_t)(uint32_t)dup_fd, native_dup);
        if (native_dup < 0) {
            return lpr_pacha_status_to_errno(native_dup);
        }
        const uint64_t native_dup_fd = (uint64_t)native_dup;
        if (native_dup_fd > LPR_LINUX_FD_MAX ||
            (target_fd >= 0 && native_dup_fd != (uint64_t)(uint32_t)dup_fd))
        {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, native_dup_fd);
            return target_fd >= 0 ? -LPR_LINUX_EINVAL : -LPR_LINUX_EMFILE;
        }
        struct pacha_fd_info dup_info;
        if (!lpr_native_pipe_slot_claimable(native_dup_fd, &dup_info)) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, native_dup_fd);
            return -LPR_LINUX_EMFILE;
        }
        const int track_status = lpr_pipe_track_native_fd(native_dup_fd, &dup_info);
        if (track_status != 0) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, native_dup_fd);
            return track_status;
        }
        return (int64_t)native_dup_fd;
    }
    if (lpr_linux_tty_fd_active(fd)) {
        uint64_t dup_handle = 0;
        const int64_t status = lpr_termd_call(
            TERMD_WIRE_OP_DUP,
            -1,
            lpr_tty_fds[fd].handle,
            &dup_handle);
        if (status != 0) {
            return status;
        }
        lpr_tty_fds[dup_fd].active = 1;
        lpr_tty_fds[dup_fd].flags =
            (lpr_tty_fds[fd].flags & ~LPR_LINUX_O_CLOEXEC) |
            (cloexec ? LPR_LINUX_O_CLOEXEC : 0);
        lpr_tty_fds[dup_fd].handle = dup_handle;
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
        lpr_fds[dup_fd].active = 1;
        lpr_fds[dup_fd].offset_valid = lpr_fds[fd].offset_valid;
        lpr_fds[dup_fd].pread_active = lpr_fds[fd].pread_active;
        lpr_fds[dup_fd].flags =
            (lpr_fds[fd].flags & ~LPR_LINUX_O_CLOEXEC) |
            (cloexec ? LPR_LINUX_O_CLOEXEC : 0);
        lpr_fds[dup_fd].handle = dup_handle;
        lpr_fds[dup_fd].offset = lpr_fds[fd].offset;
        return dup_fd;
    }
    {
        struct pacha_fd_info info;
        if (lpr_native_pipe_fd_info(fd, &info)) {
            uint64_t dup_flags = info.flags & PACHA_FD_FLAG_NONBLOCK;
            if (cloexec) {
                dup_flags |= PACHA_FD_FLAG_CLOEXEC;
            }
            const int64_t native_dup = lpr_pacha_syscall4(
                PACHAOS_SYSCALL_FD_DUP,
                fd,
                (uint64_t)(uint32_t)dup_fd,
                info.rights,
                dup_flags);
            lpr_trace_process_event("dup_into_native_pipe_result", fd, (uint64_t)(uint32_t)dup_fd, native_dup);
            if (native_dup < 0) {
                return lpr_pacha_status_to_errno(native_dup);
            }
            const uint64_t native_dup_fd = (uint64_t)native_dup;
            if (native_dup_fd > LPR_LINUX_FD_MAX ||
                (target_fd >= 0 && native_dup_fd != (uint64_t)(uint32_t)dup_fd))
            {
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, native_dup_fd);
                return target_fd >= 0 ? -LPR_LINUX_EINVAL : -LPR_LINUX_EMFILE;
            }
            struct pacha_fd_info dup_info;
            if (!lpr_native_pipe_slot_claimable(native_dup_fd, &dup_info)) {
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, native_dup_fd);
                return -LPR_LINUX_EMFILE;
            }
            const int track_status = lpr_pipe_track_native_fd(native_dup_fd, &dup_info);
            if (track_status != 0) {
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, native_dup_fd);
                return track_status;
            }
            return (int64_t)native_dup_fd;
        }
    }
    if (target_fd >= 0) {
        return -LPR_LINUX_EBADF;
    }
    return lpr_pacha_syscall4(PACHAOS_SYSCALL_FD_FCNTL, fd, PACHA_FD_FCNTL_DUP, 0, 0);
}

int64_t lpr_linux_dup(uint64_t fd, uint64_t min_fd, uint64_t cloexec)
{
    return lpr_linux_dup_into(fd, -1, min_fd, cloexec);
}

int64_t lpr_linux_dup2(uint64_t old_fd, uint64_t new_fd, uint64_t flags)
{
    lpr_trace_process_event("dup2_begin", old_fd, new_fd, (int64_t)flags);
    const uint64_t known_flags = LPR_LINUX_O_CLOEXEC;
    if ((flags & ~known_flags) != 0 || new_fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    if (!lpr_fd_is_filed(old_fd) &&
        !lpr_linux_tty_fd_active(old_fd) &&
        !lpr_pipe_fd_is_active(old_fd) &&
        !lpr_linux_eventfd_active(old_fd))
    {
        struct pacha_fd_info info;
        if (!lpr_native_pipe_fd_info(old_fd, &info)) {
            return -LPR_LINUX_EBADF;
        }
    }
    if (old_fd == new_fd) {
        return flags == 0 ? (int64_t)new_fd : -LPR_LINUX_EINVAL;
    }
    const int ensure_status = lpr_fd_table_ensure_fd(new_fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    if (lpr_fd_is_filed(new_fd) ||
        lpr_linux_tty_fd_active(new_fd) ||
        lpr_pipe_fd_is_active(new_fd) ||
        lpr_linux_eventfd_active(new_fd))
    {
        const int64_t close_status = lpr_linux_close(new_fd);
        if (close_status != 0 && close_status != -LPR_LINUX_EBADF) {
            lpr_trace_process_event("dup2_target_close_error", old_fd, new_fd, close_status);
        }
    } else {
        struct pacha_fd_info info;
        if (lpr_native_pipe_fd_info(new_fd, &info)) {
            const int64_t close_status = lpr_linux_close(new_fd);
            if (close_status != 0 && close_status != -LPR_LINUX_EBADF) {
                lpr_trace_process_event("dup2_target_close_error", old_fd, new_fd, close_status);
            }
        } else if (!lpr_runtime_reserved_fd(new_fd) &&
            lpr_native_fd_info(new_fd, &info))
        {
            const int64_t close_status =
                lpr_pacha_status_to_errno(lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, new_fd));
            if (close_status != 0 && close_status != -LPR_LINUX_EBADF) {
                lpr_trace_process_event("dup2_target_close_error", old_fd, new_fd, close_status);
            }
        }
    }
    const int64_t residual_close_status = lpr_close_native_fd_if_open(new_fd);
    lpr_trace_process_event("dup2_residual_close", old_fd, new_fd, residual_close_status);
    if (residual_close_status != 0 && residual_close_status != -LPR_LINUX_EBADF) {
        return residual_close_status;
    }
    const int64_t dup_status = lpr_linux_dup_into(old_fd, (int)new_fd, 0, (flags & LPR_LINUX_O_CLOEXEC) != 0);
    lpr_trace_process_event("dup2_end", old_fd, new_fd, dup_status);
    return dup_status;
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

static int lpr_create_tty_wire_page(void **out_page)
{
    if (out_page == 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_tty_wire_page_fd >= 16 && lpr_tty_wire_page != 0 && !lpr_tty_wire_page_busy) {
        lpr_tty_wire_page_busy = 1;
        *out_page = lpr_tty_wire_page;
        return lpr_tty_wire_page_fd;
    }
    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        TERMD_WIRE_PAGE_BYTES,
        rights,
        0);
    if (fd < 16) {
        return (int)lpr_pacha_status_to_errno(fd);
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)fd,
        0,
        TERMD_WIRE_PAGE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
        return (int)lpr_pacha_status_to_errno(mapped);
    }
    *out_page = (void *)(uintptr_t)mapped;
    if (!lpr_tty_wire_page_busy && lpr_tty_wire_page_fd < 16 && lpr_tty_wire_page == 0) {
        lpr_tty_wire_page_fd = (int)fd;
        lpr_tty_wire_page = *out_page;
        lpr_tty_wire_page_busy = 1;
    }
    return (int)fd;
}

static void lpr_destroy_tty_wire_page(int fd, void *page)
{
    if (fd == lpr_tty_wire_page_fd && page == lpr_tty_wire_page) {
        lpr_tty_wire_page_busy = 0;
        return;
    }
    if (page != 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, TERMD_WIRE_PAGE_BYTES);
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

static int64_t lpr_supervisor_call(
    uint64_t op,
    int page_fd,
    uint64_t word2,
    int transfer_fd,
    uint64_t *out_result)
{
    struct pacha_ipc_fd fds[2];
    uint64_t fd_count = 0;
    lpr_memset(fds, 0, sizeof(fds));
    if (page_fd >= 16) {
        fds[fd_count].fd = (uint64_t)(uint32_t)page_fd;
        fds[fd_count].rights =
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE;
        fd_count++;
    }
    if (transfer_fd >= 16) {
        fds[fd_count].fd = (uint64_t)(uint32_t)transfer_fd;
        fds[fd_count].rights =
            PACHA_FD_RIGHT_INSPECT |
            PACHA_FD_RIGHT_WAIT |
            PACHA_FD_RIGHT_POLL |
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_KILL;
        fd_count++;
    }

    const uint64_t request_id = ++lpr_request_id;
    const struct pacha_ipc_msg request = {
        .word0 = LPRS_WIRE_REQUEST_MAGIC,
        .word1 = op,
        .word2 = word2,
        .word3 = request_id,
        .fds = fd_count != 0 ? fds : 0,
        .fd_count = fd_count,
    };
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_SUPERVISOR_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16) {
        const int64_t err = lpr_pacha_status_to_errno(reply_fd);
        lpr_errconv_record(
            PACHA_ERRCONV_DOMAIN_KERNEL_STATUS,
            op,
            PACHA_ERRCONV_STAGE_CHILD_RPC_CALL,
            err,
            reply_fd,
            request_id,
            fd_count,
            LPR_SUPERVISOR_ENDPOINT_FD,
            0,
            "lpr supervisor ipc_call failed");
        return err;
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
        const int64_t err = lpr_pacha_status_to_errno(recv_status);
        lpr_errconv_record(
            PACHA_ERRCONV_DOMAIN_KERNEL_STATUS,
            op,
            PACHA_ERRCONV_STAGE_CHILD_RPC_RECV,
            err,
            recv_status,
            request_id,
            fd_count,
            (uint64_t)(uint32_t)reply_fd,
            0,
            "lpr supervisor reply recv failed");
        return err;
    }
    if (reply.word0 != LPRS_WIRE_REPLY_MAGIC || reply.word3 != request_id) {
        lpr_errconv_record(
            PACHA_ERRCONV_DOMAIN_LPRS_STATUS,
            op,
            PACHA_ERRCONV_STAGE_REPLY_MAGIC,
            -LPR_LINUX_EIO,
            (int64_t)reply.word0,
            request_id,
            fd_count,
            reply.word3,
            reply.word2,
            "lpr supervisor reply mismatch");
        return -LPR_LINUX_EIO;
    }
    if ((int64_t)reply.word1 < 0) {
        lpr_errconv_record(
            PACHA_ERRCONV_DOMAIN_LPRS_STATUS,
            op,
            PACHA_ERRCONV_STAGE_CHILD_STATUS,
            (int64_t)reply.word1,
            (int64_t)reply.word1,
            request_id,
            fd_count,
            0,
            reply.word2,
            "lpr supervisor returned error");
        return (int64_t)reply.word1;
    }
    if (out_result != 0) {
        *out_result = reply.word2;
    }
    return 0;
}

static int64_t lpr_supervisor_kill_pid(int32_t pid, uint32_t sig, uint64_t *out_delivered)
{
    if (!lpr_supervisor_enabled || lpr_supervisor_token == 0 || sig > LPR_LINUX_SIGNAL_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    void *page = 0;
    const int page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memset(page, 0, LPRS_WIRE_PAGE_BYTES);
    lprs_wire_kill_t *kill_req = (lprs_wire_kill_t *)page;
    kill_req->token = lpr_supervisor_token;
    kill_req->pid = pid;
    kill_req->signal = sig;
    const int64_t status = lpr_supervisor_call(
        LPRS_WIRE_OP_KILL,
        page_fd,
        lpr_supervisor_token,
        -1,
        0);
    if (status == 0 && out_delivered != 0) {
        *out_delivered = kill_req->delivered;
    }
    lpr_destroy_standalone_wire_page(page_fd, page);
    return status;
}

static int lpr_supervisor_get_state(lprs_wire_process_state_t *out_state)
{
    if (out_state == 0 || lpr_supervisor_token == 0) {
        return -LPR_LINUX_EINVAL;
    }
    void *page = 0;
    const int page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memset(page, 0, LPRS_WIRE_PAGE_BYTES);
    const int64_t status = lpr_supervisor_call(
        LPRS_WIRE_OP_GET_PROCESS_STATE,
        page_fd,
        lpr_supervisor_token,
        -1,
        0);
    if (status == 0) {
        lpr_memcpy(out_state, page, sizeof(*out_state));
    }
    lpr_destroy_standalone_wire_page(page_fd, page);
    return status == 0 ? 0 : (int)status;
}

static int lpr_supervisor_fd_table_restore(uint64_t token)
{
    if (token == 0) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t start = 0;
    for (;;) {
        void *page = 0;
        const int page_fd = lpr_create_standalone_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        lpr_memset(page, 0, LPRS_WIRE_PAGE_BYTES);
        lprs_wire_fd_table_page_t *table = (lprs_wire_fd_table_page_t *)page;
        table->token = token;
        table->start_index = start;
        const int64_t status = lpr_supervisor_call(
            LPRS_WIRE_OP_FD_TABLE_GET_CHUNK,
            page_fd,
            token,
            -1,
            0);
        if (status != 0) {
            lpr_destroy_standalone_wire_page(page_fd, page);
            return (int)status;
        }
        if (table->count > LPRS_WIRE_FD_TABLE_PAGE_MAX ||
            start > table->total_count ||
            table->count > table->total_count - start)
        {
            lpr_destroy_standalone_wire_page(page_fd, page);
            return -LPR_LINUX_EIO;
        }
        if (table->count != 0 &&
            !lpr_install_local_fd_descs((const lpr_bootstrap_fd_t *)table->entries, table->count))
        {
            lpr_destroy_standalone_wire_page(page_fd, page);
            return -LPR_LINUX_EIO;
        }
        const uint64_t got = table->count;
        start += got;
        const uint64_t total = table->total_count;
        lpr_destroy_standalone_wire_page(page_fd, page);
        if (start >= total) {
            return 0;
        }
        if (got == 0) {
            return -LPR_LINUX_EIO;
        }
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
        const int64_t err = lpr_pacha_status_to_errno(reply_fd);
        lpr_errconv_record(
            PACHA_ERRCONV_DOMAIN_KERNEL_STATUS,
            op,
            PACHA_ERRCONV_STAGE_CHILD_RPC_CALL,
            err,
            reply_fd,
            request_id,
            page_fd >= 16 ? 1u : 0u,
            LPR_FILED_ENDPOINT_FD,
            0,
            "filed ipc_call failed");
        return err;
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
        const int64_t err = lpr_pacha_status_to_errno(recv_status);
        lpr_errconv_record(
            PACHA_ERRCONV_DOMAIN_KERNEL_STATUS,
            op,
            PACHA_ERRCONV_STAGE_CHILD_RPC_RECV,
            err,
            recv_status,
            request_id,
            page_fd >= 16 ? 1u : 0u,
            (uint64_t)(uint32_t)reply_fd,
            0,
            "filed reply recv failed");
        return err;
    }
    if (reply.word0 != FILED_WIRE_REPLY_MAGIC || reply.word3 != request_id) {
        lpr_errconv_record(
            PACHA_ERRCONV_DOMAIN_FILED_STATUS,
            op,
            PACHA_ERRCONV_STAGE_REPLY_MAGIC,
            -LPR_LINUX_EIO,
            (int64_t)reply.word0,
            request_id,
            page_fd >= 16 ? 1u : 0u,
            reply.word3,
            reply.word2,
            "filed reply mismatch");
        return -LPR_LINUX_EIO;
    }
    if ((int64_t)reply.word1 < 0) {
        lpr_errconv_record(
            PACHA_ERRCONV_DOMAIN_FILED_STATUS,
            op,
            PACHA_ERRCONV_STAGE_CHILD_STATUS,
            (int64_t)reply.word1,
            (int64_t)reply.word1,
            request_id,
            page_fd >= 16 ? 1u : 0u,
            0,
            reply.word2,
            "filed returned error");
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
    if (path[0] == '/') {
        *out = 0;
        return 0;
    }
    if ((int64_t)dirfd == LPR_LINUX_AT_FDCWD) {
        lpr_cwd_init();
        *out = lpr_cwd_handle;
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

static int64_t lpr_filed_dup_handle(uint64_t handle, uint64_t fd_flags, uint64_t *out_handle)
{
    if (out_handle == 0) {
        return -LPR_LINUX_EFAULT;
    }
    *out_handle = 0;
    if (handle == 0) {
        return 0;
    }
    void *page = 0;
    const int page_fd = lpr_create_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    filed_wire_handle_flags_t *flags = (filed_wire_handle_flags_t *)page;
    lpr_memset(flags, 0, sizeof(*flags));
    flags->handle = handle;
    flags->fd_flags = fd_flags;
    uint64_t dup_handle = 0;
    const int64_t status = lpr_filed_call(FILED_WIRE_OP_DUP, page_fd, 0, &dup_handle);
    lpr_destroy_wire_page(page_fd, page);
    if (status != 0) {
        return status;
    }
    *out_handle = dup_handle;
    return 0;
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

static int64_t lpr_filed_open_handle_at(
    uint64_t dirfd,
    const char *path,
    uint64_t flags,
    uint64_t mode,
    uint64_t *out_handle)
{
    (void)mode;
    if (out_handle == 0) {
        return -LPR_LINUX_EFAULT;
    }
    *out_handle = 0;
    if ((flags & (LPR_LINUX_O_CREAT | LPR_LINUX_O_TRUNC)) != 0) {
        lpr_readlink_cache_clear();
        lpr_page_cache_clear();
    }
    void *page = 0;
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
    *out_handle = handle;
    return 0;
}

static int64_t lpr_linux_openat_once(uint64_t dirfd, uint64_t path_raw, uint64_t flags, uint64_t mode)
{
    const char *path = (const char *)(uintptr_t)path_raw;
    const int64_t tty_fd = lpr_tty_open_path(path, flags);
    if (tty_fd != -LPR_LINUX_ENOENT) {
        return tty_fd;
    }
    uint64_t handle = 0;
    const int64_t status = lpr_filed_open_handle_at(dirfd, path, flags, mode, &handle);
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

static void lpr_cwd_pop_component(char *path, uint64_t *len)
{
    if (path == 0 || len == 0 || *len <= 1u) {
        if (path != 0 && len != 0) {
            path[0] = '/';
            path[1] = 0;
            *len = 1;
        }
        return;
    }
    uint64_t i = *len;
    while (i > 1u && path[i - 1u] == '/') {
        i -= 1u;
    }
    while (i > 1u && path[i - 1u] != '/') {
        i -= 1u;
    }
    if (i <= 1u) {
        path[0] = '/';
        path[1] = 0;
        *len = 1;
        return;
    }
    path[i - 1u] = 0;
    *len = i - 1u;
}

static int lpr_cwd_append_component(char *out, uint64_t capacity, uint64_t *len, const char *component, uint64_t component_len)
{
    if (out == 0 || len == 0 || component == 0 || component_len == 0) {
        return 0;
    }
    uint64_t need = *len;
    if (need == 0) {
        if (capacity < 2u) {
            return 0;
        }
        out[0] = '/';
        out[1] = 0;
        need = 1;
    }
    if (need > 1u && out[need - 1u] != '/') {
        need += 1u;
    }
    if (need + component_len >= capacity) {
        return 0;
    }
    if (*len > 1u && out[*len - 1u] != '/') {
        out[*len] = '/';
        *len += 1u;
    }
    lpr_memcpy(out + *len, component, (size_t)component_len);
    *len += component_len;
    out[*len] = 0;
    return 1;
}

static int64_t lpr_cwd_normalize(const char *path, char *out, uint64_t capacity)
{
    if (path == 0 || out == 0 || capacity < 2u) {
        return -LPR_LINUX_EFAULT;
    }
    if (!lpr_path_is_terminated(path, FILED_WIRE_PATH_BYTES)) {
        return -LPR_LINUX_ENAMETOOLONG;
    }
    lpr_cwd_init();
    lpr_memset(out, 0, capacity);
    uint64_t len = 1;
    out[0] = '/';
    if (path[0] != '/') {
        const uint64_t cwd_len = (uint64_t)lpr_strnlen(lpr_cwd_path, sizeof(lpr_cwd_path));
        if (cwd_len == 0 || cwd_len >= capacity) {
            return -LPR_LINUX_ENAMETOOLONG;
        }
        lpr_memcpy(out, lpr_cwd_path, (size_t)cwd_len + 1u);
        len = cwd_len;
    }

    const char *cursor = path;
    while (*cursor != 0) {
        while (*cursor == '/') {
            cursor += 1;
        }
        const char *component = cursor;
        while (*cursor != 0 && *cursor != '/') {
            cursor += 1;
        }
        const uint64_t component_len = (uint64_t)(cursor - component);
        if (component_len == 0 ||
            (component_len == 1u && component[0] == '.'))
        {
            continue;
        }
        if (component_len == 2u && component[0] == '.' && component[1] == '.') {
            lpr_cwd_pop_component(out, &len);
            continue;
        }
        if (!lpr_cwd_append_component(out, capacity, &len, component, component_len)) {
            return -LPR_LINUX_ENAMETOOLONG;
        }
    }
    if (len == 0) {
        out[0] = '/';
        out[1] = 0;
    }
    return 0;
}

int64_t lpr_linux_getcwd(uint64_t buf, uint64_t size)
{
    if (buf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_cwd_init();
    const uint64_t len = (uint64_t)lpr_strnlen(lpr_cwd_path, sizeof(lpr_cwd_path));
    if (len + 1u > size) {
        return -LPR_LINUX_ERANGE;
    }
    lpr_memcpy((void *)(uintptr_t)buf, lpr_cwd_path, (size_t)len + 1u);
    return (int64_t)(len + 1u);
}

static int64_t lpr_supervisor_cwd_set(uint64_t handle, const char *path)
{
    if (!lpr_supervisor_enabled) {
        return 0;
    }
    void *page = 0;
    const int page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memset(page, 0, LPRS_WIRE_PAGE_BYTES);
    lprs_wire_cwd_t *cwd = (lprs_wire_cwd_t *)page;
    cwd->token = lpr_supervisor_token;
    cwd->cwd_handle = handle;
    if (path != 0) {
        const uint64_t len = (uint64_t)lpr_strnlen(path, LPRS_WIRE_CWD_BYTES);
        if (len >= LPRS_WIRE_CWD_BYTES) {
            lpr_destroy_standalone_wire_page(page_fd, page);
            return -LPR_LINUX_ENAMETOOLONG;
        }
        lpr_memcpy(cwd->cwd, path, (size_t)len + 1u);
    }
    const int64_t status = lpr_supervisor_call(
        LPRS_WIRE_OP_CWD_SET,
        page_fd,
        lpr_supervisor_token,
        -1,
        0);
    lpr_destroy_standalone_wire_page(page_fd, page);
    return status;
}

static int64_t lpr_cwd_install(uint64_t handle, const char *path)
{
    if (path == 0 || !lpr_path_is_terminated(path, sizeof(lpr_cwd_path))) {
        if (handle != 0) {
            (void)lpr_filed_close_handle(handle);
        }
        return -LPR_LINUX_ENAMETOOLONG;
    }
    lpr_cwd_init();
    const uint64_t old_handle = lpr_cwd_handle;
    char old_path[FILED_WIRE_PATH_BYTES];
    lpr_memcpy(old_path, lpr_cwd_path, sizeof(old_path));
    lpr_memset(lpr_cwd_path, 0, sizeof(lpr_cwd_path));
    const uint64_t len = (uint64_t)lpr_strnlen(path, sizeof(lpr_cwd_path));
    lpr_memcpy(lpr_cwd_path, path, (size_t)len + 1u);
    if (len == 1u && path[0] == '/') {
        lpr_cwd_handle = 0;
        if (handle != 0) {
            (void)lpr_filed_close_handle(handle);
        }
    } else {
        lpr_cwd_handle = handle;
    }
    const int64_t supervisor_status = lpr_supervisor_cwd_set(lpr_cwd_handle, lpr_cwd_path);
    if (supervisor_status != 0) {
        if (lpr_cwd_handle != 0 && lpr_cwd_handle != old_handle) {
            (void)lpr_filed_close_handle(lpr_cwd_handle);
        }
        lpr_cwd_handle = old_handle;
        lpr_memcpy(lpr_cwd_path, old_path, sizeof(lpr_cwd_path));
        return supervisor_status;
    }
    if (old_handle != 0) {
        (void)lpr_filed_close_handle(old_handle);
    }
    return 0;
}

int64_t lpr_linux_chdir(uint64_t path_raw)
{
    const char *path = (const char *)(uintptr_t)path_raw;
    if (path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    char normalized[FILED_WIRE_PATH_BYTES];
    int64_t status = lpr_cwd_normalize(path, normalized, sizeof(normalized));
    if (status != 0) {
        return status;
    }
    uint64_t handle = 0;
    status = lpr_filed_open_handle_at(
        (uint64_t)(int64_t)LPR_LINUX_AT_FDCWD,
        path,
        LPR_LINUX_O_RDONLY | LPR_LINUX_O_DIRECTORY,
        0,
        &handle);
    if (status != 0) {
        return status;
    }
    lpr_readlink_cache_clear();
    lpr_page_cache_clear();
    return lpr_cwd_install(handle, normalized);
}

int64_t lpr_linux_fchdir(uint64_t fd)
{
    if (!lpr_fd_is_filed(fd)) {
        return -LPR_LINUX_EBADF;
    }
    lpr_linux_stat_t st;
    const int64_t stat_status = lpr_linux_fstat(fd, (uint64_t)(uintptr_t)&st);
    if (stat_status != 0) {
        return stat_status;
    }
    if ((((uint64_t)st.st_mode) & LPR_LINUX_S_IFMT) != LPR_LINUX_S_IFDIR) {
        return -LPR_LINUX_ENOTDIR;
    }
    uint64_t dup_handle = 0;
    const int64_t dup_status = lpr_filed_dup_handle(lpr_fds[fd].handle, 0, &dup_handle);
    if (dup_status != 0) {
        return dup_status;
    }
    char cwd_copy[FILED_WIRE_PATH_BYTES];
    lpr_memset(cwd_copy, 0, sizeof(cwd_copy));
    lpr_cwd_init();
    lpr_memcpy(cwd_copy, lpr_cwd_path, sizeof(cwd_copy));
    return lpr_cwd_install(dup_handle, cwd_copy);
}

static int64_t lpr_linux_validate_timespec(const struct pachaos_timespec *ts)
{
    if (ts == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if ((int64_t)ts->tv_sec < 0 ||
        (int64_t)ts->tv_nsec < 0 ||
        ts->tv_nsec >= 1000000000ull)
    {
        return -LPR_LINUX_EINVAL;
    }
    return 0;
}

static int lpr_timespec_less_equal(
    const struct pachaos_timespec *lhs,
    const struct pachaos_timespec *rhs)
{
    if (lhs->tv_sec != rhs->tv_sec) {
        return lhs->tv_sec < rhs->tv_sec;
    }
    return lhs->tv_nsec <= rhs->tv_nsec;
}

static void lpr_timespec_subtract(
    const struct pachaos_timespec *end,
    const struct pachaos_timespec *start,
    struct pachaos_timespec *out)
{
    out->tv_sec = end->tv_sec - start->tv_sec;
    if (end->tv_nsec >= start->tv_nsec) {
        out->tv_nsec = end->tv_nsec - start->tv_nsec;
        return;
    }
    out->tv_sec -= 1u;
    out->tv_nsec = 1000000000ull + end->tv_nsec - start->tv_nsec;
}

static int64_t lpr_pacha_clock_gettime(uint64_t clock_id, struct pachaos_timespec *out)
{
    lpr_memset(out, 0, sizeof(*out));
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_CLOCK_GETTIME,
        clock_id,
        (uint64_t)(uintptr_t)out);
    return status == 0 ? 0 : lpr_pacha_status_to_errno(status);
}

static int64_t lpr_pacha_nanosleep(const struct pachaos_timespec *req)
{
    const int64_t valid = lpr_linux_validate_timespec(req);
    if (valid != 0) {
        return valid;
    }
    const int64_t status = lpr_pacha_syscall1(
        PACHAOS_SYSCALL_NANOSLEEP,
        (uint64_t)(uintptr_t)req);
    return status == 0 ? 0 : lpr_pacha_status_to_errno(status);
}

static int64_t lpr_linux_sleep_result(int64_t status)
{
    if (status != -LPR_LINUX_EAGAIN) {
        return status;
    }
    const int64_t signal_status = lpr_linux_dispatch_pending_signals();
    return signal_status != 0 ? signal_status : -LPR_LINUX_EINTR;
}

int64_t lpr_linux_nanosleep(uint64_t req_raw, uint64_t rem_raw)
{
    (void)rem_raw;
    const struct pachaos_timespec *req = (const struct pachaos_timespec *)(uintptr_t)req_raw;
    const int64_t valid = lpr_linux_validate_timespec(req);
    if (valid != 0) {
        return valid;
    }
    return lpr_linux_sleep_result(lpr_pacha_nanosleep(req));
}

int64_t lpr_linux_clock_nanosleep(uint64_t clock_id, uint64_t flags, uint64_t req_raw, uint64_t rem_raw)
{
    (void)rem_raw;
    if (clock_id != LPR_LINUX_CLOCK_REALTIME && clock_id != LPR_LINUX_CLOCK_MONOTONIC) {
        return -LPR_LINUX_EINVAL;
    }
    if ((flags & ~LPR_LINUX_TIMER_ABSTIME) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const struct pachaos_timespec *req = (const struct pachaos_timespec *)(uintptr_t)req_raw;
    const int64_t valid = lpr_linux_validate_timespec(req);
    if (valid != 0) {
        return valid;
    }
    if ((flags & LPR_LINUX_TIMER_ABSTIME) == 0) {
        return lpr_linux_sleep_result(lpr_pacha_nanosleep(req));
    }

    struct pachaos_timespec now;
    int64_t status = lpr_pacha_clock_gettime(clock_id, &now);
    if (status != 0) {
        return status;
    }
    if (lpr_timespec_less_equal(req, &now)) {
        return 0;
    }
    struct pachaos_timespec relative;
    lpr_memset(&relative, 0, sizeof(relative));
    lpr_timespec_subtract(req, &now, &relative);
    return lpr_linux_sleep_result(lpr_pacha_nanosleep(&relative));
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
    if (lpr_linux_tty_fd_active(fd)) {
        return lpr_tty_io(TERMD_WIRE_OP_READ, fd, buf, count);
    }
    if (lpr_linux_eventfd_active(fd)) {
        if (count < sizeof(uint64_t)) {
            return -LPR_LINUX_EINVAL;
        }
        if (buf == 0) {
            return -LPR_LINUX_EFAULT;
        }
        if (lpr_event_fds[fd].counter == 0) {
            return -LPR_LINUX_EAGAIN;
        }
        *(uint64_t *)(uintptr_t)buf = lpr_event_fds[fd].counter;
        lpr_event_fds[fd].counter = 0;
        return (int64_t)sizeof(uint64_t);
    }
    if (lpr_pipe_fd_is_active(fd)) {
        if (!lpr_pipe_fds[fd].readable) {
            return -LPR_LINUX_EBADF;
        }
        if (count == 0) {
            return 0;
        }
        if (buf == 0) {
            return -LPR_LINUX_EFAULT;
        }
        for (;;) {
            const int64_t n = lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_READ, fd, buf, count);
            if (n >= 0) {
                return n;
            }
            const int64_t err = lpr_pacha_status_to_errno(n);
            if (err != -LPR_LINUX_EAGAIN ||
                (lpr_pipe_fds[fd].flags & LPR_LINUX_O_NONBLOCK) != 0)
            {
                return err;
            }
            const int64_t wait_status = lpr_pipe_wait(fd, 0x0001u);
            if (wait_status != 0) {
                return wait_status;
            }
        }
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
    return lpr_native_pipe_read(fd, buf, count);
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
        if (!lpr_pipe_fds[fd].readable) {
            return -LPR_LINUX_EBADF;
        }
        for (;;) {
            const int64_t n = lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_READV, fd, iov_raw, iov_count);
            if (n >= 0) {
                return n;
            }
            const int64_t err = lpr_pacha_status_to_errno(n);
            if (err != -LPR_LINUX_EAGAIN ||
                (lpr_pipe_fds[fd].flags & LPR_LINUX_O_NONBLOCK) != 0)
            {
                return err;
            }
            const int64_t wait_status = lpr_pipe_wait(fd, 0x0001u);
            if (wait_status != 0) {
                return wait_status;
            }
        }
    }
    if (lpr_linux_tty_fd_active(fd)) {
        return lpr_iov_scalar_io(fd, iov_raw, iov_count, 0);
    }
    if (lpr_linux_eventfd_active(fd)) {
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
        return lpr_native_pipe_readv(fd, iov_raw, iov_count);
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
    enum { LPR_PROCESS_WAIT_POLL_TICKS = 50 };
    if (process_fd < 16) {
        return -LPR_LINUX_ECHILD;
    }
    lpr_trace_process_event("wait_begin", process_fd, 0, 0);
    lpr_pacha_process_status_t st;
    uint64_t waits = 0;
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
            lpr_trace_process_event("wait_end", process_fd, st.exit_code & 0xffu, 0);
            return 0;
        }
        const int64_t errno_status = lpr_pacha_status_to_errno(wait_status);
        if (errno_status != -LPR_LINUX_EAGAIN) {
            lpr_trace_process_event("wait_error", process_fd, waits, errno_status);
            return errno_status;
        }
        waits++;
        if (waits == 1 || waits == 16 || waits == 128) {
            lpr_trace_process_event("wait_pending", process_fd, waits, 0);
        }
        lpr_linux_pump_tty_signals();
        struct pacha_pollfd pollfd;
        lpr_memset(&pollfd, 0, sizeof(pollfd));
        pollfd.fd = (int)(uint32_t)process_fd;
        pollfd.events = PACHA_FD_EVENT_READABLE;
        (void)lpr_pacha_syscall4(
            PACHA_FD_SYSCALL_WAIT_MANY,
            (uint64_t)(uintptr_t)&pollfd,
            1,
            LPR_PROCESS_WAIT_POLL_TICKS,
            0);
    }
}

static int64_t lpr_linux_try_wait_process_fd(uint64_t process_fd, uint64_t *out_exit_code)
{
    if (process_fd < 16) {
        return -LPR_LINUX_ECHILD;
    }
    lpr_pacha_process_status_t st;
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
    return lpr_pacha_status_to_errno(wait_status);
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

static int64_t lpr_prepare_exec_cwd(filed_wire_exec_path_t *exec)
{
    if (exec == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_cwd_init();
    exec->dir_handle = lpr_cwd_handle;
    exec->cwd_handle = 0;
    const uint64_t cwd_len = (uint64_t)lpr_strnlen(lpr_cwd_path, sizeof(lpr_cwd_path));
    if (cwd_len == 0 || cwd_len >= LPR_BOOTSTRAP_CWD_BYTES) {
        return -LPR_LINUX_ENAMETOOLONG;
    }
    const int status = lpr_exec_add_string(exec, &exec->cwd, lpr_cwd_path);
    if (status != 0) {
        return status;
    }
    if (lpr_cwd_handle == 0) {
        return 0;
    }
    return lpr_filed_dup_handle(lpr_cwd_handle, 0, &exec->cwd_handle);
}

static void lpr_discard_exec_cwd(filed_wire_exec_path_t *exec)
{
    if (exec != 0 && exec->cwd_handle != 0) {
        (void)lpr_filed_close_handle(exec->cwd_handle);
        exec->cwd_handle = 0;
    }
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

static uint64_t lpr_exec_fd_table_capacity_for_count(uint64_t count)
{
    uint64_t capacity = LPR_EXEC_LOCAL_FD_TABLE_INITIAL_FDS;
    while (capacity < count) {
        if (capacity > UINT64_MAX / 2u) {
            return 0;
        }
        capacity *= 2u;
    }
    return capacity;
}

static uint64_t lpr_align_up_4096(uint64_t value)
{
    const uint64_t mask = 4096ull - 1ull;
    if (value > UINT64_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

static uint64_t lpr_exec_fd_table_bytes_for_capacity(uint64_t capacity)
{
    if (capacity >
        (UINT64_MAX - sizeof(filed_wire_exec_lpr_fd_table_t)) /
            sizeof(filed_wire_exec_lpr_fd_t))
    {
        return 0;
    }
    return lpr_align_up_4096(
        sizeof(filed_wire_exec_lpr_fd_table_t) +
        capacity * sizeof(filed_wire_exec_lpr_fd_t));
}

static int lpr_count_exec_local_fds(uint64_t *out_count)
{
    if (out_count == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_fd_table_init();
    uint64_t count = 0;
    for (uint64_t fd = 0; fd < lpr_fd_table_capacity; fd += 1) {
        if (lpr_fd_is_filed(fd)) {
            if ((lpr_fds[fd].flags & LPR_LINUX_O_CLOEXEC) == 0) {
                count++;
            }
            continue;
        }
        if (lpr_linux_tty_fd_active(fd)) {
            if ((lpr_tty_fds[fd].flags & LPR_LINUX_O_CLOEXEC) == 0) {
                count++;
            }
            continue;
        }
        if (lpr_linux_eventfd_active(fd)) {
            if ((lpr_event_fds[fd].flags & LPR_LINUX_O_CLOEXEC) == 0) {
                count++;
            }
            continue;
        }
        if (lpr_linux_socket_fd_active(fd) &&
            !lpr_linux_socket_fd_cloexec(fd))
        {
            return -LPR_LINUX_ENOTSUP;
        }
    }
    *out_count = count;
    return 0;
}

static void lpr_write_exec_local_fd_desc(filed_wire_exec_lpr_fd_t *desc, uint64_t fd)
{
    lpr_memset(desc, 0, sizeof(*desc));
    desc->fd = fd;
    if (lpr_fd_is_filed(fd)) {
        desc->kind = FILED_WIRE_EXEC_LPR_FD_FILED;
        desc->flags = lpr_fds[fd].flags;
        desc->handle = lpr_fds[fd].handle;
        desc->offset_or_counter = lpr_fds[fd].offset;
    } else if (lpr_linux_tty_fd_active(fd)) {
        desc->kind = FILED_WIRE_EXEC_LPR_FD_TTY;
        desc->flags = lpr_tty_fds[fd].flags;
        desc->handle = lpr_tty_fds[fd].handle;
    } else if (lpr_linux_eventfd_active(fd)) {
        desc->kind = FILED_WIRE_EXEC_LPR_FD_EVENT;
        desc->flags = lpr_event_fds[fd].flags;
        desc->offset_or_counter = lpr_event_fds[fd].counter;
    }
}

static int lpr_supervisor_fd_table_replace(void)
{
    if (!lpr_supervisor_enabled || lpr_supervisor_token == 0) {
        return 0;
    }
    uint64_t count = 0;
    int status = lpr_count_exec_local_fds(&count);
    if (status != 0) {
        return status;
    }
    int64_t call_status = lpr_supervisor_call(
        LPRS_WIRE_OP_FD_TABLE_REPLACE_BEGIN,
        -1,
        lpr_supervisor_token,
        -1,
        0);
    if (call_status != 0) {
        return (int)call_status;
    }

    uint64_t emitted = 0;
    void *page = 0;
    int page_fd = -1;
    lprs_wire_fd_table_page_t *table = 0;
    for (uint64_t fd = 0; fd < lpr_fd_table_capacity; fd += 1) {
        if (!lpr_fd_is_filed(fd) &&
            !lpr_linux_tty_fd_active(fd) &&
            !lpr_linux_eventfd_active(fd))
        {
            continue;
        }
        if (lpr_fd_is_filed(fd) && ((lpr_fds[fd].flags & LPR_LINUX_O_CLOEXEC) != 0)) {
            continue;
        }
        if (lpr_linux_tty_fd_active(fd) && ((lpr_tty_fds[fd].flags & LPR_LINUX_O_CLOEXEC) != 0)) {
            continue;
        }
        if (lpr_linux_eventfd_active(fd) && ((lpr_event_fds[fd].flags & LPR_LINUX_O_CLOEXEC) != 0)) {
            continue;
        }
        if (page == 0) {
            page_fd = lpr_create_standalone_wire_page(&page);
            if (page_fd < 0) {
                return page_fd;
            }
            lpr_memset(page, 0, LPRS_WIRE_PAGE_BYTES);
            table = (lprs_wire_fd_table_page_t *)page;
            table->token = lpr_supervisor_token;
            table->start_index = emitted;
            table->total_count = count;
        }
        filed_wire_exec_lpr_fd_t desc;
        lpr_write_exec_local_fd_desc(&desc, fd);
        lprs_wire_fd_desc_t *out = &table->entries[table->count++];
        out->fd = desc.fd;
        out->kind = desc.kind;
        out->flags = desc.flags;
        out->handle = desc.handle;
        out->offset_or_counter = desc.offset_or_counter;
        emitted++;
        if (table->count == LPRS_WIRE_FD_TABLE_PAGE_MAX || emitted == count) {
            call_status = lpr_supervisor_call(
                LPRS_WIRE_OP_FD_TABLE_REPLACE_CHUNK,
                page_fd,
                lpr_supervisor_token,
                -1,
                0);
            lpr_destroy_standalone_wire_page(page_fd, page);
            page = 0;
            page_fd = -1;
            table = 0;
            if (call_status != 0) {
                return (int)call_status;
            }
        }
    }
    if (page != 0) {
        lpr_destroy_standalone_wire_page(page_fd, page);
    }
    if (emitted != count) {
        return -LPR_LINUX_EIO;
    }
    call_status = lpr_supervisor_call(
        LPRS_WIRE_OP_FD_TABLE_REPLACE_COMMIT,
        -1,
        lpr_supervisor_token,
        -1,
        0);
    return call_status == 0 ? 0 : (int)call_status;
}

static int lpr_prepare_exec_local_fds(
    filed_wire_exec_path_t *exec,
    lpr_exec_local_fd_table_t *local_table)
{
    if (exec == 0 || local_table == 0) {
        return -LPR_LINUX_EFAULT;
    }
    local_table->fd = -1;
    local_table->map_bytes = 0;
    local_table->table = 0;
    exec->flags &= ~((uint64_t)FILED_WIRE_EXEC_LPR_FD_TABLE);
    exec->lpr_fd_table_bytes = 0;

    uint64_t count = 0;
    int status = lpr_count_exec_local_fds(&count);
    if (status != 0) {
        return status;
    }
    if (count == 0) {
        return 0;
    }

    const uint64_t capacity = lpr_exec_fd_table_capacity_for_count(count);
    if (capacity == 0 ||
        capacity > (UINT64_MAX - sizeof(filed_wire_exec_lpr_fd_table_t)) /
            sizeof(filed_wire_exec_lpr_fd_t))
    {
        return -LPR_LINUX_E2BIG;
    }
    const uint64_t map_bytes = lpr_exec_fd_table_bytes_for_capacity(capacity);
    if (map_bytes == 0) {
        return -LPR_LINUX_E2BIG;
    }
    const uint64_t used_bytes =
        sizeof(filed_wire_exec_lpr_fd_table_t) +
        count * sizeof(filed_wire_exec_lpr_fd_t);

    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE,
        map_bytes,
        rights,
        0);
    if (fd < 16) {
        return (int)lpr_pacha_status_to_errno(fd);
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)fd,
        0,
        map_bytes,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
        return (int)lpr_pacha_status_to_errno(mapped);
    }

    filed_wire_exec_lpr_fd_table_t *table =
        (filed_wire_exec_lpr_fd_table_t *)(uintptr_t)mapped;
    lpr_zero_bytes(table, map_bytes);
    table->magic = FILED_WIRE_EXEC_LPR_FD_TABLE_MAGIC;
    table->version = FILED_WIRE_EXEC_LPR_FD_TABLE_VERSION;
    table->byte_size = used_bytes;
    table->fd_count = count;

    filed_wire_exec_lpr_fd_t *entries =
        (filed_wire_exec_lpr_fd_t *)((uintptr_t)mapped + sizeof(*table));
    uint64_t index = 0;
    for (uint64_t fd_index = 0; fd_index < lpr_fd_table_capacity; fd_index += 1) {
        const int preserve =
            (lpr_fd_is_filed(fd_index) &&
                (lpr_fds[fd_index].flags & LPR_LINUX_O_CLOEXEC) == 0) ||
            (lpr_linux_tty_fd_active(fd_index) &&
                (lpr_tty_fds[fd_index].flags & LPR_LINUX_O_CLOEXEC) == 0) ||
            (lpr_linux_eventfd_active(fd_index) &&
                (lpr_event_fds[fd_index].flags & LPR_LINUX_O_CLOEXEC) == 0);
        if (!preserve) {
            continue;
        }
        if (index >= count) {
            (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)mapped, map_bytes);
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
            return -LPR_LINUX_EIO;
        }
        lpr_write_exec_local_fd_desc(&entries[index], fd_index);
        index++;
    }
    if (index != count) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)mapped, map_bytes);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
        return -LPR_LINUX_EIO;
    }

    local_table->fd = (int)fd;
    local_table->map_bytes = map_bytes;
    local_table->table = table;
    exec->flags |= FILED_WIRE_EXEC_LPR_FD_TABLE;
    exec->lpr_fd_table_bytes = map_bytes;
    return 0;
}

static void lpr_destroy_exec_local_fd_table(lpr_exec_local_fd_table_t *local_table)
{
    if (local_table == 0) {
        return;
    }
    if (local_table->table != 0 && local_table->map_bytes != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)local_table->table,
            local_table->map_bytes);
    }
    if (local_table->fd >= 16) {
        (void)lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE,
            (uint64_t)(uint32_t)local_table->fd);
    }
    local_table->fd = -1;
    local_table->map_bytes = 0;
    local_table->table = 0;
}

static void lpr_close_local_state_before_self_exec(void)
{
    lpr_fd_table_init();
    for (uint64_t fd = 0; fd < lpr_fd_table_capacity; fd += 1) {
        if (lpr_runtime_reserved_fd(fd)) {
            continue;
        }
        if (lpr_pipe_fd_is_active(fd)) {
            lpr_memset(&lpr_pipe_fds[fd], 0, sizeof(lpr_pipe_fds[fd]));
            continue;
        }
        if (lpr_linux_socket_fd_active(fd)) {
            (void)lpr_linux_socket_close(fd);
            continue;
        }
        if (lpr_linux_tty_fd_active(fd)) {
            const uint64_t handle = lpr_tty_fds[fd].handle;
            const uint32_t flags = lpr_tty_fds[fd].flags;
            lpr_memset(&lpr_tty_fds[fd], 0, sizeof(lpr_tty_fds[fd]));
            if ((flags & LPR_LINUX_O_CLOEXEC) != 0 && handle != 0) {
                (void)lpr_termd_call(TERMD_WIRE_OP_CLOSE, -1, handle, 0);
            }
            continue;
        }
        if (lpr_linux_eventfd_active(fd)) {
            lpr_memset(&lpr_event_fds[fd], 0, sizeof(lpr_event_fds[fd]));
            continue;
        }
        if (lpr_fd_is_filed(fd)) {
            const uint64_t handle = lpr_fds[fd].handle;
            const uint32_t flags = lpr_fds[fd].flags;
            lpr_memset(&lpr_fds[fd], 0, sizeof(lpr_fds[fd]));
            if ((flags & LPR_LINUX_O_CLOEXEC) != 0 && handle != 0) {
                (void)lpr_filed_close_handle(handle);
            }
        }
    }
}

void lpr_linux_prepare_process_exit(uint64_t exit_code)
{
    lpr_trace_process_event("exit_prepare", exit_code, 0, 0);
}

static int lpr_install_exec_bootstrap_fd(int bootstrap_fd)
{
    if (bootstrap_fd < 16) {
        return -LPR_LINUX_EBADF;
    }
    struct pacha_fd_info info;
    lpr_memset(&info, 0, sizeof(info));
    const int64_t info_status = lpr_pacha_syscall2(
        PACHA_FD_SYSCALL_GET_INFO,
        (uint64_t)(uint32_t)bootstrap_fd,
        (uint64_t)(uintptr_t)&info);
    if (info_status != 0) {
        return (int)lpr_pacha_status_to_errno(info_status);
    }
    const uint64_t flags =
        (info.flags & ~(uint64_t)PACHA_FD_FLAG_CLOEXEC) |
        PACHA_FD_FLAG_PRIVATE;
    const uint64_t flag_mask =
        PACHA_FD_FLAG_CLOEXEC |
        PACHA_FD_FLAG_NONBLOCK |
        PACHA_FD_FLAG_INHERIT |
        PACHA_FD_FLAG_PRIVATE;
    if (bootstrap_fd == LPR_BOOTSTRAP_FD) {
        const int64_t flag_status = lpr_pacha_syscall4(
            PACHA_FD_SYSCALL_FCNTL,
            (uint64_t)(uint32_t)bootstrap_fd,
            PACHA_FD_FCNTL_SET_FLAGS,
            flags,
            flag_mask);
        return flag_status == 0 ? 0 : (int)lpr_pacha_status_to_errno(flag_status);
    }
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, LPR_BOOTSTRAP_FD);
    const int64_t dup_fd = lpr_pacha_syscall4(
        PACHA_FD_SYSCALL_FCNTL,
        (uint64_t)(uint32_t)bootstrap_fd,
        PACHA_FD_FCNTL_DUP,
        LPR_BOOTSTRAP_FD,
        info.rights);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)bootstrap_fd);
    if (dup_fd != LPR_BOOTSTRAP_FD) {
        if (dup_fd >= 16) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)dup_fd);
        }
        return -LPR_LINUX_EIO;
    }
    const int64_t flag_status = lpr_pacha_syscall4(
        PACHA_FD_SYSCALL_FCNTL,
        LPR_BOOTSTRAP_FD,
        PACHA_FD_FCNTL_SET_FLAGS,
        flags,
        flag_mask);
    return flag_status == 0 ? 0 : (int)lpr_pacha_status_to_errno(flag_status);
}

static int64_t lpr_filed_exec_self(
    filed_wire_exec_path_t *exec,
    const lpr_exec_local_fd_table_t *local_table,
    int *out_process_fd,
    int *out_thread_fd,
    int *out_bootstrap_fd)
{
    if (exec == 0 || out_process_fd == 0 || out_thread_fd == 0 || out_bootstrap_fd == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (((exec->flags & FILED_WIRE_EXEC_LPR_FD_TABLE) != 0) &&
        (local_table == 0 || local_table->fd < 16 || local_table->table == 0))
    {
        return -LPR_LINUX_EINVAL;
    }
    *out_process_fd = -1;
    *out_thread_fd = -1;
    *out_bootstrap_fd = -1;

    void *page = 0;
    const int page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_memcpy(page, exec, sizeof(*exec));

    struct pacha_ipc_fd request_fds[2];
    lpr_memset(request_fds, 0, sizeof(request_fds));
    request_fds[0].fd = (uint64_t)(uint32_t)page_fd;
    request_fds[0].rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    uint64_t request_fd_count = 1;
    if ((exec->flags & FILED_WIRE_EXEC_LPR_FD_TABLE) != 0) {
        request_fds[1].fd = (uint64_t)(uint32_t)local_table->fd;
        request_fds[1].rights =
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ;
        request_fd_count = 2;
    }

    const uint64_t request_id = ++lpr_request_id;
    const struct pacha_ipc_msg request = {
        .word0 = FILED_WIRE_REQUEST_MAGIC,
        .word1 = FILED_WIRE_OP_EXEC_SELF,
        .word2 = 0,
        .word3 = request_id,
        .fds = request_fds,
        .fd_count = request_fd_count,
    };
    lpr_trace_process_event("exec_self_call", 0, 1, 0);
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        LPR_FILED_ENDPOINT_FD,
        (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16) {
        lpr_destroy_standalone_wire_page(page_fd, page);
        return lpr_pacha_status_to_errno(reply_fd);
    }

    struct pacha_ipc_fd reply_fds[3];
    struct pacha_ipc_msg reply;
    lpr_memset(reply_fds, 0, sizeof(reply_fds));
    lpr_memset(&reply, 0, sizeof(reply));
    reply.fds = reply_fds;
    reply.fd_capacity = 3;
    const int64_t recv_status = lpr_pacha_syscall4(
        PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)(uint32_t)reply_fd,
        (uint64_t)(uintptr_t)&reply,
        UINT64_MAX,
        0);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    lpr_destroy_standalone_wire_page(page_fd, page);
    if (recv_status != 0) {
        return lpr_pacha_status_to_errno(recv_status);
    }
    if (reply.word0 != FILED_WIRE_REPLY_MAGIC || reply.word3 != request_id) {
        return -LPR_LINUX_EIO;
    }
    if ((int64_t)reply.word1 < 0) {
        return (int64_t)reply.word1;
    }
    if (reply.fd_count < 3 ||
        reply_fds[0].fd < 16 ||
        reply_fds[1].fd < 16 ||
        reply_fds[2].fd < 16)
    {
        return -LPR_LINUX_EIO;
    }
    *out_process_fd = (int)(uint32_t)reply_fds[0].fd;
    *out_thread_fd = (int)(uint32_t)reply_fds[1].fd;
    *out_bootstrap_fd = (int)(uint32_t)reply_fds[2].fd;
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
    lpr_linux_process_state_init();
    int32_t child_pid = 0;
    uint64_t child_token = 0;
    if (lpr_supervisor_enabled) {
        void *page = 0;
        const int page_fd = lpr_create_standalone_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        lpr_memset(page, 0, LPRS_WIRE_PAGE_BYTES);
        lprs_wire_fork_t *fork_req = (lprs_wire_fork_t *)page;
        fork_req->parent_token = lpr_supervisor_token;
        const int64_t fork_status = lpr_supervisor_call(
            LPRS_WIRE_OP_FORK_BEGIN,
            page_fd,
            lpr_supervisor_token,
            -1,
            0);
        if (fork_status == 0 &&
            fork_req->child_pid != 0 &&
            fork_req->child_pid <= INT32_MAX &&
            fork_req->child_token != 0)
        {
            child_pid = (int32_t)fork_req->child_pid;
            child_token = fork_req->child_token;
        }
        lpr_destroy_standalone_wire_page(page_fd, page);
        if (fork_status != 0) {
            return fork_status;
        }
        if (child_pid <= 0 || child_token == 0) {
            return -LPR_LINUX_EIO;
        }
    } else {
        child_pid = lpr_linux_alloc_child_pid();
        if (child_pid <= 0) {
            return -LPR_LINUX_EAGAIN;
        }
    }
    lpr_linux_pending_child_pid = child_pid;
    lpr_linux_pending_child_ppid = lpr_linux_current_pid;
    lpr_linux_pending_child_sid = lpr_linux_current_sid;
    lpr_linux_pending_child_pgrp = lpr_linux_current_pgrp;
    lpr_supervisor_pending_child_token = child_token;
    lpr_trace_clone_frame("before_drop", user_frame, 0);
    lpr_filed_session_drop();
    lpr_trace_clone_frame("before_syscall", user_frame, 0);
    uint64_t child_process_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_KILL;
    if (lpr_supervisor_enabled) {
        child_process_rights |= PACHA_FD_RIGHT_TRANSFER;
    }
    const int64_t ret = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_PROCESS_CLONE,
        child_process_rights,
        PACHA_PROCESS_CLONE_CURRENT_THREAD | PACHA_PROCESS_CLONE_USER_FRAME,
        (uint64_t)(uintptr_t)user_frame);
    lpr_trace_clone_frame("after_syscall", user_frame, ret);
    if (ret == 0) {
        lpr_trace_process_event("clone_child", flags, child_stack, 0);
        lpr_linux_process_state_checked = 1;
        lpr_linux_current_pid = lpr_linux_pending_child_pid;
        lpr_linux_current_ppid = lpr_linux_pending_child_ppid;
        lpr_linux_current_sid = lpr_linux_pending_child_sid;
        lpr_linux_current_pgrp = lpr_linux_pending_child_pgrp;
        if (lpr_supervisor_pending_child_token != 0) {
            lpr_supervisor_token = lpr_supervisor_pending_child_token;
            lpr_supervisor_enabled = 1;
            (void)lpr_supervisor_call(
                LPRS_WIRE_OP_FORK_CHILD_READY,
                -1,
                lpr_supervisor_token,
                -1,
                0);
        }
        lpr_linux_pending_child_pid = 0;
        lpr_linux_pending_child_ppid = 0;
        lpr_linux_pending_child_sid = 0;
        lpr_linux_pending_child_pgrp = 0;
        lpr_supervisor_pending_child_token = 0;
        lpr_linux_process_clear_children();
        lpr_pipe_after_fork_child();
        return 0;
    }
    if (ret >= 16) {
        lpr_trace_process_event("clone_parent", flags, child_stack, ret);
        int reg_status = 0;
        if (lpr_supervisor_enabled && child_token != 0) {
            const int64_t supervisor_status = lpr_supervisor_call(
                LPRS_WIRE_OP_FORK_PARENT_REGISTER,
                -1,
                child_token,
                (int)(uint32_t)ret,
                0);
            reg_status = supervisor_status == 0 ? 0 : (int)supervisor_status;
        } else {
            reg_status = lpr_linux_process_register(
                child_pid,
                lpr_linux_current_pid,
                lpr_linux_current_sid,
                lpr_linux_current_pgrp,
                (int)(uint32_t)ret);
        }
        lpr_linux_pending_child_pid = 0;
        lpr_linux_pending_child_ppid = 0;
        lpr_linux_pending_child_sid = 0;
        lpr_linux_pending_child_pgrp = 0;
        lpr_supervisor_pending_child_token = 0;
        if (reg_status != 0) {
            (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_PROCESS_KILL, (uint64_t)(uint32_t)ret, 1);
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)ret);
            return reg_status;
        }
        if (lpr_supervisor_enabled && child_token != 0) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)ret);
        }
        return (int64_t)child_pid;
    }
    lpr_trace_process_event("clone_error", flags, child_stack, ret);
    lpr_linux_pending_child_pid = 0;
    lpr_linux_pending_child_ppid = 0;
    lpr_linux_pending_child_sid = 0;
    lpr_linux_pending_child_pgrp = 0;
    lpr_supervisor_pending_child_token = 0;
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

int64_t lpr_linux_getpid(void)
{
    lpr_linux_process_state_init();
    return lpr_linux_current_pid;
}

int64_t lpr_linux_getppid(void)
{
    lpr_linux_process_state_init();
    return lpr_linux_current_ppid;
}

int64_t lpr_linux_getpgrp(void)
{
    lpr_linux_process_state_init();
    return lpr_linux_current_pgrp;
}

int64_t lpr_linux_getpgid(uint64_t pid_raw)
{
    lpr_linux_process_state_init();
    const int32_t pid = (int32_t)(int64_t)pid_raw;
    if (pid < 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_supervisor_enabled) {
        void *page = 0;
        const int page_fd = lpr_create_standalone_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        lpr_memset(page, 0, LPRS_WIRE_PAGE_BYTES);
        lprs_wire_pid_op_t *op = (lprs_wire_pid_op_t *)page;
        op->token = lpr_supervisor_token;
        op->pid = pid;
        const int64_t status = lpr_supervisor_call(
            LPRS_WIRE_OP_GETPGID,
            page_fd,
            lpr_supervisor_token,
            -1,
            0);
        const int64_t result = status == 0 ? (int64_t)op->result : status;
        lpr_destroy_standalone_wire_page(page_fd, page);
        return result;
    }
    if (pid == 0 || pid == lpr_linux_current_pid) {
        return lpr_linux_current_pgrp;
    }
    lpr_linux_process_entry_t *entry = lpr_linux_process_find(pid);
    return entry != 0 ? entry->linux_pgrp : -LPR_LINUX_ESRCH;
}

int64_t lpr_linux_setpgid(uint64_t pid_raw, uint64_t pgid_raw)
{
    lpr_linux_process_state_init();
    const int32_t pid = (int32_t)(int64_t)pid_raw;
    int32_t pgid = (int32_t)(int64_t)pgid_raw;
    if (pid < 0 || pgid < 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_supervisor_enabled) {
        void *page = 0;
        const int page_fd = lpr_create_standalone_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        lpr_memset(page, 0, LPRS_WIRE_PAGE_BYTES);
        lprs_wire_pid_op_t *op = (lprs_wire_pid_op_t *)page;
        op->token = lpr_supervisor_token;
        op->pid = pid;
        op->value = pgid;
        const int64_t status = lpr_supervisor_call(
            LPRS_WIRE_OP_SETPGID,
            page_fd,
            lpr_supervisor_token,
            -1,
            0);
        if (status == 0 && (pid == 0 || pid == lpr_linux_current_pid)) {
            lpr_linux_current_pgrp = (int32_t)op->result;
        }
        lpr_destroy_standalone_wire_page(page_fd, page);
        return status;
    }
    const int32_t target_pid = pid == 0 ? lpr_linux_current_pid : pid;
    if (pgid == 0) {
        pgid = target_pid;
    }
    if (target_pid == lpr_linux_current_pid) {
        lpr_linux_current_pgrp = pgid;
        return 0;
    }
    lpr_linux_process_entry_t *entry = lpr_linux_process_find(target_pid);
    if (entry == 0) {
        return -LPR_LINUX_ESRCH;
    }
    entry->linux_pgrp = pgid;
    return 0;
}

int64_t lpr_linux_setsid(void)
{
    lpr_linux_process_state_init();
    if (lpr_supervisor_enabled) {
        void *page = 0;
        const int page_fd = lpr_create_standalone_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        lpr_memset(page, 0, LPRS_WIRE_PAGE_BYTES);
        lprs_wire_pid_op_t *op = (lprs_wire_pid_op_t *)page;
        op->token = lpr_supervisor_token;
        const int64_t status = lpr_supervisor_call(
            LPRS_WIRE_OP_SETSID,
            page_fd,
            lpr_supervisor_token,
            -1,
            0);
        if (status == 0) {
            lpr_linux_current_sid = (int32_t)op->result;
            lpr_linux_current_pgrp = lpr_linux_current_pid;
        }
        lpr_destroy_standalone_wire_page(page_fd, page);
        return status == 0 ? lpr_linux_current_sid : status;
    }
    if (lpr_linux_current_pgrp == lpr_linux_current_pid) {
        return -LPR_LINUX_EPERM;
    }
    lpr_linux_current_sid = lpr_linux_current_pid;
    lpr_linux_current_pgrp = lpr_linux_current_pid;
    return lpr_linux_current_sid;
}

int64_t lpr_linux_getsid(uint64_t pid_raw)
{
    lpr_linux_process_state_init();
    const int32_t pid = (int32_t)(int64_t)pid_raw;
    if (pid < 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_supervisor_enabled) {
        void *page = 0;
        const int page_fd = lpr_create_standalone_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        lpr_memset(page, 0, LPRS_WIRE_PAGE_BYTES);
        lprs_wire_pid_op_t *op = (lprs_wire_pid_op_t *)page;
        op->token = lpr_supervisor_token;
        op->pid = pid;
        const int64_t status = lpr_supervisor_call(
            LPRS_WIRE_OP_GETSID,
            page_fd,
            lpr_supervisor_token,
            -1,
            0);
        const int64_t result = status == 0 ? (int64_t)op->result : status;
        lpr_destroy_standalone_wire_page(page_fd, page);
        return result;
    }
    if (pid == 0 || pid == lpr_linux_current_pid) {
        return lpr_linux_current_sid;
    }
    lpr_linux_process_entry_t *entry = lpr_linux_process_find(pid);
    return entry != 0 ? entry->linux_sid : -LPR_LINUX_ESRCH;
}

int64_t lpr_linux_kill(uint64_t pid_raw, uint64_t sig_raw)
{
    lpr_linux_process_state_init();
    const int32_t pid = (int32_t)(int64_t)pid_raw;
    const uint32_t sig = (uint32_t)sig_raw;
    if (sig > LPR_LINUX_SIGNAL_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_supervisor_enabled) {
        const int64_t status = lpr_supervisor_kill_pid(pid, sig, 0);
        const int targets_current =
            pid == -1 ||
            pid == 0 ||
            pid == lpr_linux_current_pid ||
            (pid < -1 && lpr_linux_current_pgrp == -pid);
        if (status == 0 && targets_current && sig != 0) {
            const int64_t signal_status = lpr_linux_dispatch_pending_signals();
            return signal_status != 0 ? signal_status : 0;
        }
        return status;
    }
    if (pid == 0) {
        const int64_t status = lpr_linux_signal_pgrp(lpr_linux_current_pgrp, sig);
        if (status == 0) {
            const int64_t signal_status = lpr_linux_dispatch_pending_signals();
            return signal_status != 0 ? signal_status : 0;
        }
        return status;
    }
    if (pid < -1) {
        const int64_t status = lpr_linux_signal_pgrp(-pid, sig);
        if (status == 0) {
            const int64_t signal_status = lpr_linux_dispatch_pending_signals();
            return signal_status != 0 ? signal_status : 0;
        }
        return status;
    }
    if (pid == -1) {
        int delivered = 0;
        if (sig != 0) {
            lpr_linux_queue_signal(sig);
            (void)lpr_linux_dispatch_pending_signals();
        }
        delivered = 1;
        for (uint64_t i = 0; i < LPR_LINUX_PROCESS_TABLE_SIZE; i++) {
            lpr_linux_process_entry_t *entry = &lpr_linux_processes[i];
            if (!entry->active || entry->process_fd < 16) {
                continue;
            }
            if (sig != 0) {
                (void)lpr_linux_signal_process_fd(entry->process_fd, sig);
            }
            delivered = 1;
        }
        return delivered ? 0 : -LPR_LINUX_ESRCH;
    }
    if (pid == lpr_linux_current_pid) {
        if (sig != 0) {
            lpr_linux_queue_signal(sig);
            return lpr_linux_dispatch_pending_signals();
        }
        return 0;
    }
    lpr_linux_process_entry_t *entry = lpr_linux_process_find(pid);
    if (entry == 0) {
        return -LPR_LINUX_ESRCH;
    }
    return sig == 0 ? 0 : lpr_linux_signal_process_fd(entry->process_fd, sig);
}

int64_t lpr_linux_rt_sigaction(uint64_t sig_raw, uint64_t act_raw, uint64_t oldact_raw, uint64_t sigsetsize)
{
    const uint64_t sig = sig_raw;
    if (sig == 0 || sig > LPR_LINUX_SIGNAL_MAX || sigsetsize != 8u) {
        return -LPR_LINUX_EINVAL;
    }
    lpr_linux_sigaction_record_t *slot = &lpr_linux_sigactions[sig];
    if (oldact_raw != 0) {
        lpr_memcpy((void *)(uintptr_t)oldact_raw, slot, sizeof(*slot));
    }
    if (act_raw != 0) {
        lpr_memcpy(slot, (const void *)(uintptr_t)act_raw, sizeof(*slot));
    }
    return 0;
}

int64_t lpr_linux_rt_sigprocmask(uint64_t how, uint64_t set_raw, uint64_t oldset_raw, uint64_t sigsetsize)
{
    if (sigsetsize != 8u) {
        return -LPR_LINUX_EINVAL;
    }
    if (oldset_raw != 0) {
        *(uint64_t *)(uintptr_t)oldset_raw = lpr_linux_signal_mask;
    }
    if (set_raw == 0) {
        return 0;
    }
    const uint64_t set = *(const uint64_t *)(uintptr_t)set_raw;
    switch (how) {
    case LPR_LINUX_SIG_BLOCK:
        lpr_linux_signal_mask |= set;
        lpr_linux_signal_mask &= ~lpr_linux_unblockable_signal_mask();
        return 0;
    case LPR_LINUX_SIG_UNBLOCK:
        lpr_linux_signal_mask &= ~set;
        lpr_linux_signal_mask &= ~lpr_linux_unblockable_signal_mask();
        return lpr_linux_dispatch_pending_signals();
    case LPR_LINUX_SIG_SETMASK:
        lpr_linux_signal_mask = set & ~lpr_linux_unblockable_signal_mask();
        return lpr_linux_dispatch_pending_signals();
    default:
        return -LPR_LINUX_EINVAL;
    }
}

int64_t lpr_linux_wait4(uint64_t pid, uint64_t status_raw, uint64_t options, uint64_t rusage)
{
    (void)rusage;
    if ((options & ~(LPR_LINUX_WNOHANG | LPR_LINUX_WUNTRACED | LPR_LINUX_WCONTINUED)) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    lpr_linux_process_state_init();
    const int32_t requested = (int32_t)(int64_t)pid;
    lpr_trace_process_event("wait4_request", (uint64_t)(uint32_t)requested, options, 0);
    if (lpr_supervisor_enabled) {
        void *page = 0;
        const int page_fd = lpr_create_standalone_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        lpr_memset(page, 0, LPRS_WIRE_PAGE_BYTES);
        lprs_wire_wait4_t *wait_req = (lprs_wire_wait4_t *)page;
        wait_req->token = lpr_supervisor_token;
        wait_req->requested_pid = requested;
        wait_req->options = options & LPR_LINUX_WNOHANG;
        uint64_t packed_result = 0;
        const int64_t wait_status = lpr_supervisor_call(
            LPRS_WIRE_OP_WAIT4,
            page_fd,
            lpr_supervisor_token,
            -1,
            &packed_result);
        int64_t result = 0;
        if (wait_status == 0) {
            const uint32_t result_pid = LPRS_WIRE_WAIT4_RESULT_PID(packed_result);
            const uint32_t wait_result_status = LPRS_WIRE_WAIT4_RESULT_STATUS(packed_result);
            if (result_pid == 0) {
                result = 0;
            } else {
                if (status_raw != 0) {
                    *(int *)(uintptr_t)status_raw = (int)wait_result_status;
                }
                result = (int64_t)result_pid;
            }
        } else {
            result = wait_status;
        }
        lpr_destroy_standalone_wire_page(page_fd, page);
        return result;
    }
    lpr_linux_process_entry_t *selected = 0;
    for (uint64_t i = 0; i < LPR_LINUX_PROCESS_TABLE_SIZE; i++) {
        lpr_linux_process_entry_t *entry = &lpr_linux_processes[i];
        if (!entry->active || entry->process_fd < 16) {
            continue;
        }
        if (requested > 0 && entry->linux_pid != requested) {
            continue;
        }
        if (requested == 0 && entry->linux_pgrp != lpr_linux_current_pgrp) {
            continue;
        }
        if (requested < -1 && entry->linux_pgrp != -requested) {
            continue;
        }
        selected = entry;
        break;
    }
    if (selected == 0) {
        lpr_trace_process_event("wait4_nochild", (uint64_t)(uint32_t)requested, options, -LPR_LINUX_ECHILD);
        return -LPR_LINUX_ECHILD;
    }

    uint64_t exit_code = 0;
    int64_t status = 0;
    lpr_trace_process_event(
        "wait4_selected",
        (uint64_t)(uint32_t)selected->linux_pid,
        (uint64_t)(uint32_t)selected->process_fd,
        0);
    if ((options & LPR_LINUX_WNOHANG) != 0) {
        lpr_linux_pump_tty_signals();
        status = lpr_linux_try_wait_process_fd((uint64_t)(uint32_t)selected->process_fd, &exit_code);
        if (status == -LPR_LINUX_EAGAIN) {
            return 0;
        }
    } else {
        status = lpr_linux_wait_process_fd((uint64_t)(uint32_t)selected->process_fd, &exit_code);
    }
    if (status != 0) {
        return status;
    }
    if (status_raw != 0) {
        int *out_status = (int *)(uintptr_t)status_raw;
        *out_status = (int)((exit_code & 0xffu) << 8);
    }
    const int32_t linux_pid = selected->linux_pid;
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)selected->process_fd);
    lpr_memset(selected, 0, sizeof(*selected));
    lpr_trace_process_event("wait4_reaped", (uint64_t)(uint32_t)linux_pid, exit_code & 0xffu, 0);
    return (int64_t)linux_pid;
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
    exec.flags = FILED_WIRE_EXEC_LINUX_LPR | FILED_WIRE_EXEC_LINUX_BOOTSTRAP | FILED_WIRE_EXEC_SELF;
    lpr_memcpy(exec.path, path, (size_t)path_len + 1u);
    int status = (int)lpr_prepare_exec_cwd(&exec);
    if (status != 0) {
        return status;
    }
    lpr_linux_process_state_init();
    exec.linux_pid = (uint64_t)(uint32_t)lpr_linux_current_pid;
    exec.linux_ppid = (uint64_t)(uint32_t)lpr_linux_current_ppid;
    exec.linux_sid = (uint64_t)(uint32_t)lpr_linux_current_sid;
    exec.linux_pgrp = (uint64_t)(uint32_t)lpr_linux_current_pgrp;
    exec.linux_next_pid = (uint64_t)(uint32_t)lpr_linux_next_pid;
    if (lpr_supervisor_enabled) {
        exec.lpr_supervisor_token = lpr_supervisor_token;
        exec.lpr_fd_table_token = lpr_supervisor_token;
    }
    lpr_trace_process_event("execve_begin", path_len, 0, 0);

    status = lpr_exec_copy_string_vector(
        &exec,
        exec.argv,
        FILED_WIRE_EXEC_MAX_ARGS,
        argv_raw,
        &exec.argc);
    if (status != 0) {
        lpr_discard_exec_cwd(&exec);
        return status;
    }
    if (exec.argc == 0) {
        status = lpr_exec_add_string(&exec, &exec.argv[0], path);
        if (status != 0) {
            lpr_discard_exec_cwd(&exec);
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
        lpr_discard_exec_cwd(&exec);
        return status;
    }

    lpr_exec_local_fd_table_t local_table;
    local_table.fd = -1;
    local_table.map_bytes = 0;
    local_table.table = 0;
    status = lpr_supervisor_enabled ?
        lpr_supervisor_fd_table_replace() :
        lpr_prepare_exec_local_fds(&exec, &local_table);
    if (status != 0) {
        lpr_discard_exec_cwd(&exec);
        return status;
    }

    int process_fd = -1;
    int thread_fd = -1;
    int bootstrap_fd = -1;
    const int64_t exec_status =
        lpr_filed_exec_self(&exec, &local_table, &process_fd, &thread_fd, &bootstrap_fd);
    lpr_destroy_exec_local_fd_table(&local_table);
    if (exec_status != 0) {
        lpr_discard_exec_cwd(&exec);
        lpr_trace_process_event("execve_error", path_len, 0, exec_status);
        return exec_status;
    }
    status = lpr_install_exec_bootstrap_fd(bootstrap_fd);
    if (status != 0) {
        lpr_discard_exec_cwd(&exec);
        if (bootstrap_fd >= 16) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)bootstrap_fd);
        }
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_PROCESS_KILL, (uint64_t)(uint32_t)process_fd, 1);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)thread_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)process_fd);
        return status;
    }
    lpr_trace_process_event("execve_commit", (uint64_t)(uint32_t)process_fd, (uint64_t)(uint32_t)thread_fd, 0);
    if (lpr_supervisor_enabled) {
        (void)lpr_supervisor_call(
            LPRS_WIRE_OP_EXEC_COMMIT_BEGIN,
            -1,
            lpr_supervisor_token,
            -1,
            0);
    }
    lpr_close_local_state_before_self_exec();
    const int64_t commit_status = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_PROCESS_EXEC_FROM,
        (uint64_t)(uint32_t)process_fd,
        (uint64_t)(uint32_t)thread_fd,
        0);
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_PROCESS_KILL, (uint64_t)(uint32_t)process_fd, 1);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)thread_fd);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)process_fd);
    if (commit_status != 0) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, LPR_BOOTSTRAP_FD);
        return lpr_pacha_status_to_errno(commit_status);
    }
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
    if (lpr_linux_tty_fd_active(fd)) {
        return lpr_tty_io(TERMD_WIRE_OP_WRITE, fd, buf, count);
    }
    if (lpr_linux_eventfd_active(fd)) {
        if (count < sizeof(uint64_t)) {
            return -LPR_LINUX_EINVAL;
        }
        if (buf == 0) {
            return -LPR_LINUX_EFAULT;
        }
        const uint64_t value = *(const uint64_t *)(uintptr_t)buf;
        if (value == UINT64_MAX || lpr_event_fds[fd].counter > UINT64_MAX - value) {
            return -LPR_LINUX_EAGAIN;
        }
        lpr_event_fds[fd].counter += value;
        return (int64_t)sizeof(uint64_t);
    }
    if (lpr_pipe_fd_is_active(fd)) {
        if (!lpr_pipe_fds[fd].writable) {
            return -LPR_LINUX_EBADF;
        }
        if (count == 0) {
            return 0;
        }
        if (buf == 0) {
            return -LPR_LINUX_EFAULT;
        }
        for (;;) {
            const int64_t n = lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, fd, buf, count);
            if (n >= 0) {
                return n;
            }
            const int64_t err = lpr_pacha_status_to_errno(n);
            if (err == -LPR_LINUX_EPIPE) {
                return err;
            }
            if (err != -LPR_LINUX_EAGAIN ||
                (lpr_pipe_fds[fd].flags & LPR_LINUX_O_NONBLOCK) != 0)
            {
                return err;
            }
            const int64_t wait_status = lpr_pipe_wait(fd, 0x0004u);
            if (wait_status != 0) {
                return wait_status;
            }
        }
    }
    if (lpr_fd_is_filed(fd)) {
        if (count != 0) {
            lpr_page_cache_invalidate_handle(lpr_fds[fd].handle);
        }
        return lpr_filed_io(FILED_WIRE_OP_WRITE, fd, buf, count, 0);
    }
    {
        const int64_t native_status = lpr_native_pipe_write(fd, buf, count);
        if (native_status != -LPR_LINUX_EBADF) {
            return native_status;
        }
    }
    return -LPR_LINUX_EBADF;
}

int64_t lpr_linux_writev(uint64_t fd, uint64_t iov_raw, uint64_t iov_count)
{
    if (iov_raw == 0 && iov_count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (lpr_linux_tty_fd_active(fd)) {
        return lpr_iov_scalar_io(fd, iov_raw, iov_count, 1);
    }
    if (!lpr_fd_is_filed(fd)) {
        if (lpr_pipe_fd_is_active(fd)) {
            if (!lpr_pipe_fds[fd].writable) {
                return -LPR_LINUX_EBADF;
            }
            for (;;) {
                const int64_t n = lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITEV, fd, iov_raw, iov_count);
                if (n >= 0) {
                    return n;
                }
                const int64_t err = lpr_pacha_status_to_errno(n);
                if (err == -LPR_LINUX_EPIPE) {
                    return err;
                }
                if (err != -LPR_LINUX_EAGAIN ||
                    (lpr_pipe_fds[fd].flags & LPR_LINUX_O_NONBLOCK) != 0)
                {
                    return err;
                }
                const int64_t wait_status = lpr_pipe_wait(fd, 0x0004u);
                if (wait_status != 0) {
                    return wait_status;
                }
            }
        }
        if (lpr_linux_eventfd_active(fd)) {
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
        {
            const int64_t native_status = lpr_native_pipe_writev(fd, iov_raw, iov_count);
            if (native_status != -LPR_LINUX_EBADF) {
                return native_status;
            }
        }
        return -LPR_LINUX_EBADF;
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
    if (lpr_linux_tty_fd_active(fd)) {
        const uint64_t handle = lpr_tty_fds[fd].handle;
        lpr_memset(&lpr_tty_fds[fd], 0, sizeof(lpr_tty_fds[fd]));
        return lpr_termd_call(TERMD_WIRE_OP_CLOSE, -1, handle, 0);
    }
    if (lpr_linux_eventfd_active(fd)) {
        lpr_memset(&lpr_event_fds[fd], 0, sizeof(lpr_event_fds[fd]));
        return 0;
    }
    if (lpr_pipe_fd_is_active(fd)) {
        lpr_pipe_close_fd(fd);
        return 0;
    }
    {
        struct pacha_fd_info info;
        if (lpr_native_pipe_fd_info(fd, &info)) {
            return lpr_pacha_status_to_errno(lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, fd));
        }
    }
    if (lpr_fd_is_filed(fd)) {
        const uint64_t handle = lpr_fds[fd].handle;
        lpr_memset(&lpr_fds[fd], 0, sizeof(lpr_fds[fd]));
        return lpr_filed_close_handle(handle);
    }
    if (fd >= 3 && fd < LPR_FD_TABLE_MAX_SIZE) {
        return -LPR_LINUX_EBADF;
    }
    return lpr_pacha_status_to_errno(lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, fd));
}

int64_t lpr_linux_close_range(uint64_t first, uint64_t last, uint64_t flags)
{
    const uint64_t known_flags =
        LPR_LINUX_CLOSE_RANGE_UNSHARE |
        LPR_LINUX_CLOSE_RANGE_CLOEXEC;
    if (first > last || (flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    lpr_fd_table_init();
    const int cloexec = (flags & LPR_LINUX_CLOSE_RANGE_CLOEXEC) != 0;
    uint64_t local_last = last;
    if (local_last >= lpr_fd_table_capacity) {
        local_last = lpr_fd_table_capacity == 0 ? 0 : lpr_fd_table_capacity - 1u;
    }
    if (first <= local_last) {
        for (uint64_t fd = first; fd <= local_last; fd += 1) {
            if (lpr_runtime_reserved_fd(fd)) {
                continue;
            }
            if (cloexec) {
                if (lpr_linux_tty_fd_active(fd)) {
                    lpr_tty_fds[fd].flags |= LPR_LINUX_O_CLOEXEC;
                } else if (lpr_linux_eventfd_active(fd)) {
                    lpr_event_fds[fd].flags |= LPR_LINUX_O_CLOEXEC;
                } else if (lpr_pipe_fd_is_active(fd)) {
                    const int64_t status = lpr_pacha_syscall3(
                        PACHAOS_SYSCALL_FD_SET_FLAGS,
                        fd,
                        PACHA_FD_FLAG_CLOEXEC,
                        PACHA_FD_FLAG_CLOEXEC);
                    if (status == 0) {
                        lpr_pipe_fds[fd].flags |= LPR_LINUX_O_CLOEXEC;
                    }
                } else if (lpr_fd_is_filed(fd)) {
                    lpr_fds[fd].flags |= LPR_LINUX_O_CLOEXEC;
                } else if (lpr_linux_socket_fd_active(fd)) {
                    (void)lpr_linux_socket_fcntl(fd, LPR_LINUX_F_SETFD, LPR_LINUX_FD_CLOEXEC);
                }
                continue;
            }
            if (lpr_fd_local_active(fd) || lpr_linux_socket_fd_active(fd)) {
                (void)lpr_linux_close(fd);
            }
        }
    }
    const uint64_t native_last =
        last >= LPR_FD_TABLE_INITIAL_SIZE ? LPR_FD_TABLE_INITIAL_SIZE - 1u : last;
    if (first <= native_last) {
        for (uint64_t fd = first; fd <= native_last; fd += 1) {
            if (lpr_runtime_reserved_fd(fd) ||
                lpr_fd_local_active(fd) ||
                lpr_linux_socket_fd_active(fd))
            {
                continue;
            }
            if (cloexec) {
                (void)lpr_pacha_syscall3(
                    PACHAOS_SYSCALL_FD_SET_FLAGS,
                    fd,
                    PACHA_FD_FLAG_CLOEXEC,
                    PACHA_FD_FLAG_CLOEXEC);
            } else {
                (void)lpr_close_native_fd_if_open(fd);
            }
        }
    }
    return 0;
}

int64_t lpr_linux_lseek(uint64_t fd, uint64_t offset, uint64_t whence)
{
    if (lpr_linux_tty_fd_active(fd)) {
        return -LPR_LINUX_ESPIPE;
    }
    if (lpr_pipe_fd_is_active(fd)) {
        return -LPR_LINUX_ESPIPE;
    }
    {
        struct pacha_fd_info info;
        if (lpr_native_pipe_fd_info(fd, &info)) {
            return -LPR_LINUX_ESPIPE;
        }
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
    if (lpr_linux_tty_fd_active(fd)) {
        switch (cmd) {
        case LPR_LINUX_F_GETFD:
            return (lpr_tty_fds[fd].flags & LPR_LINUX_O_CLOEXEC) != 0 ? LPR_LINUX_FD_CLOEXEC : 0;
        case LPR_LINUX_F_SETFD:
            if ((arg & LPR_LINUX_FD_CLOEXEC) != 0) {
                lpr_tty_fds[fd].flags |= LPR_LINUX_O_CLOEXEC;
            } else {
                lpr_tty_fds[fd].flags &= ~LPR_LINUX_O_CLOEXEC;
            }
            return 0;
        case LPR_LINUX_F_GETFL:
            return lpr_tty_fds[fd].flags;
        case LPR_LINUX_F_SETFL:
            lpr_tty_fds[fd].flags = (uint32_t)arg;
            return 0;
        case LPR_LINUX_F_DUPFD:
        case LPR_LINUX_F_DUPFD_CLOEXEC:
            return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
        default:
            return -LPR_LINUX_EINVAL;
        }
    }
    if (lpr_linux_eventfd_active(fd)) {
        switch (cmd) {
        case LPR_LINUX_F_GETFD:
            return (lpr_event_fds[fd].flags & LPR_LINUX_O_CLOEXEC) != 0 ? LPR_LINUX_FD_CLOEXEC : 0;
        case LPR_LINUX_F_SETFD:
            if ((arg & LPR_LINUX_FD_CLOEXEC) != 0) {
                lpr_event_fds[fd].flags |= LPR_LINUX_O_CLOEXEC;
            } else {
                lpr_event_fds[fd].flags &= ~LPR_LINUX_O_CLOEXEC;
            }
            return 0;
        case LPR_LINUX_F_GETFL:
            return lpr_event_fds[fd].flags;
        case LPR_LINUX_F_SETFL:
            lpr_event_fds[fd].flags = (uint32_t)((lpr_event_fds[fd].flags & LPR_LINUX_O_CLOEXEC) |
                (arg & LPR_LINUX_O_NONBLOCK));
            return 0;
        case LPR_LINUX_F_DUPFD:
        case LPR_LINUX_F_DUPFD_CLOEXEC:
            return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
        default:
            return -LPR_LINUX_EINVAL;
        }
    }
    if (lpr_pipe_fd_is_active(fd)) {
        switch (cmd) {
        case LPR_LINUX_F_GETFD:
            return (lpr_pipe_fds[fd].flags & LPR_LINUX_O_CLOEXEC) != 0 ? LPR_LINUX_FD_CLOEXEC : 0;
        case LPR_LINUX_F_SETFD: {
            const uint64_t pacha_flags = (arg & LPR_LINUX_FD_CLOEXEC) != 0 ? PACHA_FD_FLAG_CLOEXEC : 0;
            const int64_t status = lpr_pacha_syscall3(
                PACHAOS_SYSCALL_FD_SET_FLAGS,
                fd,
                pacha_flags,
                PACHA_FD_FLAG_CLOEXEC);
            if (status != 0) {
                return lpr_pacha_status_to_errno(status);
            }
            if ((arg & LPR_LINUX_FD_CLOEXEC) != 0) {
                lpr_pipe_fds[fd].flags |= LPR_LINUX_O_CLOEXEC;
            } else {
                lpr_pipe_fds[fd].flags &= ~LPR_LINUX_O_CLOEXEC;
            }
            return 0;
        }
        case LPR_LINUX_F_GETFL:
            return lpr_pipe_fds[fd].flags;
        case LPR_LINUX_F_SETFL: {
            const uint64_t pacha_flags = (arg & LPR_LINUX_O_NONBLOCK) != 0 ? PACHA_FD_FLAG_NONBLOCK : 0;
            const int64_t status = lpr_pacha_syscall3(
                PACHAOS_SYSCALL_FD_SET_FLAGS,
                fd,
                pacha_flags,
                PACHA_FD_FLAG_NONBLOCK);
            if (status != 0) {
                return lpr_pacha_status_to_errno(status);
            }
            lpr_pipe_fds[fd].flags = (uint32_t)((lpr_pipe_fds[fd].flags & ~LPR_LINUX_O_NONBLOCK) |
                (arg & LPR_LINUX_O_NONBLOCK));
            return 0;
        }
        case LPR_LINUX_F_DUPFD:
        case LPR_LINUX_F_DUPFD_CLOEXEC:
            return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
        default:
            return -LPR_LINUX_EINVAL;
        }
    }
    {
        struct pacha_fd_info info;
        if (lpr_native_pipe_fd_info(fd, &info)) {
            switch (cmd) {
            case LPR_LINUX_F_GETFD:
                return (info.flags & PACHA_FD_FLAG_CLOEXEC) != 0 ? LPR_LINUX_FD_CLOEXEC : 0;
            case LPR_LINUX_F_SETFD: {
                const uint64_t pacha_flags = (arg & LPR_LINUX_FD_CLOEXEC) != 0 ? PACHA_FD_FLAG_CLOEXEC : 0;
                const int64_t status = lpr_pacha_syscall3(
                    PACHAOS_SYSCALL_FD_SET_FLAGS,
                    fd,
                    pacha_flags,
                    PACHA_FD_FLAG_CLOEXEC);
                return status == 0 ? 0 : lpr_pacha_status_to_errno(status);
            }
            case LPR_LINUX_F_GETFL: {
                uint64_t flags = 0;
                const int readable = (info.rights & PACHA_FD_RIGHT_READ) != 0;
                const int writable = (info.rights & PACHA_FD_RIGHT_WRITE) != 0;
                if (readable && writable) {
                    flags |= LPR_LINUX_O_RDWR;
                } else if (writable) {
                    flags |= LPR_LINUX_O_WRONLY;
                }
                if ((info.flags & PACHA_FD_FLAG_NONBLOCK) != 0) {
                    flags |= LPR_LINUX_O_NONBLOCK;
                }
                return (int64_t)flags;
            }
            case LPR_LINUX_F_SETFL: {
                const uint64_t pacha_flags = (arg & LPR_LINUX_O_NONBLOCK) != 0 ? PACHA_FD_FLAG_NONBLOCK : 0;
                const int64_t status = lpr_pacha_syscall3(
                    PACHAOS_SYSCALL_FD_SET_FLAGS,
                    fd,
                    pacha_flags,
                    PACHA_FD_FLAG_NONBLOCK);
                return status == 0 ? 0 : lpr_pacha_status_to_errno(status);
            }
            case LPR_LINUX_F_DUPFD:
            case LPR_LINUX_F_DUPFD_CLOEXEC:
                return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
            default:
                return -LPR_LINUX_EINVAL;
            }
        }
    }
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
    case LPR_LINUX_F_GETLK:
        if (arg == 0) {
            return -LPR_LINUX_EFAULT;
        }
        *(int16_t *)(uintptr_t)arg = LPR_LINUX_F_UNLCK;
        return 0;
    case LPR_LINUX_F_SETLK:
    case LPR_LINUX_F_SETLKW:
        return arg != 0 ? 0 : -LPR_LINUX_EFAULT;
    case LPR_LINUX_F_DUPFD:
    case LPR_LINUX_F_DUPFD_CLOEXEC:
        return lpr_linux_dup(fd, arg, cmd == LPR_LINUX_F_DUPFD_CLOEXEC);
    default:
        return -LPR_LINUX_EINVAL;
    }
}

int64_t lpr_linux_flock(uint64_t fd, uint64_t operation)
{
    if (!lpr_fd_is_filed(fd) && !lpr_linux_eventfd_active(fd) && !lpr_linux_tty_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    const uint64_t lock_op = operation & ~(uint64_t)LPR_LINUX_LOCK_NB;
    switch (lock_op) {
    case LPR_LINUX_LOCK_SH:
    case LPR_LINUX_LOCK_EX:
    case LPR_LINUX_LOCK_UN:
        return 0;
    default:
        return -LPR_LINUX_EINVAL;
    }
}

int64_t lpr_linux_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
    if (lpr_linux_tty_fd_active(fd)) {
        return lpr_tty_ioctl(fd, request, arg);
    }
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
    if (lpr_linux_eventfd_active(fd)) {
        lpr_linux_stat_t *st = (lpr_linux_stat_t *)(uintptr_t)statbuf;
        lpr_memset(st, 0, sizeof(*st));
        st->st_ino = fd + 1u;
        st->st_nlink = 1;
        st->st_mode = LPR_LINUX_S_IFIFO | 0600u;
        st->st_blksize = 4096;
        return 0;
    }
    if (lpr_linux_tty_fd_active(fd)) {
        lpr_linux_stat_t *st = (lpr_linux_stat_t *)(uintptr_t)statbuf;
        lpr_memset(st, 0, sizeof(*st));
        st->st_ino = 0x74747900ull + fd;
        st->st_nlink = 1;
        st->st_mode = LPR_LINUX_S_IFCHR | 0620u;
        st->st_rdev = 0x8800ull;
        st->st_blksize = 4096;
        return 0;
    }
    if (lpr_pipe_fd_is_active(fd)) {
        lpr_linux_stat_t *st = (lpr_linux_stat_t *)(uintptr_t)statbuf;
        lpr_memset(st, 0, sizeof(*st));
        st->st_ino = fd + 1u;
        st->st_nlink = 1;
        st->st_mode = LPR_LINUX_S_IFIFO | 0600u;
        st->st_blksize = 4096;
        return 0;
    }
    {
        struct pacha_fd_info info;
        if (lpr_native_pipe_fd_info(fd, &info)) {
            lpr_linux_stat_t *st = (lpr_linux_stat_t *)(uintptr_t)statbuf;
            lpr_memset(st, 0, sizeof(*st));
            st->st_ino = fd + 1u;
            st->st_nlink = 1;
            st->st_mode = LPR_LINUX_S_IFIFO | 0600u;
            st->st_blksize = 4096;
            return 0;
        }
    }
    if (!lpr_fd_is_filed(fd)) {
        return lpr_pacha_status_to_errno(lpr_pacha_syscall2(PACHAOS_SYSCALL_FD_STAT, fd, statbuf));
    }
    void *page = 0;
    const int page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    lpr_trace_process_event("fstat_filed_begin", fd, (uint64_t)(uint32_t)page_fd, 0);
    filed_wire_statx_t *stat = (filed_wire_statx_t *)page;
    lpr_memset(stat, 0, sizeof(*stat));
    stat->handle = lpr_fds[fd].handle;
    uint64_t ignored = 0;
    const int64_t status = lpr_filed_call(FILED_WIRE_OP_STAT, page_fd, 0, &ignored);
    lpr_trace_process_event("fstat_filed_end", fd, (uint64_t)(uint32_t)page_fd, status);
    if (status == 0) {
        lpr_write_linux_stat((void *)(uintptr_t)statbuf, stat);
    }
    lpr_destroy_standalone_wire_page(page_fd, page);
    return status;
}

int64_t lpr_linux_newfstatat(uint64_t dirfd, uint64_t path_raw, uint64_t statbuf, uint64_t flags)
{
    if (statbuf == 0) {
        return -LPR_LINUX_EFAULT;
    }
    lpr_trace_process_event("newfstatat_begin", dirfd, flags, 0);
    const char *path = (const char *)(uintptr_t)path_raw;
    if ((flags & LPR_LINUX_AT_EMPTY_PATH) != 0 && path != 0 && path[0] == 0) {
        const uint64_t empty_known_flags = LPR_LINUX_AT_EMPTY_PATH | LPR_LINUX_AT_SYMLINK_NOFOLLOW;
        if ((flags & ~empty_known_flags) != 0) {
            return -LPR_LINUX_EINVAL;
        }
        return lpr_linux_fstat(dirfd, statbuf);
    }
    const int64_t fd = lpr_linux_openat(dirfd, path_raw, LPR_LINUX_O_RDONLY, 0);
    lpr_trace_process_event("newfstatat_open", dirfd, flags, fd);
    if (fd < 0) {
        return fd;
    }
    const int64_t status = lpr_linux_fstat((uint64_t)fd, statbuf);
    lpr_trace_process_event("newfstatat_fstat", (uint64_t)fd, flags, status);
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
    const int page_fd = lpr_create_standalone_wire_page(&page);
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
        lpr_destroy_standalone_wire_page(page_fd, page);
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
        uint64_t next_offset = gd->offset + i + 1u;
        if (next_offset <= gd->offset) {
            next_offset = 0x7fffffffffffffffull;
        }
        *(uint64_t *)(void *)(out + written + 0u) = entry->handle != 0 ? entry->handle : next_offset;
        *(int64_t *)(void *)(out + written + 8u) =
            next_offset > 0x7fffffffffffffffull ? (int64_t)0x7fffffffffffffffull : (int64_t)next_offset;
        *(uint16_t *)(void *)(out + written + 16u) = reclen;
        *(uint8_t *)(void *)(out + written + 18u) = lpr_dtype_from_mode(entry->kind);
        lpr_memcpy(out + written + 19u, entry->name, (size_t)name_len);
        out[written + 19u + name_len] = 0;
        written += reclen;
    }
    lpr_destroy_standalone_wire_page(page_fd, page);
    return (int64_t)written;
}

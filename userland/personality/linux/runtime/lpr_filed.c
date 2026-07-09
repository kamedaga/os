#include "lpr_filed.h"
#include "lpr_error.h"
#include "lpr_fd/table.h"
#include "lpr_linux_syscall.h"
#include "lpr_process/client.h"
#include "lpr_socket.h"
#include "lpr_process/supervisor_fd_snapshot.h"
#include "support/string.h"
#include "support/syscall.h"
#include <pacha/error_conveyor.h>
#include <pacha/ipc.h>
#include <pacha/service_abi.h>
#include <pacha/trace.h>
#include <pachaos/abi.h>
#include <personality/lpr_client_abi.h>
#include <personality/linux_lpr.h>
#include <stddef.h>
#include <stdint.h>

__attribute__((visibility("hidden")))
void *memset(void *dst, int c, size_t n)
{
    return lpr_memset(dst, c, n);
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
    filed_v2_exec_lpr_fd_table_t *table;
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
    char path[FILED_V2_PATH_BYTES];
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
static lpr_fd_table_slot_t lpr_control_slots_initial[LPR_FD_TABLE_INITIAL_SIZE];
static lpr_fd_table_file_t lpr_control_files_initial[LPR_FD_TABLE_INITIAL_SIZE];
static lpr_filed_fd_t *lpr_fds;
static lpr_pipe_fd_t *lpr_pipe_fds;
static lpr_event_fd_t *lpr_event_fds;
static lpr_tty_fd_t *lpr_tty_fds;
static lpr_fd_table_slot_t *lpr_control_slots;
static lpr_fd_table_file_t *lpr_control_files;
static lpr_fd_table_t lpr_control_fd_table;
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
static char lpr_cwd_path[FILED_V2_PATH_BYTES];

#include "lpr_common/runtime_support.inc"

#include "lpr_tty/client.inc"

#include "lpr_process/bootstrap_state.inc"

#include "lpr_tty/runtime.inc"

#include "lpr_fd/dup_pipe.inc"

#include "lpr_vfs/cache.inc"

#include "lpr_vfs/path.inc"

#include "lpr_vfs/io.inc"

#include "lpr_process/exec.inc"

#include "lpr_fd/metadata.inc"

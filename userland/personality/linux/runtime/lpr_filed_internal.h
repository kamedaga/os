#ifndef LPR_FILED_INTERNAL_H
#define LPR_FILED_INTERNAL_H

#include "lpr_error.h"
#include "lpr_fd/table.h"
#include "lpr_filed.h"
#include "lpr_linux_syscall.h"
#include "lpr_process/client.h"
#include "lpr_socket.h"
#include "support/string.h"
#include "support/syscall.h"
#include <pacha/ipc.h>
#include <pacha/service_abi.h>
#include <pacha/status.h>
#include <pacha/trace.h>
#include <pachaos/abi.h>
#include <personality/lpr_client_abi.h>
#include <personality/linux_lpr.h>
#include <stddef.h>
#include <stdint.h>

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
#define LPR_LINUX_SIGPIPE 13u
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

typedef struct lpr_pacha_process_status {
    uint64_t state;
    uint64_t exit_code;
    uint64_t id;
    uint64_t generation;
} lpr_pacha_process_status_t;

enum {
    LPR_READLINK_CACHE_ENTRIES = 8,
};

typedef struct lpr_state {
    lpr_fd_table_t fd_table;
    uint64_t generation;
} lpr_state_t;

extern lpr_fd_table_slot_t lpr_control_slots_initial[LPR_FD_TABLE_INITIAL_SIZE];
extern lpr_fd_table_file_t lpr_control_files_initial[LPR_FD_TABLE_INITIAL_SIZE];
extern lpr_fd_table_slot_t *lpr_control_slots;
extern lpr_fd_table_file_t *lpr_control_files;
extern lpr_state_t lpr_state;
#define lpr_control_fd_table (lpr_state.fd_table)
extern uint64_t lpr_fd_table_capacity;
extern void *lpr_fd_table_dynamic_base;
extern uint64_t lpr_fd_table_dynamic_bytes;
extern lpr_readlink_cache_entry_t lpr_readlink_cache[LPR_READLINK_CACHE_ENTRIES];
extern lpr_filed_page_cache_entry_t lpr_page_cache[LPR_FILED_PAGE_CACHE_ENTRIES];
extern uint64_t lpr_readlink_cache_clock;
extern uint64_t lpr_page_cache_clock;
extern uint64_t lpr_readv_cache_total;
extern uint64_t lpr_readv_cache_coalesced;
extern uint64_t lpr_readv_cache_hit;
extern uint64_t lpr_readv_cache_fill;
extern uint64_t lpr_readv_cache_fallback;
extern uint64_t lpr_readv_cache_cross_page;
extern uint64_t lpr_readv_cache_to_vmo;
extern uint64_t lpr_readv_cache_bytes;
extern uint64_t lpr_request_id;
extern int lpr_filed_endpoint_checked;
extern int lpr_wire_page_fd;
extern void *lpr_wire_page;
extern int lpr_wire_page_busy;
extern int lpr_tty_wire_page_fd;
extern void *lpr_tty_wire_page;
extern int lpr_tty_wire_page_busy;
extern int lpr_session_fd;
extern int lpr_session_page_fd;
extern void *lpr_session_page;
extern int lpr_session_checked;
extern int lpr_session_payload_busy;
extern uint64_t lpr_termd_request_id;
extern int lpr_readv_vmo_fd;
extern void *lpr_readv_vmo_map;
extern uint64_t lpr_readv_vmo_len;
extern int lpr_pread_vmo_page_fd;
extern void *lpr_pread_vmo_page;
extern int lpr_pread_vmo_page_busy;
extern int lpr_default_stdio_checked;
extern int lpr_bootstrap_checked;
extern int lpr_bootstrap_valid;
extern int lpr_bootstrap_local_fds_installed;
extern struct lpr_bootstrap lpr_bootstrap;
extern int lpr_linux_process_state_checked;
extern int32_t lpr_linux_current_pid;
extern int32_t lpr_linux_current_ppid;
extern int32_t lpr_linux_current_sid;
extern int32_t lpr_linux_current_pgrp;
extern int32_t lpr_linux_next_pid;
extern int32_t lpr_linux_pending_child_pid;
extern int32_t lpr_linux_pending_child_ppid;
extern int32_t lpr_linux_pending_child_sid;
extern int32_t lpr_linux_pending_child_pgrp;
extern uint64_t lpr_supervisor_token;
extern uint64_t lpr_supervisor_pending_child_token;
extern int lpr_supervisor_enabled;
extern lpr_linux_process_entry_t lpr_linux_processes[LPR_LINUX_PROCESS_TABLE_SIZE];
extern lpr_linux_sigaction_record_t lpr_linux_sigactions[LPR_LINUX_SIGNAL_MAX + 1u];
extern uint64_t lpr_linux_signal_mask;
extern uint64_t lpr_linux_pending_signal_mask;
extern int lpr_linux_signal_dispatching;
extern int lpr_cwd_checked;
extern uint64_t lpr_cwd_handle;
extern char lpr_cwd_path[FILED_V2_PATH_BYTES];

void lpr_filed_session_drop(void);
void *lpr_session_payload_slot(uint64_t slot);
void lpr_zero_bytes(void *ptr, uint64_t len);
void lpr_fd_arrays_init(void);
int lpr_fd_table_ensure_capacity(uint64_t required_capacity);
int lpr_fd_table_ensure_fd(uint64_t fd);
int64_t lpr_pacha_status_to_errno(int64_t status);
void lpr_trace_clone_args(uint64_t flags, uint64_t child_stack, uint64_t parent_tid, uint64_t child_tid);
void lpr_trace_clone_frame(const char *event, const struct lpr_linux_user_frame *frame, int64_t status);
void lpr_trace_process_event(const char *event, uint64_t a, uint64_t b, int64_t status);
void lpr_trace_readv_size(uint64_t fd, uint64_t iov_count, uint64_t requested, uint64_t coalesced, uint64_t offset);
void lpr_trace_readv_to_vmo_status(uint64_t fd, uint64_t requested, int64_t status);
void lpr_state_dump(const char *reason);

int lpr_pipe_fd_is_active(uint64_t fd);
int lpr_native_pipe_fd_info(uint64_t fd, struct pacha_fd_info *out);
int lpr_native_pipe_slot_claimable(uint64_t fd, struct pacha_fd_info *out);
int lpr_fd_slot_available(uint64_t fd);
int lpr_fd_slot_alloc_from(uint64_t min_fd);
int lpr_create_wire_page(void **out_page);
void lpr_destroy_wire_page(int page_fd, void *page);
int lpr_create_tty_wire_page(void **out_page);
void lpr_destroy_tty_wire_page(int page_fd, void *page);
void lpr_reset_fork_child_rpc_state(void);
void lpr_linux_process_state_init(void);
void lpr_linux_pump_tty_signals(void);
void lpr_linux_raise_sigpipe(void);
uint64_t lpr_linux_unblockable_signal_mask(void);
void lpr_fill_termd_caller(uint64_t *session_id, uint64_t *process_id, uint64_t *pgrp_id);
void lpr_fill_termd_signal_state(uint64_t *signal_mask, uint64_t *signal_ignored);
void *lpr_termd_payload(void *page);
void *lpr_supervisor_payload(void *page);
lpr_linux_process_entry_t *lpr_linux_process_find(int32_t linux_pid);
int64_t lpr_supervisor_call(
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    int transfer_fd,
    uint64_t *out_result);
int64_t lpr_supervisor_call_token(
    uint32_t op,
    uint64_t token,
    int transfer_fd,
    uint64_t *out_result);
int64_t lpr_supervisor_kill_pid(int32_t pid, uint32_t sig, uint64_t *out_delivered);
int lpr_supervisor_get_state(lprs_v2_process_state_t *out_state);
int64_t lpr_tty_wait(uint64_t fd, uint32_t events);
void lpr_pipe_after_fork_child(void);
void lpr_cwd_init(void);
int64_t lpr_filed_call(uint32_t op, int page_fd, uint64_t word2, uint64_t *out_result);
int64_t lpr_filed_close_handle(uint64_t handle);
int64_t lpr_filed_dup_handle(uint64_t handle, uint64_t fd_flags, uint64_t *out_handle);
int lpr_create_standalone_wire_page(void **out_page);
void lpr_destroy_standalone_wire_page(int fd, void *page);
int lpr_exec_local_fd_preserve(uint64_t fd, int *out_preserve);
int lpr_count_exec_local_fds(uint64_t *out_count);
void lpr_write_exec_local_fd_desc(filed_v2_exec_lpr_fd_t *desc, uint64_t fd);
int lpr_prepare_exec_local_fds(filed_v2_exec_path_t *exec, lpr_exec_local_fd_table_t *local_table);
void lpr_destroy_exec_local_fd_table(lpr_exec_local_fd_table_t *local_table);
int lpr_install_bootstrap_local_fds(const lpr_bootstrap_fd_t *descs, uint64_t count);
void lpr_readlink_cache_clear(void);
int lpr_readlink_cache_lookup(const char *path, uint64_t length, int64_t *out_status);
void lpr_readlink_cache_store(const char *path, uint64_t length, int64_t status);
void lpr_page_cache_clear(void);
void lpr_page_cache_invalidate_handle(uint64_t handle);
lpr_filed_page_cache_entry_t *lpr_page_cache_lookup(uint64_t handle, uint64_t offset, uint64_t requested);
lpr_filed_page_cache_entry_t *lpr_page_cache_find_marker(uint64_t handle, uint64_t page_start);
lpr_filed_page_cache_entry_t *lpr_page_cache_slot(void);
uint64_t lpr_page_align_up(uint64_t value);
uint64_t lpr_scatter_iov(const lpr_linux_iovec_t *iov, uint64_t iov_count, const unsigned char *src, uint64_t src_len);
int64_t lpr_linux_readlinkat_to_buffer(uint64_t dirfd, uint64_t path_raw, char *target, uint64_t capacity);
int lpr_resolve_final_symlink_path(const char *path, const char *target, char *out, uint64_t capacity);

/* Prototypes for split runtime translation units. */
int lpr_bootstrap_fd_desc_valid_common(const lpr_bootstrap_fd_t *desc, uint64_t *out_fd);
int lpr_control_dup_fd(uint64_t old_fd, uint64_t new_fd, uint64_t cloexec);
int lpr_control_ensure_from_legacy(uint64_t fd);
int lpr_control_fd_active(uint64_t fd);
int lpr_control_fd_cloexec(uint64_t fd, int *out_cloexec);
int lpr_control_install_fd( uint64_t fd, uint8_t kind, uint64_t linux_flags, uint64_t backend_id, uint64_t offset);
int lpr_control_set_fd_flags(uint64_t fd, uint64_t flags);
int lpr_control_set_status_flags(uint64_t fd, uint64_t flags);
int lpr_count_exec_local_fds(uint64_t *out_count);
int lpr_create_pread_vmo_wire_page(void **out_page);
int lpr_create_standalone_wire_page(void **out_page);
int lpr_create_tty_wire_page(void **out_page);
int lpr_create_wire_page(void **out_page);
int lpr_cwd_append_component(char *out, uint64_t capacity, uint64_t *len, const char *component, uint64_t component_len);
int lpr_exec_add_string(filed_v2_exec_path_t *exec, filed_v2_exec_string_ref_t *ref, const char *value);
int lpr_exec_copy_string_vector( filed_v2_exec_path_t *exec, filed_v2_exec_string_ref_t *refs, uint64_t max_refs, uint64_t vector_raw, uint64_t *out_count);
int lpr_exec_local_fd_active(uint64_t fd);
int lpr_exec_local_fd_preserve(uint64_t fd, int *out_preserve);
int lpr_fd_alloc(uint64_t handle, uint64_t flags);
int lpr_fd_is_filed(uint64_t fd);
int lpr_fd_linux_visible_active(uint64_t fd);
int lpr_fd_local_active(uint64_t fd);
int lpr_fd_shadow_offset_eligible(uint64_t fd);
int lpr_fd_slot_alloc(void);
int lpr_fd_slot_alloc_from(uint64_t min_fd);
int lpr_fd_slot_available(uint64_t fd);
int lpr_fd_table_alloc( lpr_fd_table_t *table, uint32_t min_fd, const lpr_fd_table_install_t *install, uint32_t *out_fd);
int lpr_fd_table_close(lpr_fd_table_t *table, uint32_t fd);
int lpr_fd_table_close_range( lpr_fd_table_t *table, uint32_t first, uint32_t last, uint32_t cloexec_only);
int lpr_fd_table_dup( lpr_fd_table_t *table, uint32_t old_fd, uint32_t min_fd, uint16_t new_fd_flags, uint32_t *out_fd);
int lpr_fd_table_dup2( lpr_fd_table_t *table, uint32_t old_fd, uint32_t new_fd, uint16_t new_fd_flags);
int lpr_fd_table_ensure_capacity(uint64_t required_capacity);
int lpr_fd_table_ensure_fd(uint64_t fd);
int lpr_fd_table_get_fd_flags(const lpr_fd_table_t *table, uint32_t fd, uint16_t *out_flags);
int lpr_fd_table_get_refcount(const lpr_fd_table_t *table, uint32_t fd, uint32_t *out_refcount);
int lpr_fd_table_get_offset(const lpr_fd_table_t *table, uint32_t fd, uint64_t *out_offset);
int lpr_fd_table_get_status_flags(const lpr_fd_table_t *table, uint32_t fd, uint32_t *out_flags);
int lpr_fd_table_install_at( lpr_fd_table_t *table, uint32_t fd, const lpr_fd_table_install_t *install);
int lpr_fd_table_layout( uint64_t capacity, uint64_t *control_slot_offset, uint64_t *control_file_offset, uint64_t *total_bytes);
int lpr_fd_table_segment_bytes(uint64_t capacity, uint64_t element_size, uint64_t *out);
int lpr_fd_table_set_fd_flags(lpr_fd_table_t *table, uint32_t fd, uint16_t flags);
int lpr_fd_table_set_offset(lpr_fd_table_t *table, uint32_t fd, uint64_t offset);
int lpr_fd_table_set_status_flags(lpr_fd_table_t *table, uint32_t fd, uint32_t flags);
const lpr_fd_object_t *lpr_fd_object_for_fd_const(uint64_t fd);
lpr_event_fd_t *lpr_fd_event_payload(uint64_t fd);
lpr_fd_object_t *lpr_fd_object_for_fd(uint64_t fd);
lpr_filed_fd_t *lpr_fd_filed_payload(uint64_t fd);
lpr_pipe_fd_t *lpr_fd_pipe_payload(uint64_t fd);
lpr_socket_fd_t *lpr_fd_socket_payload(uint64_t fd);
lpr_tty_fd_t *lpr_fd_tty_payload(uint64_t fd);
int lpr_install_bootstrap_local_fds(const lpr_bootstrap_fd_t *descs, uint64_t count);
int lpr_install_exec_bootstrap_fd(int bootstrap_fd);
int lpr_install_local_fd_descs(const lpr_bootstrap_fd_t *descs, uint64_t count);
int lpr_linux_default_signal_ignored(uint32_t sig);
int lpr_linux_default_signal_stops(uint32_t sig);
int lpr_linux_eventfd_active(uint64_t fd);
int lpr_linux_filed_fd_active(uint64_t fd);
int lpr_linux_pipe_fd_active(uint64_t fd);
int lpr_linux_process_register( int32_t linux_pid, int32_t linux_ppid, int32_t linux_sid, int32_t linux_pgrp, int process_fd);
int lpr_linux_signal_pgrp(int32_t pgrp, uint32_t signo);
int lpr_linux_signal_process_fd(int process_fd, uint32_t signo);
int lpr_linux_tty_fd_active(uint64_t fd);
int lpr_load_bootstrap(void);
int lpr_native_fd_info(uint64_t fd, struct pacha_fd_info *out);
int lpr_native_pipe_fd_info(uint64_t fd, struct pacha_fd_info *out);
int lpr_native_pipe_slot_claimable(uint64_t fd, struct pacha_fd_info *out);
int lpr_path_is_terminated(const char *path, uint64_t capacity);
int lpr_pipe_fd_is_active(uint64_t fd);
int lpr_pipe_track_native_fd(uint64_t fd, const struct pacha_fd_info *info);
int lpr_prepare_exec_local_fds( filed_v2_exec_path_t *exec, lpr_exec_local_fd_table_t *local_table);
int lpr_readlink_cache_lookup(const char *path, uint64_t length, int64_t *out_status);
int lpr_resolve_final_symlink_path(const char *path, const char *target, char *out, uint64_t capacity);
int lpr_restore_bootstrap_event_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd);
int lpr_restore_bootstrap_fd_desc(const lpr_bootstrap_fd_t *desc);
int lpr_restore_bootstrap_filed_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd);
int lpr_restore_bootstrap_pipe_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd);
int lpr_restore_bootstrap_socket_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd);
int lpr_restore_bootstrap_tty_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd);
int lpr_runtime_reserved_fd(uint64_t fd);
int lpr_supervisor_get_state(lprs_v2_process_state_t *out_state);
int lpr_timespec_less_equal( const struct pachaos_timespec *lhs, const struct pachaos_timespec *rhs);
int lpr_tty_fd_alloc(uint64_t handle, uint64_t flags);
int32_t lpr_linux_alloc_child_pid(void);
int64_t lpr_close_native_fd_if_open(uint64_t fd);
int64_t lpr_control_get_fd_flags(uint64_t fd);
int64_t lpr_control_get_status_flags(uint64_t fd, uint32_t access_mode);
int64_t lpr_copy_path(char *out, uint64_t capacity, const char *path);
int64_t lpr_cwd_install(uint64_t handle, const char *path);
int64_t lpr_cwd_normalize(const char *path, char *out, uint64_t capacity);
int64_t lpr_dir_handle_for(uint64_t dirfd, const char *path, uint64_t *out);
int64_t lpr_filed_call(uint32_t op, int page_fd, uint64_t word2, uint64_t *out_result);
int64_t lpr_filed_close_handle(uint64_t handle);
int64_t lpr_filed_dup_handle(uint64_t handle, uint64_t fd_flags, uint64_t *out_handle);
int64_t lpr_filed_endpoint_ready(void);
int64_t lpr_filed_exec_self( filed_v2_exec_path_t *exec, const lpr_exec_local_fd_table_t *local_table, int *out_process_fd, int *out_thread_fd, int *out_bootstrap_fd);
int64_t lpr_filed_fast_call(uint32_t op, uint64_t word2, uint64_t *out_result);
int64_t lpr_filed_io(uint32_t op, uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset);
int64_t lpr_filed_open_handle_at( uint64_t dirfd, const char *path, uint64_t flags, uint64_t mode, uint64_t *out_handle);
int64_t lpr_filed_session_connect(void);
int64_t lpr_filed_utimens_handle(uint64_t handle, uint64_t times_raw);
int64_t lpr_filed_v2_payload_size(uint32_t op, uint32_t *out_payload_size);
int64_t lpr_install_stdio_fd_from_tty(uint64_t tty_fd, uint64_t target_fd);
int64_t lpr_iov_scalar_io(uint64_t fd, uint64_t iov_raw, uint64_t iov_count, int write);
int64_t lpr_linux_access(uint64_t path, uint64_t mode);
int64_t lpr_linux_chdir(uint64_t path_raw);
int64_t lpr_linux_clock_nanosleep(uint64_t clock_id, uint64_t flags, uint64_t req_raw, uint64_t rem_raw);
int64_t lpr_linux_clone(uint64_t flags, uint64_t child_stack, uint64_t parent_tid, uint64_t child_tid, uint64_t tls);
int64_t lpr_linux_close(uint64_t fd);
int64_t lpr_linux_close_range(uint64_t first, uint64_t last, uint64_t flags);
int64_t lpr_linux_dispatch_pending_signals(void);
int64_t lpr_linux_dup(uint64_t fd, uint64_t min_fd, uint64_t cloexec);
int64_t lpr_linux_dup2(uint64_t old_fd, uint64_t new_fd, uint64_t flags);
int64_t lpr_linux_dup_into(uint64_t fd, int target_fd, uint64_t min_fd, uint64_t cloexec);
int64_t lpr_linux_eventfd2(uint64_t initval, uint64_t flags);
int64_t lpr_linux_execve(uint64_t path_raw, uint64_t argv_raw, uint64_t envp_raw);
int64_t lpr_linux_faccessat(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t flags);
int64_t lpr_linux_fchdir(uint64_t fd);
int64_t lpr_linux_fchmod(uint64_t fd, uint64_t mode);
int64_t lpr_linux_fchmodat(uint64_t dirfd, uint64_t path_raw, uint64_t mode, uint64_t flags);
int64_t lpr_linux_fchownat(uint64_t dirfd, uint64_t path, uint64_t owner, uint64_t group, uint64_t flags);
int64_t lpr_linux_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg);
int64_t lpr_linux_file_vmo(uint64_t fd, uint64_t file_offset, uint64_t length, uint64_t *out_loaded);
int64_t lpr_linux_flock(uint64_t fd, uint64_t operation);
int64_t lpr_linux_fork(void);
int64_t lpr_linux_fstat(uint64_t fd, uint64_t statbuf);
int64_t lpr_linux_fsync(uint64_t fd);
int64_t lpr_linux_getcwd(uint64_t buf, uint64_t size);
int64_t lpr_linux_getdents64(uint64_t fd, uint64_t buf, uint64_t count);
int64_t lpr_linux_getpgid(uint64_t pid_raw);
int64_t lpr_linux_getpgrp(void);
int64_t lpr_linux_getpid(void);
int64_t lpr_linux_getppid(void);
int64_t lpr_linux_getsid(uint64_t pid_raw);
int64_t lpr_linux_ioctl(uint64_t fd, uint64_t request, uint64_t arg);
int64_t lpr_linux_kill(uint64_t pid_raw, uint64_t sig_raw);
int64_t lpr_linux_linkat(uint64_t old_dirfd, uint64_t old_path_raw, uint64_t new_dirfd, uint64_t new_path_raw, uint64_t flags);
int64_t lpr_linux_lseek(uint64_t fd, uint64_t offset, uint64_t whence);
int64_t lpr_linux_mkdirat(uint64_t dirfd, uint64_t path_raw, uint64_t mode);
int64_t lpr_linux_mknodat(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t dev);
int64_t lpr_linux_nanosleep(uint64_t req_raw, uint64_t rem_raw);
int64_t lpr_linux_newfstatat(uint64_t dirfd, uint64_t path_raw, uint64_t statbuf, uint64_t flags);
int64_t lpr_linux_now(lpr_linux_timespec_t *out);
int64_t lpr_linux_open_metadata(uint64_t dirfd, uint64_t path_raw);
int64_t lpr_linux_openat(uint64_t dirfd, uint64_t path_raw, uint64_t flags, uint64_t mode);
int64_t lpr_linux_openat_once(uint64_t dirfd, uint64_t path_raw, uint64_t flags, uint64_t mode);
int64_t lpr_linux_pipe2(uint64_t fds_raw, uint64_t flags);
int64_t lpr_linux_pread64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset);
int64_t lpr_linux_pread_to_vmo( uint64_t fd, uint64_t vmo_fd, uint64_t vmo_offset, uint64_t count, uint64_t file_offset);
int64_t lpr_linux_read(uint64_t fd, uint64_t buf, uint64_t count);
int64_t lpr_linux_readlink(uint64_t path, uint64_t buf, uint64_t bufsiz);
int64_t lpr_linux_readlinkat_to_buffer(uint64_t dirfd, uint64_t path_raw, char *target, uint64_t capacity);
int64_t lpr_linux_readv(uint64_t fd, uint64_t iov_raw, uint64_t iov_count);
int64_t lpr_linux_renameat(uint64_t old_dirfd, uint64_t old_path_raw, uint64_t new_dirfd, uint64_t new_path_raw);
int64_t lpr_linux_resolve_utime( const lpr_linux_timespec_t *input, const lpr_linux_timespec_t *now, uint64_t wire_bit, uint64_t *mask, int64_t *out_sec, int64_t *out_nsec);
int64_t lpr_linux_rt_sigaction(uint64_t sig_raw, uint64_t act_raw, uint64_t oldact_raw, uint64_t sigsetsize);
int64_t lpr_linux_rt_sigprocmask(uint64_t how, uint64_t set_raw, uint64_t oldset_raw, uint64_t sigsetsize);
int64_t lpr_linux_setpgid(uint64_t pid_raw, uint64_t pgid_raw);
int64_t lpr_linux_setsid(void);
int64_t lpr_linux_sleep_result(int64_t status);
int64_t lpr_linux_symlinkat(uint64_t target_raw, uint64_t new_dirfd, uint64_t linkpath_raw);
int64_t lpr_linux_sync(void);
int64_t lpr_linux_try_wait_process_fd(uint64_t process_fd, uint64_t *out_exit_code);
int64_t lpr_linux_unlinkat(uint64_t dirfd, uint64_t path_raw, uint64_t flags);
int64_t lpr_linux_utimensat(uint64_t dirfd, uint64_t path_raw, uint64_t times, uint64_t flags);
int64_t lpr_linux_validate_timespec(const struct pachaos_timespec *ts);
int64_t lpr_linux_vfork(void);
int64_t lpr_linux_wait4(uint64_t pid, uint64_t status_raw, uint64_t options, uint64_t rusage);
int64_t lpr_linux_wait_process_fd(uint64_t process_fd, uint64_t *out_exit_code);
int64_t lpr_linux_write(uint64_t fd, uint64_t buf, uint64_t count);
int64_t lpr_linux_writev(uint64_t fd, uint64_t iov_raw, uint64_t iov_count);
int64_t lpr_native_pipe_read(uint64_t fd, uint64_t buf, uint64_t count);
int64_t lpr_native_pipe_readv(uint64_t fd, uint64_t iov_raw, uint64_t iov_count);
int64_t lpr_native_pipe_write(uint64_t fd, uint64_t buf, uint64_t count);
int64_t lpr_native_pipe_writev(uint64_t fd, uint64_t iov_raw, uint64_t iov_count);
int64_t lpr_pacha_clock_gettime(uint64_t clock_id, struct pachaos_timespec *out);
int64_t lpr_pacha_nanosleep(const struct pachaos_timespec *req);
int64_t lpr_pacha_status_to_errno(int64_t status);
int64_t lpr_pipe_wait(uint64_t fd, uint32_t events, uint64_t min_write_bytes);
int64_t lpr_prepare_exec_cwd(filed_v2_exec_path_t *exec);
int64_t lpr_process_client_call( uint64_t *request_counter, int64_t (*status_to_errno)(int64_t status), uint32_t op, int page_fd, void *page, uint32_t payload_size, int transfer_fd, uint64_t *out_result);
int64_t lpr_process_client_call_token( uint64_t *request_counter, int64_t (*status_to_errno)(int64_t status), int (*create_page)(void **out_page), void (*destroy_page)(int fd, void *page), uint32_t op, uint64_t token, int transfer_fd, uint64_t *out_result);
int64_t lpr_read_from_page_cache(uint64_t fd, uint64_t buf, uint64_t requested, uint64_t offset);
int64_t lpr_readv_scratch_vmo(uint64_t requested, int *out_fd, unsigned char **out_map, uint64_t *out_len);
int64_t lpr_supervisor_call( uint32_t op, int page_fd, void *page, uint32_t payload_size, int transfer_fd, uint64_t *out_result);
int64_t lpr_supervisor_call_token( uint32_t op, uint64_t token, int transfer_fd, uint64_t *out_result);
int64_t lpr_supervisor_cwd_set(uint64_t handle, const char *path);
int64_t lpr_supervisor_kill_pid(int32_t pid, uint32_t sig, uint64_t *out_delivered);
int64_t lpr_termd_call( uint32_t op, int page_fd, void *page, uint32_t payload_size, uint64_t *out_result);
int64_t lpr_termd_call_handle(uint32_t op, uint64_t handle, uint64_t *out_result);
int64_t lpr_tty_io(uint64_t op, uint64_t fd, uint64_t buf, uint64_t count);
int64_t lpr_tty_ioctl(uint64_t fd, uint64_t request, uint64_t arg);
int64_t lpr_tty_open_path(const char *path, uint64_t flags);
int64_t lpr_tty_sleep_ms(uint64_t ms);
int64_t lpr_tty_wait(uint64_t fd, uint32_t events);
uint16_t lpr_control_fd_flags_from_fcntl(uint64_t flags);
uint16_t lpr_control_fd_flags_from_linux(uint64_t flags);
uint16_t lpr_dirent_reclen(uint64_t name_len);
uint32_t lpr_control_merge_legacy_flags( uint32_t old_flags, uint16_t fd_flags, uint32_t status_flags);
uint32_t lpr_control_status_flags_from_linux(uint64_t flags);
uint32_t lpr_control_status_flags_to_linux(uint32_t status);
uint32_t lpr_fd_table_live_file_count(const lpr_fd_table_t *table);
uint32_t lpr_fd_table_open_count(const lpr_fd_table_t *table);
uint32_t lpr_linux_eventfd_poll_events(uint64_t fd, uint32_t events);
uint32_t lpr_linux_first_pending_signal(uint64_t mask);
uint32_t lpr_linux_native_fd_poll_events(uint64_t fd, uint32_t events);
uint32_t lpr_linux_pipe_poll_events(uint64_t fd, uint32_t events);
uint32_t lpr_linux_tty_poll_events(uint64_t fd, uint32_t events);
uint32_t lpr_pipe_flags_from_info(const struct pacha_fd_info *info);
uint32_t lpr_pipe_poll_events_from_pacha(uint64_t events);
uint64_t lpr_align_up_4096(uint64_t value);
uint64_t lpr_align_up_pow2(uint64_t value, uint64_t align);
uint64_t lpr_exec_fd_table_bytes_for_capacity(uint64_t capacity);
uint64_t lpr_exec_fd_table_capacity_for_count(uint64_t count);
uint64_t lpr_fd_table_next_capacity(uint64_t required_capacity);
uint64_t lpr_filed_control_offset(uint64_t fd);
uint64_t lpr_linux_filed_fd_handle(uint64_t fd);
uint64_t lpr_linux_ignored_signal_mask(void);
uint64_t lpr_linux_signal_bit(uint32_t sig);
uint64_t lpr_linux_unblockable_signal_mask(void);
uint64_t lpr_open_flags(uint64_t flags);
uint64_t lpr_open_rights(uint64_t flags);
uint64_t lpr_page_align_up(uint64_t value);
uint64_t lpr_parse_hvc_index(const char *path);
uint64_t lpr_parse_pts_index(const char *path);
uint64_t lpr_pipe_flags_to_pacha(uint64_t flags);
uint64_t lpr_pipe_poll_events_to_pacha(uint32_t events);
uint64_t lpr_pipe_rights(int readable);
uint64_t lpr_pipe_writev_wait_min(uint64_t iov_raw, uint64_t iov_count);
uint64_t lpr_scatter_iov( const lpr_linux_iovec_t *iov, uint64_t iov_count, const unsigned char *src, uint64_t length);
uint8_t lpr_dtype_from_mode(uint64_t mode);
void lpr_close_local_state_before_self_exec(void);
void lpr_close_non_linux_native_fd(uint64_t fd);
void lpr_control_close_fd(uint64_t fd);
void lpr_control_sync_legacy_flags(uint64_t fd);
void lpr_cwd_init(void);
void lpr_cwd_pop_component(char *path, uint64_t *len);
void lpr_cwd_set_root(void);
void lpr_destroy_exec_local_fd_table(lpr_exec_local_fd_table_t *local_table);
void lpr_destroy_pread_vmo_wire_page(int fd, void *page);
void lpr_destroy_standalone_wire_page(int fd, void *page);
void lpr_destroy_tty_wire_page(int fd, void *page);
void lpr_destroy_wire_page(int fd, void *page);
void lpr_discard_exec_cwd(filed_v2_exec_path_t *exec);
void lpr_fd_arrays_init(void);
void lpr_fd_table_init( lpr_fd_table_t *table, lpr_fd_table_slot_t *slots, uint32_t slot_count, lpr_fd_table_file_t *files, uint32_t file_count);
void lpr_filed_control_advance_offset(uint64_t fd, uint64_t old_offset, uint64_t amount);
void lpr_filed_control_set_offset(uint64_t fd, uint64_t offset);
void lpr_filed_session_drop(void);
void lpr_fill_termd_caller(uint64_t *session_id, uint64_t *process_id, uint64_t *pgrp_id);
void lpr_fill_termd_signal_state(uint64_t *signal_mask, uint64_t *signal_ignored);
void lpr_linux_apply_pending_fork_child(void);
void lpr_linux_ensure_default_stdio(void);
void lpr_linux_exit_for_signal(uint32_t sig);
void lpr_linux_prepare_process_exit(uint64_t exit_code);
void lpr_linux_process_clear_children(void);
void lpr_linux_process_state_init(void);
void lpr_linux_pump_tty_signals(void);
void lpr_linux_queue_signal(uint32_t sig);
void lpr_linux_raise_sigpipe(void);
void lpr_linux_readv_cache_trace_dump(void);
void lpr_page_cache_clear(void);
void lpr_page_cache_invalidate_handle(uint64_t handle);
void lpr_pipe_after_fork_child(void);
void lpr_pipe_close_fd(uint64_t fd);
void lpr_readlink_cache_clear(void);
void lpr_readlink_cache_store(const char *path, uint64_t length, int64_t status);
void lpr_timespec_subtract( const struct pachaos_timespec *end, const struct pachaos_timespec *start, struct pachaos_timespec *out);
void lpr_trace_clone_args(uint64_t flags, uint64_t child_stack, uint64_t parent_tid, uint64_t child_tid);
void lpr_trace_clone_frame(const char *event, const struct lpr_linux_user_frame *frame, int64_t status);
void lpr_trace_process_event(const char *event, uint64_t a, uint64_t b, int64_t status);
void lpr_trace_readv_size(uint64_t fd, uint64_t iov_count, uint64_t requested, uint64_t coalesced, uint64_t offset);
void lpr_trace_readv_to_vmo_status(uint64_t fd, uint64_t requested, int64_t status);
void lpr_write_exec_local_fd_desc(filed_v2_exec_lpr_fd_t *desc, uint64_t fd);
void lpr_write_linux_stat(void *statbuf, const filed_v2_statx_t *wire);
void lpr_zero_bytes(void *ptr, uint64_t len);

#endif

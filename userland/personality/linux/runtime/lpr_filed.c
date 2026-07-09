#include "lpr_filed_internal.h"

__attribute__((visibility("hidden")))
void *memset(void *dst, int c, size_t n)
{
    return lpr_memset(dst, c, n);
}

lpr_fd_table_slot_t lpr_control_slots_initial[LPR_FD_TABLE_INITIAL_SIZE];
lpr_fd_table_file_t lpr_control_files_initial[LPR_FD_TABLE_INITIAL_SIZE];
lpr_fd_table_slot_t *lpr_control_slots;
lpr_fd_table_file_t *lpr_control_files;
lpr_state_t lpr_state;
uint64_t lpr_fd_table_capacity;
void *lpr_fd_table_dynamic_base;
uint64_t lpr_fd_table_dynamic_bytes;
lpr_readlink_cache_entry_t lpr_readlink_cache[LPR_READLINK_CACHE_ENTRIES];
lpr_filed_page_cache_entry_t lpr_page_cache[LPR_FILED_PAGE_CACHE_ENTRIES];
uint64_t lpr_readlink_cache_clock;
uint64_t lpr_page_cache_clock;
uint64_t lpr_request_id = 0x4c505246494c4501ull;
int lpr_filed_endpoint_checked;
int lpr_wire_page_fd = -1;
void *lpr_wire_page;
int lpr_wire_page_busy;
int lpr_tty_wire_page_fd = -1;
void *lpr_tty_wire_page;
int lpr_tty_wire_page_busy;
int lpr_session_fd = -1;
int lpr_session_page_fd = -1;
void *lpr_session_page;
int lpr_session_checked;
int lpr_session_payload_busy;
uint64_t lpr_termd_request_id = 0x4c50525445524d01ull;
int lpr_readv_vmo_fd = -1;
void *lpr_readv_vmo_map;
uint64_t lpr_readv_vmo_len;
int lpr_pread_vmo_page_fd = -1;
void *lpr_pread_vmo_page;
int lpr_pread_vmo_page_busy;
int lpr_default_stdio_checked;
int lpr_bootstrap_checked;
int lpr_bootstrap_valid;
int lpr_bootstrap_local_fds_installed;
struct lpr_bootstrap lpr_bootstrap;
int lpr_linux_process_state_checked;
int32_t lpr_linux_current_pid;
int32_t lpr_linux_current_ppid;
int32_t lpr_linux_current_sid;
int32_t lpr_linux_current_pgrp;
int32_t lpr_linux_next_pid;
int32_t lpr_linux_pending_child_pid;
int32_t lpr_linux_pending_child_ppid;
int32_t lpr_linux_pending_child_sid;
int32_t lpr_linux_pending_child_pgrp;
uint64_t lpr_supervisor_token;
uint64_t lpr_supervisor_pending_child_token;
int lpr_supervisor_enabled;
lpr_linux_process_entry_t lpr_linux_processes[LPR_LINUX_PROCESS_TABLE_SIZE];
lpr_linux_sigaction_record_t lpr_linux_sigactions[LPR_LINUX_SIGNAL_MAX + 1u];
uint64_t lpr_linux_signal_mask;
uint64_t lpr_linux_pending_signal_mask;
int lpr_linux_signal_dispatching;
int lpr_cwd_checked;
uint64_t lpr_cwd_handle;
char lpr_cwd_path[FILED_V2_PATH_BYTES];

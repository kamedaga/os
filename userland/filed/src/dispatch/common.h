#include "filed/dispatch.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "filed/fd_ipc.h"
#include "filed/exec.h"
#include "filed/exec_linux_lpr.h"
#include "filed/payload.h"
#include "filed/ipc_protocol.h"
#include "filed/cache.h"
#include "filed/tmpfs_backend.h"
#include "pacha/abi.h"
#include "pacha/ipc.h"
#include "pacha/status.h"
#include "pacha/syscall.h"
#include "pacha/trace.h"
#include "termd/ipc_protocol.h"
#include "../internal/dispatch_state.h"

#define filed_dispatch_metrics (runtime->dispatch_state->dispatch_metrics)
#define filed_fast_metrics (runtime->dispatch_state->fast_metrics)
#define filed_fast_op_metrics (runtime->dispatch_state->fast_op_metrics)
#define filed_cache (runtime->dispatch_state->cache)
#define filed_page_cache (runtime->dispatch_state->cache.page)
#define filed_dir_cache (runtime->dispatch_state->cache.dir)
#define filed_negative_lookup_cache (runtime->dispatch_state->cache.negative)
#define filed_file_vmo_cache (runtime->dispatch_state->cache.file_vmo)
#define filed_target_lookup_vfs_hits (runtime->dispatch_state->target_lookup_vfs_hits)
#define filed_target_lookup_backend_hits (runtime->dispatch_state->target_lookup_backend_hits)
#define filed_target_lookup_misses (runtime->dispatch_state->target_lookup_misses)
#define filed_file_vmo_cache_hits (runtime->dispatch_state->cache.file_vmo.hits)
#define filed_file_vmo_cache_misses (runtime->dispatch_state->cache.file_vmo.misses)
#define filed_file_vmo_cache_stores (runtime->dispatch_state->cache.file_vmo.stores)
#define filed_file_vmo_cache_evictions (runtime->dispatch_state->cache.file_vmo.evictions)

typedef struct filed_page_dispatch_result {
    int64_t status;
    uint64_t result;
    int process_fd;
    int thread_fd;
} filed_page_dispatch_result_t;

typedef struct filed_dispatch_saved_fd {
    int fd;
    uint64_t rights;
    uint64_t flags;
} filed_dispatch_saved_fd_t;

bool filed_backend_object_is_tmpfs(uint64_t backend_object);
int filed_backend_lookup(filed_runtime_t *runtime, uint64_t parent_object_id, const char *name, uint64_t *out_object_id);
int filed_backend_statx(filed_runtime_t *runtime, uint64_t object_id, storage_statx_reply_t *out_stat);
int filed_backend_pread(filed_runtime_t *runtime, uint64_t object_id, uint64_t offset, void *buffer, uint64_t length, uint64_t *out_bytes);
int filed_backend_pwrite(filed_runtime_t *runtime, uint64_t object_id, uint64_t offset, const void *buffer, uint64_t length, uint64_t *out_bytes);
int filed_backend_fsync(filed_runtime_t *runtime, uint64_t object_id);
int filed_backend_create(filed_runtime_t *runtime, uint64_t parent_object_id, const char *name, uint64_t mode, uint64_t *out_object_id);
int filed_backend_truncate(filed_runtime_t *runtime, uint64_t object_id, uint64_t size);
int filed_backend_utimens(filed_runtime_t *runtime, uint64_t object_id, uint32_t mask, int64_t atime_sec, int64_t atime_nsec, int64_t mtime_sec, int64_t mtime_nsec);
int filed_backend_chmod(filed_runtime_t *runtime, uint64_t object_id, uint64_t mode);
int filed_backend_unlink(filed_runtime_t *runtime, uint64_t parent_object_id, const char *name);
int filed_backend_mknod(filed_runtime_t *runtime, uint64_t parent_object_id, const char *name, uint64_t mode, uint64_t dev, uint64_t *out_object_id);
int filed_backend_link(filed_runtime_t *runtime, uint64_t old_object_id, uint64_t new_parent_object_id, const char *new_name, uint64_t *out_object_id);
int filed_backend_mkdir(filed_runtime_t *runtime, uint64_t parent_object_id, const char *name, uint64_t mode, uint64_t *out_object_id);
int filed_backend_symlink(filed_runtime_t *runtime, uint64_t parent_object_id, const char *name, const char *target, uint64_t target_length, uint64_t *out_object_id);
int filed_backend_readlink(filed_runtime_t *runtime, uint64_t object_id, char *out_target, uint64_t target_capacity, uint64_t *out_length);
int filed_backend_rmdir(filed_runtime_t *runtime, uint64_t parent_object_id, const char *name);
int filed_backend_rename(filed_runtime_t *runtime, uint64_t old_parent_object_id, const char *old_name, uint64_t new_parent_object_id, const char *new_name, uint64_t *out_object_id);
int filed_backend_release_object(filed_runtime_t *runtime, uint64_t object_id);
int filed_backend_getdents(filed_runtime_t *runtime, uint64_t dir_object_id, uint64_t offset, storage_getdents_request_t *out_entries);
bool filed_root_getdents_splices_tmpfs(filed_runtime_t *runtime, uint64_t dir_object_id);
uint64_t filed_root_getdents_backend_offset(filed_runtime_t *runtime, uint64_t dir_object_id, uint64_t logical_offset);

uint64_t filed_now_ns(void);
uint64_t filed_read_tsc(void);
void filed_record_dispatch_metric(filed_runtime_t *runtime, uint64_t op, uint64_t start_ns, uint64_t end_ns, uint64_t start_cycles, uint64_t end_cycles, int status);
void filed_record_dispatch_metric_cycles(filed_runtime_t *runtime, uint64_t op, uint64_t start_cycles, uint64_t end_cycles, int status);
void filed_record_fast_op_metric_cycles(filed_runtime_t *runtime, uint64_t op, uint64_t start_cycles, uint64_t end_cycles, int status);
void filed_runtime_publish_generation(filed_runtime_t *runtime, filed_handle_id_t handle_id, filed_generation_t object_generation, filed_generation_t dir_generation);
void filed_runtime_publish_backend_object_generation(filed_runtime_t *runtime, filed_backend_object_id_t backend_object);
void filed_dump_dispatch_metrics(filed_runtime_t *runtime);

uint64_t filed_error_token(int64_t status, uint64_t op, uint64_t stage, int64_t detail, uint64_t request_id, uint64_t fd_count, uint64_t subject, uint64_t aux, const char *message);
int filed_send_reply(int reply_fd, void *reply_page, const pacha_service_envelope_t *header, int64_t status, uint64_t result, uint64_t error_token);
int filed_send_session_reply(int channel_fd, uint64_t request_id, int64_t status, uint64_t result);
int filed_send_exec_reply(int reply_fd, uint64_t request_id, int process_fd, int thread_fd, int transfer_process_fd);
int filed_send_exec_self_reply(int reply_fd, uint64_t request_id, int process_fd, int thread_fd, int bootstrap_fd);
int filed_dispatch_set_inherit(int fd, int enabled);
void filed_dispatch_saved_fd_init(filed_dispatch_saved_fd_t *saved);
void filed_dispatch_close_owned_fd(int *fd);
int filed_dispatch_save_target_fd(int target_fd, filed_dispatch_saved_fd_t *saved);
void filed_dispatch_restore_target_fd(int target_fd, filed_dispatch_saved_fd_t *saved);
int filed_dispatch_prepare_inherit_fd_to_target(int source_fd, uint64_t target_raw, int *out_fd, filed_dispatch_saved_fd_t *saved);
int filed_dispatch_dup_endpoint_to_fixed(int source_fd, int target_fd, int *out_fd);
int filed_dispatch_prepare_endpoint_to_fixed(int source_fd, int target_fd, int *out_fd, int *out_borrowed);
void filed_dispatch_close_prepared_endpoint(int *fd, int borrowed);

int64_t filed_status_to_wire(filed_status_t status);
int filed_release_reclaimed_object(filed_runtime_t *runtime, const filed_vfs_reclaim_result_t *reclaim);
int64_t filed_close_handle_runtime(filed_runtime_t *runtime, filed_handle_id_t handle_id);
void filed_write_u64_le(void *base, uint64_t offset, uint64_t value);
filed_vnode_kind_t filed_kind_from_unix_type(uint64_t kind);
uint32_t filed_rights_to_vfs(uint64_t rights);
uint32_t filed_open_flags_to_vfs(uint64_t flags);
uint32_t filed_fd_flags_to_vfs(uint64_t flags);
uint32_t filed_file_status_flags_to_vfs(uint64_t flags);
uint64_t filed_vfs_fd_flags_to_wire(uint32_t flags);
uint64_t filed_vfs_file_status_flags_to_wire(uint32_t flags);
int filed_flags_are_known(uint64_t fd_flags, uint64_t status_flags);
void *filed_map_request_page(const struct pacha_ipc_msg *request, uint64_t size, int *out_fd);
filed_page_dispatch_result_t filed_page_result(int64_t status, uint64_t result);
int filed_write_stat_from_backend(filed_statx_t *out, const storage_statx_reply_t *stat, uint64_t handle_id, uint64_t object_generation, uint64_t dir_generation);
filed_vfs_stat_snapshot_t filed_stat_snapshot_from_backend(const storage_statx_reply_t *stat, uint64_t handle_id, uint64_t object_generation, uint64_t dir_generation);
filed_vfs_stat_snapshot_t filed_directory_snapshot_from_create(uint64_t handle_id, uint64_t mode, uint64_t object_generation, uint64_t dir_generation);
filed_vfs_stat_snapshot_t filed_symlink_snapshot_from_create(uint64_t handle_id, uint64_t target_length, uint64_t object_generation, uint64_t dir_generation);
int filed_write_stat_from_snapshot(filed_statx_t *out, const filed_vfs_stat_snapshot_t *snapshot, uint64_t handle_id);
int filed_backend_object_for_handle(filed_runtime_t *runtime, filed_handle_id_t handle_id, filed_vfs_io_decision_t *out_decision);
int filed_name_is_terminated(const char *name, size_t capacity);
const char *filed_skip_slashes(const char *path);
int filed_path_is_single_component(const char *path);
void filed_close_walk_handle(filed_runtime_t *runtime, filed_handle_id_t handle_id, int owned);
int64_t filed_lookup_component_stat(filed_runtime_t *runtime, filed_handle_id_t parent_handle, const char *name, uint64_t *out_object_id, storage_statx_reply_t *out_stat, bool *out_lookup_owned);
int64_t filed_splice_symlink_target(filed_runtime_t *runtime, uint64_t object_id, const char *rest, char *out_path, size_t out_path_size);
int64_t filed_resolve_parent_path(filed_runtime_t *runtime, filed_handle_id_t base_dir_handle, const char *path, uint32_t parent_rights, filed_handle_id_t *out_parent_handle, int *out_parent_owned, char *out_name, size_t out_name_size);
int64_t filed_openat_path(filed_runtime_t *runtime, const filed_openat_t *openat, filed_vfs_open_result_t *out_open);

filed_page_dispatch_result_t filed_dispatch_openat_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_validate_open_cache_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_stat_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_utimens_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_chmod_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_pread_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_pread_to_vmo_page(filed_runtime_t *runtime, void *page, int vmo_fd);
int filed_dispatch_file_vmo(filed_runtime_t *runtime, int reply_fd, const struct pacha_ipc_msg *request, void *reply_page, const pacha_service_envelope_t *header);
int filed_dispatch_shared_file_vmo(filed_runtime_t *runtime, int reply_fd, const struct pacha_ipc_msg *request, void *reply_page, const pacha_service_envelope_t *header);
filed_page_dispatch_result_t filed_dispatch_memfd_create_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_read_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_pwrite_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_write_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_seek_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_fsync_page(filed_runtime_t *runtime, const struct pacha_ipc_msg *request);
filed_page_dispatch_result_t filed_dispatch_truncate_page(filed_runtime_t *runtime, void *page);
uint64_t filed_lookup_cache_target_object(filed_runtime_t *runtime, filed_handle_id_t parent_handle, uint64_t parent_backend_object, const char *name, bool *out_lookup_owned);
void filed_invalidate_mutated_object(filed_runtime_t *runtime, uint64_t backend_object);
int filed_flush_mutated_object(filed_runtime_t *runtime, uint64_t backend_object);

filed_page_dispatch_result_t filed_dispatch_unlink_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_mkdir_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_mknod_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_symlink_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_readlink_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_link_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_rmdir_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_rename_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_getdents_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_close_page(filed_runtime_t *runtime, const struct pacha_ipc_msg *request);
filed_page_dispatch_result_t filed_dispatch_dup_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_get_flags_page(filed_runtime_t *runtime, void *page);
filed_page_dispatch_result_t filed_dispatch_set_flags_page(filed_runtime_t *runtime, void *page);

filed_page_dispatch_result_t filed_dispatch_exec_path_session_page(filed_runtime_t *runtime, void *page);
int filed_dispatch_exec_path(filed_runtime_t *runtime, int reply_fd, const struct pacha_ipc_msg *request);
int filed_dispatch_exec_self(filed_runtime_t *runtime, int reply_fd, const struct pacha_ipc_msg *request);
int filed_dispatch_session_open(filed_runtime_t *runtime, int reply_fd, const struct pacha_ipc_msg *request, void *page, const pacha_service_envelope_t *header, int *out_channel_fd, int *out_page_fd);
uint64_t filed_import_termd_error(filed_runtime_t *runtime, uint64_t child_token, uint64_t request_id, int64_t status, uint64_t fd_count, uint64_t subject, const char *text);
int filed_dispatch_register_termd_signal_supervisor(filed_runtime_t *runtime, int reply_fd, const struct pacha_ipc_msg *request, void *page, const pacha_service_envelope_t *header);

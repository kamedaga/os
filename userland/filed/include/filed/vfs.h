#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "filed/flags.h"

#define FILED_MAX_MOUNTS 16u
#define FILED_MAX_VNODES 256u
#define FILED_MAX_FILES 256u
#define FILED_MAX_HANDLES 256u
#define FILED_MAX_TRANSFER_LEASES 64u
#define FILED_ID_HINT_SLOTS 1024u

typedef uint32_t filed_mount_id_t;
typedef uint32_t filed_vnode_id_t;
typedef uint32_t filed_file_id_t;
typedef uint32_t filed_handle_id_t;
typedef uint32_t filed_backend_id_t;
typedef uint64_t filed_backend_object_id_t;
typedef uint32_t filed_generation_t;

typedef struct filed_lock {
    atomic_flag flag;
} filed_lock_t;

typedef enum filed_status {
    FILED_OK = 0,
    FILED_ERR_NOT_FOUND,
    FILED_ERR_NOT_DIR,
    FILED_ERR_IS_DIR,
    FILED_ERR_EXISTS,
    FILED_ERR_DENIED,
    FILED_ERR_INVALID,
    FILED_ERR_CROSS_MOUNT,
    FILED_ERR_NOT_EMPTY,
    FILED_ERR_IO,
    FILED_ERR_UNSUPPORTED,
    FILED_ERR_BAD_FORMAT,
    FILED_ERR_INVALID_IMAGE,
    FILED_ERR_LOOP,
    FILED_ERR_OVERFLOW,
    FILED_ERR_FULL,
} filed_status_t;

typedef enum filed_fs_kind {
    FILED_FS_EXT4 = 1,
    FILED_FS_BTRFS = 2,
    FILED_FS_SYNTHETIC = 3,
} filed_fs_kind_t;

typedef enum filed_vnode_kind {
    FILED_VNODE_REGULAR = 1,
    FILED_VNODE_DIRECTORY = 2,
    FILED_VNODE_SYMLINK = 3,
    FILED_VNODE_DEVICE = 4,
    FILED_VNODE_FIFO = 5,
    FILED_VNODE_SOCKET = 6,
} filed_vnode_kind_t;

typedef enum filed_time_update_flags {
    FILED_TIME_UPDATE_ATIME = 1u << 0,
    FILED_TIME_UPDATE_MTIME = 1u << 1,
} filed_time_update_flags_t;

typedef enum filed_handle_target_kind {
    FILED_HANDLE_NONE = 0,
    FILED_HANDLE_VNODE = 1,
    FILED_HANDLE_FILE = 2,
    FILED_HANDLE_MOUNT = 3,
} filed_handle_target_kind_t;

typedef struct filed_mount {
    bool active;
    filed_mount_id_t id;
    filed_vnode_id_t root_vnode;
    filed_backend_id_t backend;
    filed_fs_kind_t fs_kind;
    uint64_t flags;
} filed_mount_t;

typedef struct filed_vnode {
    bool active;
    bool linked;
    bool stat_valid;
    filed_vnode_id_t id;
    filed_mount_id_t mount_id;
    filed_backend_object_id_t backend_object;
    filed_vnode_kind_t kind;
    uint64_t stat_mode;
    uint64_t stat_size;
    uint64_t stat_blocks;
    uint64_t stat_nlink;
    uint64_t stat_kind;
    uint64_t stat_rdev;
    bool stat_times_valid;
    int64_t stat_atime_sec;
    int64_t stat_atime_nsec;
    int64_t stat_mtime_sec;
    int64_t stat_mtime_nsec;
    int64_t stat_ctime_sec;
    int64_t stat_ctime_nsec;
    filed_vnode_id_t parent;
    char name[64];
    filed_generation_t generation;
    filed_generation_t object_generation;
    filed_generation_t dir_generation;
    uint64_t last_used;
    uint32_t refcount;
    filed_lock_t lock;
} filed_vnode_t;

typedef struct filed_open_file {
    bool active;
    filed_file_id_t id;
    filed_vnode_id_t vnode_id;
    int64_t offset;
    uint32_t status_flags;
    uint32_t rights;
    uint32_t refcount;
    filed_lock_t lock;
    filed_lock_t offset_lock;
} filed_open_file_t;

typedef filed_open_file_t filed_file_t;

typedef struct filed_handle {
    bool active;
    filed_handle_id_t id;
    filed_handle_target_kind_t target_kind;
    uint32_t target_id;
    uint32_t rights;
    uint32_t fd_flags;
    filed_generation_t generation;
    uint32_t owner_session;
    int32_t lease_fd;
} filed_handle_t;

typedef struct filed_vfs {
    filed_mount_t mounts[FILED_MAX_MOUNTS];
    filed_vnode_t vnodes[FILED_MAX_VNODES];
    filed_file_t files[FILED_MAX_FILES];
    filed_handle_t handles[FILED_MAX_HANDLES];
    uint16_t vnode_slot_hints[FILED_ID_HINT_SLOTS];
    uint16_t file_slot_hints[FILED_ID_HINT_SLOTS];
    uint16_t handle_slot_hints[FILED_ID_HINT_SLOTS];
    filed_mount_id_t next_mount_id;
    filed_vnode_id_t next_vnode_id;
    filed_file_id_t next_file_id;
    filed_handle_id_t next_handle_id;
    uint64_t vnode_clock;
} filed_vfs_t;

typedef struct filed_vfs_open_result {
    filed_handle_id_t handle_id;
    filed_vnode_id_t vnode_id;
    filed_backend_object_id_t backend_object;
    filed_vnode_kind_t kind;
    filed_generation_t object_generation;
    filed_generation_t dir_generation;
} filed_vfs_open_result_t;

typedef struct filed_vfs_io_decision {
    filed_backend_object_id_t backend_object;
    filed_vnode_kind_t kind;
    uint64_t offset;
    uint64_t length;
    filed_generation_t object_generation;
    filed_generation_t dir_generation;
} filed_vfs_io_decision_t;

typedef struct filed_vfs_handle_flags {
    uint32_t fd_flags;
    uint32_t status_flags;
} filed_vfs_handle_flags_t;

typedef struct filed_vfs_stat_snapshot {
    bool valid;
    uint64_t handle_id;
    uint64_t mode;
    uint64_t size;
    uint64_t blocks;
    uint64_t nlink;
    uint64_t kind;
    uint64_t rdev;
    bool times_valid;
    int64_t atime_sec;
    int64_t atime_nsec;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t ctime_sec;
    int64_t ctime_nsec;
    filed_generation_t object_generation;
    filed_generation_t dir_generation;
} filed_vfs_stat_snapshot_t;

typedef struct filed_vfs_reclaim_result {
    bool released;
    filed_backend_object_id_t backend_object;
} filed_vfs_reclaim_result_t;

typedef bool (*filed_vfs_backend_evictable_fn)(
    void *context,
    filed_backend_object_id_t backend_object);

const char *filed_status_name(filed_status_t status);
void filed_vfs_init(filed_vfs_t *vfs);
bool filed_rights_include(uint32_t available, uint32_t requested);
uint32_t filed_fd_flags_from_open(uint32_t open_flags);
uint32_t filed_file_status_flags_from_open(uint32_t open_flags);

filed_status_t filed_vfs_mount_root(
    filed_vfs_t *vfs,
    filed_fs_kind_t fs_kind,
    filed_backend_id_t backend,
    filed_backend_object_id_t root_backend_object,
    filed_mount_id_t *out_mount_id);

filed_status_t filed_vfs_open_root(
    filed_vfs_t *vfs,
    filed_mount_id_t mount_id,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open);

filed_status_t filed_vfs_open_backend_child(
    filed_vfs_t *vfs,
    filed_handle_id_t parent_handle,
    filed_backend_object_id_t child_backend_object,
    filed_vnode_kind_t child_kind,
    const char *name,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open);

filed_status_t filed_vfs_open_cached_child(
    filed_vfs_t *vfs,
    filed_handle_id_t parent_handle,
    const char *name,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open);
filed_status_t filed_vfs_cached_child_backend_object(
    const filed_vfs_t *vfs,
    filed_handle_id_t parent_handle,
    const char *name,
    filed_backend_object_id_t *out_backend_object);

filed_status_t filed_vfs_create_backend_child(
    filed_vfs_t *vfs,
    filed_handle_id_t parent_handle,
    filed_backend_object_id_t child_backend_object,
    filed_vnode_kind_t child_kind,
    const char *name,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open);

filed_status_t filed_vfs_open_existing(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open);

filed_status_t filed_vfs_open_parent(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open);

filed_status_t filed_vfs_close_handle(filed_vfs_t *vfs, filed_handle_id_t handle_id);
filed_status_t filed_vfs_set_handle_owner(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint32_t owner_session);
filed_status_t filed_vfs_set_handle_lease(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    int lease_fd);
int filed_vfs_get_handle_lease(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id);
filed_status_t filed_vfs_close_handle_ex(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_reclaim_result_t *out_reclaim);
filed_status_t filed_vfs_evict_lru_unused_linked(
    filed_vfs_t *vfs,
    uint32_t max_cached,
    filed_vfs_backend_evictable_fn evictable,
    void *context,
    filed_vfs_reclaim_result_t *out_reclaim);

filed_status_t filed_vfs_dup_handle(
    filed_vfs_t *vfs,
    filed_handle_id_t source_handle_id,
    uint32_t fd_flags,
    filed_handle_id_t *out_handle_id);

filed_status_t filed_vfs_dup_handle_for_exec(
    filed_vfs_t *vfs,
    filed_handle_id_t source_handle_id,
    filed_handle_id_t *out_handle_id);

filed_status_t filed_vfs_get_handle_flags(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_handle_flags_t *out_flags);

filed_status_t filed_vfs_set_handle_flags(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    const filed_vfs_handle_flags_t *flags);

filed_status_t filed_vfs_stat_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_io_decision_t *out_decision);
filed_status_t filed_vfs_get_stat_snapshot(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_stat_snapshot_t *out_snapshot);
filed_status_t filed_vfs_validate_cached_handle_path(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    const char *absolute_path,
    uint32_t rights,
    filed_generation_t object_generation);
filed_status_t filed_vfs_update_stat_snapshot(
    filed_vfs_t *vfs,
    filed_backend_object_id_t backend_object,
    const filed_vfs_stat_snapshot_t *snapshot);
filed_status_t filed_vfs_note_write(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t offset,
    uint64_t bytes_written);
filed_status_t filed_vfs_note_truncate(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t size);
filed_status_t filed_vfs_update_times(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint32_t mask,
    int64_t atime_sec,
    int64_t atime_nsec,
    int64_t mtime_sec,
    int64_t mtime_nsec);
filed_status_t filed_vfs_update_mode(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t mode);

filed_status_t filed_vfs_lookup_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_io_decision_t *out_decision);

filed_status_t filed_vfs_create_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t parent_handle_id,
    const char *name,
    filed_vfs_io_decision_t *out_decision);

filed_status_t filed_vfs_pread_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t offset,
    uint64_t length,
    filed_vfs_io_decision_t *out_decision);

filed_status_t filed_vfs_pwrite_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t offset,
    uint64_t length,
    filed_vfs_io_decision_t *out_decision);

filed_status_t filed_vfs_truncate_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t size,
    filed_vfs_io_decision_t *out_decision);

filed_status_t filed_vfs_unlink_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t parent_handle_id,
    const char *name,
    filed_vfs_io_decision_t *out_decision);

filed_status_t filed_vfs_unlink_commit(
    filed_vfs_t *vfs,
    filed_handle_id_t parent_handle_id,
    const char *name);
filed_status_t filed_vfs_unlink_commit_ex(
    filed_vfs_t *vfs,
    filed_handle_id_t parent_handle_id,
    const char *name,
    filed_vfs_reclaim_result_t *out_reclaim);

filed_status_t filed_vfs_link_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t old_parent_handle_id,
    filed_handle_id_t new_parent_handle_id,
    const char *old_name,
    const char *new_name,
    filed_vfs_io_decision_t *out_old_parent,
    filed_vfs_io_decision_t *out_new_parent);
filed_status_t filed_vfs_link_commit(
    filed_vfs_t *vfs,
    filed_handle_id_t new_parent_handle_id,
    filed_backend_object_id_t child_backend_object,
    filed_vnode_kind_t child_kind,
    const char *new_name,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open);

filed_status_t filed_vfs_rename_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t old_parent_handle_id,
    filed_handle_id_t new_parent_handle_id,
    const char *old_name,
    const char *new_name,
    filed_vfs_io_decision_t *out_old_parent,
    filed_vfs_io_decision_t *out_new_parent);

filed_status_t filed_vfs_rename_commit(
    filed_vfs_t *vfs,
    filed_handle_id_t old_parent_handle_id,
    filed_handle_id_t new_parent_handle_id,
    const char *old_name,
    const char *new_name,
    filed_backend_object_id_t backend_object);
filed_status_t filed_vfs_rename_commit_ex(
    filed_vfs_t *vfs,
    filed_handle_id_t old_parent_handle_id,
    filed_handle_id_t new_parent_handle_id,
    const char *old_name,
    const char *new_name,
    filed_backend_object_id_t backend_object,
    filed_vfs_reclaim_result_t *out_reclaim);

filed_status_t filed_vfs_read_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t length,
    filed_vfs_io_decision_t *out_decision);

filed_status_t filed_vfs_read_commit(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t bytes_read);

filed_status_t filed_vfs_write_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t length,
    filed_vfs_io_decision_t *out_decision);

filed_status_t filed_vfs_write_commit(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t bytes_written);

filed_status_t filed_vfs_seek(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    int64_t offset,
    int whence,
    uint64_t file_size,
    int64_t *out_offset);

filed_status_t filed_vfs_fsync_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_io_decision_t *out_decision);
filed_status_t filed_vfs_close_flush_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_io_decision_t *out_decision);

filed_status_t filed_vfs_getdents_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_io_decision_t *out_decision);

filed_status_t filed_vfs_getdents_commit(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t entries_read);

filed_status_t filed_vfs_check_basic(const filed_vfs_t *vfs);

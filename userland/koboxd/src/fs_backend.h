#pragma once

#include "kobox/module.h"
#include "linux_subsystem/fs/fs.h"

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

enum {
    KOBOXD_FS_BACKEND_NAME_BYTES = 64,
    KOBOXD_FS_BACKEND_INLINE_DATA_BYTES = 64,
    KOBOXD_FS_BACKEND_MAX_OBJECTS = 256,
};

typedef struct koboxd_fs_object {
    uint64_t object_id;
    uint64_t parent_object_id;
    uint64_t inode_number;
    void *inode;
    void *dentry;
    uint16_t mode;
    uint32_t nlink;
    uint64_t size;
    uint64_t blocks;
    uint64_t rdev;
    int64_t atime_sec;
    int64_t atime_nsec;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t ctime_sec;
    int64_t ctime_nsec;
    char name[KOBOXD_FS_BACKEND_NAME_BYTES];
    uint64_t last_used;
    uint32_t references;
    /* Lazily opened Linux struct files kept for the lifetime of filed object
     * references.  Filed splits large operations into transport-sized I/O;
     * those chunks still belong to one Linux open-file lifetime. */
    void *native_read_file;
    void *native_write_file;
    uint8_t used;
    uint8_t linked;
    uint8_t dirty;
    uint8_t release_prepared;
} koboxd_fs_object_t;

typedef struct koboxd_fs_object_stats {
    uint32_t capacity;
    uint32_t used;
    uint32_t referenced;
    uint32_t cached;
    uint64_t evictions;
} koboxd_fs_object_stats_t;

typedef struct koboxd_fs_hotpath_profile {
    uint64_t cache_lookup_cycles;
    uint64_t ext4_lookup_cycles;
    uint64_t object_register_cycles;
    uint64_t ext4_create_cycles;
    uint64_t ext4_rename_cycles;
    uint64_t rename_post_cycles;
    uint64_t ext4_unlink_cycles;
    uint64_t unlink_post_cycles;
    uint64_t object_refresh_cycles;
} koboxd_fs_hotpath_profile_t;

typedef struct koboxd_fs_lock {
    atomic_flag flag;
} koboxd_fs_lock_t;

typedef struct koboxd_fs_backend {
    koboxd_fs_lock_t lock;
    kb_module_t *ext4_module;
    kb_fs_mount_result_t mount_result;
    koboxd_fs_object_t objects[KOBOXD_FS_BACKEND_MAX_OBJECTS];
    uint64_t next_object_id;
    uint64_t object_clock;
    uint64_t object_evictions;
    uint8_t mounted;
    uint8_t metadata_dirty;
    uint32_t deferred_unlinked_count;
} koboxd_fs_backend_t;

typedef struct koboxd_fs_lookup_request {
    char name[KOBOXD_FS_BACKEND_NAME_BYTES];
} koboxd_fs_lookup_request_t;

typedef struct koboxd_fs_io_request {
    uint64_t object_id;
    uint64_t offset;
    uint64_t length;
    uint8_t data[KOBOXD_FS_BACKEND_INLINE_DATA_BYTES];
} koboxd_fs_io_request_t;

typedef struct koboxd_fs_io_reply {
    uint8_t data[KOBOXD_FS_BACKEND_INLINE_DATA_BYTES];
} koboxd_fs_io_reply_t;

int koboxd_fs_backend_mount_ext4(
    koboxd_fs_backend_t *backend,
    kb_module_t *ext4_module,
    kb_fs_block_device_t *root_device);
void koboxd_fs_backend_lock(koboxd_fs_backend_t *backend);
void koboxd_fs_backend_unlock(koboxd_fs_backend_t *backend);
int koboxd_fs_backend_lookup(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint64_t *out_object_id);
int koboxd_fs_backend_pread(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    uint64_t offset,
    void *buffer,
    size_t length,
    size_t buffer_capacity);
int koboxd_fs_backend_pwrite(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    uint64_t offset,
    const void *buffer,
    size_t length);
int koboxd_fs_backend_readlink(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    char *out_target,
    size_t target_capacity,
    size_t *out_length);
int koboxd_fs_backend_symlink(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    const char *target,
    uint64_t *out_object_id);
int koboxd_fs_backend_create(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint16_t mode,
    uint64_t *out_object_id);
int koboxd_fs_backend_truncate(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    uint64_t size);
int koboxd_fs_backend_utimens(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    uint32_t mask,
    int64_t atime_sec,
    int64_t atime_nsec,
    int64_t mtime_sec,
    int64_t mtime_nsec);
int koboxd_fs_backend_chmod(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    uint16_t mode);
int koboxd_fs_backend_unlink(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name);
int koboxd_fs_backend_mkdir(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint16_t mode,
    uint64_t *out_object_id);
int koboxd_fs_backend_mknod(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint16_t mode,
    uint64_t dev,
    uint64_t *out_object_id);
int koboxd_fs_backend_rmdir(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name);
int koboxd_fs_backend_rename(
    koboxd_fs_backend_t *backend,
    uint64_t old_parent_object_id,
    const char *old_name,
    uint64_t new_parent_object_id,
    const char *new_name,
    uint64_t *out_object_id);
int koboxd_fs_backend_release_object(koboxd_fs_backend_t *backend, uint64_t object_id);
int koboxd_fs_backend_fsync(koboxd_fs_backend_t *backend, uint64_t object_id);
int koboxd_fs_backend_sync_all(koboxd_fs_backend_t *backend);
int koboxd_fs_backend_statx(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    koboxd_fs_object_t *out_stat);
int koboxd_fs_backend_getdents(
    koboxd_fs_backend_t *backend,
    uint64_t dir_object_id,
    uint64_t offset,
    koboxd_fs_object_t *out_entries,
    size_t capacity,
    size_t *out_count);
void koboxd_fs_backend_object_stats(
    const koboxd_fs_backend_t *backend,
    koboxd_fs_object_stats_t *out_stats);
void koboxd_fs_hotpath_profile_reset(void);
void koboxd_fs_hotpath_profile_snapshot(koboxd_fs_hotpath_profile_t *out_profile);

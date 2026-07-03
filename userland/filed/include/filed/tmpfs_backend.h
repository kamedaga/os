#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "koboxd/ipc_protocol.h"

enum {
    FILED_TMPFS_NAME_BYTES = KOBOXD_WIRE_FS_NAME_BYTES,
    FILED_TMPFS_PAGE_BYTES = 4096,
    FILED_TMPFS_NODE_MAX_PAGES = 4096,
    FILED_TMPFS_MAX_FILE_BYTES = FILED_TMPFS_PAGE_BYTES * FILED_TMPFS_NODE_MAX_PAGES,
};

typedef struct filed_tmpfs_backend filed_tmpfs_backend_t;

void filed_tmpfs_backend_init(filed_tmpfs_backend_t *backend);
bool filed_tmpfs_backend_is_object(uint64_t object_id);
uint64_t filed_tmpfs_backend_root_object(const filed_tmpfs_backend_t *backend);
int filed_tmpfs_backend_mount_root(filed_tmpfs_backend_t *backend, uint64_t *out_root_object_id);
int filed_tmpfs_backend_lookup(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name, uint64_t *out_object_id);
int filed_tmpfs_backend_statx(filed_tmpfs_backend_t *backend, uint64_t object_id, koboxd_wire_fs_statx_t *out_stat);
int filed_tmpfs_backend_pread(filed_tmpfs_backend_t *backend, uint64_t object_id, uint64_t offset, void *buffer, uint64_t length, uint64_t *out_bytes);
int filed_tmpfs_backend_pwrite(filed_tmpfs_backend_t *backend, uint64_t object_id, uint64_t offset, const void *buffer, uint64_t length, uint64_t *out_bytes);
int filed_tmpfs_backend_fsync(filed_tmpfs_backend_t *backend, uint64_t object_id);
int filed_tmpfs_backend_create(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name, uint64_t mode, uint64_t *out_object_id);
int filed_tmpfs_backend_link(filed_tmpfs_backend_t *backend, uint64_t old_object_id, uint64_t new_parent_object_id, const char *new_name, uint64_t *out_object_id);
int filed_tmpfs_backend_symlink(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name, const char *target, uint64_t target_length, uint64_t *out_object_id);
int filed_tmpfs_backend_readlink(filed_tmpfs_backend_t *backend, uint64_t object_id, char *out_target, uint64_t target_capacity, uint64_t *out_length);
int filed_tmpfs_backend_truncate(filed_tmpfs_backend_t *backend, uint64_t object_id, uint64_t size);
int filed_tmpfs_backend_utimens(filed_tmpfs_backend_t *backend, uint64_t object_id, uint32_t mask, int64_t atime_sec, int64_t atime_nsec, int64_t mtime_sec, int64_t mtime_nsec);
int filed_tmpfs_backend_chmod(filed_tmpfs_backend_t *backend, uint64_t object_id, uint64_t mode);
int filed_tmpfs_backend_unlink(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name);
int filed_tmpfs_backend_mkdir(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name, uint64_t mode, uint64_t *out_object_id);
int filed_tmpfs_backend_rmdir(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name);
int filed_tmpfs_backend_rename(filed_tmpfs_backend_t *backend, uint64_t old_parent_object_id, const char *old_name, uint64_t new_parent_object_id, const char *new_name, uint64_t *out_object_id);
int filed_tmpfs_backend_release_object(filed_tmpfs_backend_t *backend, uint64_t object_id);
int filed_tmpfs_backend_getdents(filed_tmpfs_backend_t *backend, uint64_t dir_object_id, uint64_t offset, koboxd_wire_fs_getdents_t *out_entries);

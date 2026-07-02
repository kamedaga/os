#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "filed/vfs.h"
#include "koboxd/ipc_protocol.h"

enum {
    FILED_TMPFS_MAX_NODES = 128,
    FILED_TMPFS_NAME_BYTES = KOBOXD_WIRE_FS_NAME_BYTES,
    FILED_TMPFS_PAGE_BYTES = 4096,
    FILED_TMPFS_NODE_MAX_PAGES = 4096,
    FILED_TMPFS_PAGE_POOL_PAGES = 4096,
    FILED_TMPFS_MAX_FILE_BYTES = FILED_TMPFS_PAGE_BYTES * FILED_TMPFS_NODE_MAX_PAGES,
    FILED_TMPFS_CHILD_HASH_BUCKETS = 256,
    FILED_TMPFS_NO_NODE = 0xffffu,
};

typedef struct filed_tmpfs_page {
    bool used;
    uint8_t data[FILED_TMPFS_PAGE_BYTES];
} filed_tmpfs_page_t;

typedef struct filed_tmpfs_node {
    bool used;
    bool linked;
    uint16_t slot_index;
    uint16_t parent_slot;
    uint16_t first_child_slot;
    uint16_t next_sibling_slot;
    uint16_t hash_next_slot;
    uint64_t object_id;
    uint64_t parent_object_id;
    uint64_t mode;
    uint64_t size;
    uint64_t generation;
    filed_vnode_kind_t kind;
    uint16_t allocated_page_count;
    uint16_t pages[FILED_TMPFS_NODE_MAX_PAGES];
    uint16_t allocated_page_indices[FILED_TMPFS_NODE_MAX_PAGES];
    char name[FILED_TMPFS_NAME_BYTES];
} filed_tmpfs_node_t;

typedef struct filed_tmpfs_backend {
    filed_lock_t lock;
    uint64_t root_object_id;
    uint64_t next_object_generation;
    uint32_t free_node_count;
    uint32_t free_page_count;
    uint16_t free_node_stack[FILED_TMPFS_MAX_NODES];
    uint16_t free_page_stack[FILED_TMPFS_PAGE_POOL_PAGES];
    uint16_t child_hash_buckets[FILED_TMPFS_CHILD_HASH_BUCKETS];
    filed_tmpfs_node_t nodes[FILED_TMPFS_MAX_NODES];
    filed_tmpfs_page_t pages[FILED_TMPFS_PAGE_POOL_PAGES];
} filed_tmpfs_backend_t;

void filed_tmpfs_backend_init(filed_tmpfs_backend_t *backend);
bool filed_tmpfs_backend_is_object(uint64_t object_id);
int filed_tmpfs_backend_mount_root(filed_tmpfs_backend_t *backend, uint64_t *out_root_object_id);
int filed_tmpfs_backend_lookup(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name, uint64_t *out_object_id);
int filed_tmpfs_backend_statx(filed_tmpfs_backend_t *backend, uint64_t object_id, koboxd_wire_fs_statx_t *out_stat);
int filed_tmpfs_backend_pread(filed_tmpfs_backend_t *backend, uint64_t object_id, uint64_t offset, void *buffer, uint64_t length, uint64_t *out_bytes);
int filed_tmpfs_backend_pwrite(filed_tmpfs_backend_t *backend, uint64_t object_id, uint64_t offset, const void *buffer, uint64_t length, uint64_t *out_bytes);
int filed_tmpfs_backend_fsync(filed_tmpfs_backend_t *backend, uint64_t object_id);
int filed_tmpfs_backend_create(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name, uint64_t mode, uint64_t *out_object_id);
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

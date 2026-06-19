#pragma once

#include <stdint.h>

enum {
    FILED_VFS_MAX_MOUNTS = 8,
    FILED_VFS_MAX_VNODES = 256,
    FILED_VFS_MAX_FILES = 256,
    FILED_VFS_MAX_HANDLES = 256,
    FILED_VFS_PATH_BYTES = 256,
};

typedef enum filed_vnode_kind {
    FILED_VNODE_KIND_UNKNOWN = 0,
    FILED_VNODE_KIND_DIR = 1,
    FILED_VNODE_KIND_REG = 2,
    FILED_VNODE_KIND_SYMLINK = 3,
    FILED_VNODE_KIND_DEVICE = 4,
} filed_vnode_kind_t;

typedef struct filed_vmount {
    uint64_t mount_id;
    int fs_backend_fd;
    uint64_t root_object_id;
    uint64_t generation;
    uint8_t active;
} filed_vmount_t;

typedef struct filed_vnode {
    uint64_t vnode_id;
    uint64_t mount_id;
    uint64_t backend_object_id;
    uint64_t parent_vnode_id;
    uint64_t size;
    uint64_t mode;
    uint64_t generation;
    filed_vnode_kind_t kind;
    char name[FILED_VFS_PATH_BYTES];
    uint8_t active;
} filed_vnode_t;

typedef struct filed_vfile {
    uint64_t file_id;
    uint64_t vnode_id;
    uint64_t offset;
    uint64_t flags;
    uint64_t generation;
    uint8_t active;
} filed_vfile_t;

typedef struct filed_vfs_handle {
    uint64_t handle_id;
    uint64_t object_id;
    uint64_t generation;
    uint64_t rights;
    uint8_t active;
} filed_vfs_handle_t;

typedef struct filed_vfs {
    filed_vmount_t mounts[FILED_VFS_MAX_MOUNTS];
    filed_vnode_t vnodes[FILED_VFS_MAX_VNODES];
    filed_vfile_t files[FILED_VFS_MAX_FILES];
    filed_vfs_handle_t handles[FILED_VFS_MAX_HANDLES];
    uint64_t next_mount_id;
    uint64_t next_vnode_id;
    uint64_t next_file_id;
    uint64_t next_handle_id;
    uint64_t generation;
    int fs_backend_fd;
} filed_vfs_t;

void filed_vfs_init(filed_vfs_t *vfs);
int filed_vfs_attach_root_backend(filed_vfs_t *vfs, int fs_backend_fd);
int filed_vfs_self_check(filed_vfs_t *vfs);

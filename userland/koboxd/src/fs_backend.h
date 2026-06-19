#pragma once

#include "ipc_service.h"
#include "kobox/module.h"
#include "linux_subsystem/fs/fs.h"

#include <stddef.h>
#include <stdint.h>

enum {
    KOBOXD_FS_BACKEND_NAME_BYTES = 64,
    KOBOXD_FS_BACKEND_INLINE_DATA_BYTES = 64,
    KOBOXD_FS_BACKEND_MAX_OBJECTS = 64,
};

typedef struct koboxd_fs_object {
    uint64_t object_id;
    void *inode;
    void *dentry;
    uint16_t mode;
    uint32_t nlink;
    uint64_t size;
    uint64_t blocks;
    char name[KOBOXD_FS_BACKEND_NAME_BYTES];
    uint8_t used;
} koboxd_fs_object_t;

typedef struct koboxd_fs_backend {
    kb_module_t *ext4_module;
    kb_fs_mount_result_t mount_result;
    koboxd_fs_object_t objects[KOBOXD_FS_BACKEND_MAX_OBJECTS];
    uint64_t next_object_id;
    uint8_t mounted;
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
    size_t length);
int koboxd_fs_backend_pwrite(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    uint64_t offset,
    const void *buffer,
    size_t length);
int koboxd_fs_backend_fsync(koboxd_fs_backend_t *backend, uint64_t object_id);
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
int koboxd_fs_backend_handle_ipc(void *ctx, const koboxd_ipc_request_t *request, koboxd_ipc_reply_t *reply);

/* SPDX-License-Identifier: MIT */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <kobox2/filesystem.h>

struct filed_runtime;
typedef uint64_t filed_handle_id_t;

/* These are decoded values, not another filesystem model or a wire overlay. */
typedef struct filed_storage_stat {
    uint64_t inode, size, blocks, filesystem;
    uint64_t change_cookie;
    uint32_t inode_generation, snapshot_valid;
    uint32_t rdev_major, rdev_minor;
    uint32_t mode, uid, gid, nlink;
} filed_storage_stat_t;

int filed_storage_exchange(struct filed_runtime *, const uint8_t *, size_t,
                           uint8_t *, size_t, size_t *);
int filed_storage_request(struct filed_runtime *, kb2_fs_request_t *,
                          const void *, size_t, kb2_fs_response_t *,
                          void *, size_t);
int filed_storage_open(struct filed_runtime *, uint64_t, const char *, uint64_t,
                       uint64_t, uint32_t, uint64_t *);
int filed_storage_stat(struct filed_runtime *, uint64_t, filed_storage_stat_t *);
int filed_storage_snapshot_stat(struct filed_runtime *, uint64_t, filed_storage_stat_t *);
int filed_storage_snapshot_pread(struct filed_runtime *, uint64_t, uint64_t, void *,
    size_t, size_t *, const filed_storage_stat_t *, int *valid);
int filed_storage_device_route(struct filed_runtime *, kb2_fs_request_t *,
    const void *tail, size_t tail_size, filed_storage_stat_t *, uint64_t *handle,
    uint8_t statx[KB2_FILESYSTEM_STATX_SIZE]);
/* Internal snapshot readers may return an arena-sized short read. Their
 * callers assemble the complete image; this is not a client I/O size limit. */
int filed_storage_pread(struct filed_runtime *, uint64_t, uint64_t, void *,
                        size_t, size_t *);
int filed_storage_close(struct filed_runtime *, uint64_t);
/* Worker requests retain the client identity that owned the DUP handle. */
int filed_storage_pager_request(struct filed_runtime *, uint64_t, uint64_t,
    kb2_fs_request_t *, kb2_fs_response_t *, void *, size_t);
int filed_storage_pager_request_ex(struct filed_runtime *,uint64_t,uint64_t,
    kb2_fs_request_t *,const void *,size_t,kb2_fs_response_t *,void *,size_t);
int filed_storage_pager_pread(struct filed_runtime *, uint64_t, uint64_t,
    uint64_t, uint64_t, void *, size_t, size_t *);
int filed_storage_executable_mapping(struct filed_runtime *, uint64_t);
/* These handles belong only to the authenticated owner execution namespace.
 * Never return them to a client or pass them to ordinary file operations. */
int filed_storage_exec_open(struct filed_runtime *, uint64_t, const char *, uint64_t *);
int filed_storage_exec_stat(struct filed_runtime *, uint64_t, filed_storage_stat_t *);
int filed_storage_exec_pread(struct filed_runtime *, uint64_t, uint64_t,
    void *, size_t, size_t *);
int filed_storage_exec_close(struct filed_runtime *, uint64_t);

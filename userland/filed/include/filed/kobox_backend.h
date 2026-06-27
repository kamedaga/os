#pragma once

#include <stddef.h>
#include <stdint.h>

#include "filed/fd_ipc.h"
#include "koboxd/ipc_protocol.h"

enum {
    FILED_KOBOX_BACKEND_METRIC_OP_MAX = 32,
};

typedef struct filed_kobox_backend_metric {
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t total_cycles;
    uint64_t max_cycles;
    uint64_t errors;
} filed_kobox_backend_metric_t;

struct filed_kobox_direct_ops;

typedef struct filed_kobox_backend {
    int fs_fd;
    void *direct_ctx;
    const struct filed_kobox_direct_ops *direct_ops;
    uint64_t root_object_id;
    uint64_t ext4_magic;
    uint64_t calls;
    uint64_t bytes_read;
    uint64_t bytes_written;
    filed_kobox_backend_metric_t metrics[FILED_KOBOX_BACKEND_METRIC_OP_MAX];
    filed_wire_page_t wire_page;
    int wire_page_ready;
} filed_kobox_backend_t;

typedef struct filed_kobox_direct_ops {
    int (*mount_root)(void *ctx, uint64_t *out_magic);
    int (*lookup)(void *ctx, uint64_t parent_object_id, const char *name, uint64_t *out_object_id);
    int (*statx)(void *ctx, uint64_t object_id, koboxd_wire_fs_statx_t *out_stat);
    int (*pread)(void *ctx, uint64_t object_id, uint64_t offset, void *buffer, uint64_t length, uint64_t *out_bytes);
    int (*pwrite)(void *ctx, uint64_t object_id, uint64_t offset, const void *buffer, uint64_t length, uint64_t *out_bytes);
    int (*fsync)(void *ctx, uint64_t object_id);
    int (*create)(void *ctx, uint64_t parent_object_id, const char *name, uint64_t mode, uint64_t *out_object_id);
    int (*truncate)(void *ctx, uint64_t object_id, uint64_t size);
    int (*unlink)(void *ctx, uint64_t parent_object_id, const char *name);
    int (*mkdir)(void *ctx, uint64_t parent_object_id, const char *name, uint64_t mode, uint64_t *out_object_id);
    int (*rmdir)(void *ctx, uint64_t parent_object_id, const char *name);
    int (*rename)(void *ctx, uint64_t old_parent_object_id, const char *old_name, uint64_t new_parent_object_id, const char *new_name, uint64_t *out_object_id);
    int (*release_object)(void *ctx, uint64_t object_id);
    int (*getdents)(void *ctx, uint64_t dir_object_id, uint64_t offset, koboxd_wire_fs_getdents_t *out_entries);
} filed_kobox_direct_ops_t;

void filed_kobox_backend_init(filed_kobox_backend_t *backend, int fs_fd);
void filed_kobox_backend_init_direct(
    filed_kobox_backend_t *backend,
    void *direct_ctx,
    const filed_kobox_direct_ops_t *direct_ops);
void filed_kobox_backend_dump_metrics(const filed_kobox_backend_t *backend);
int filed_kobox_backend_mount_root(filed_kobox_backend_t *backend);
int filed_kobox_backend_lookup(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint64_t *out_object_id);
int filed_kobox_backend_statx(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    koboxd_wire_fs_statx_t *out_stat);
int filed_kobox_backend_pread(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    uint64_t offset,
    void *buffer,
    uint64_t length,
    uint64_t *out_bytes);
int filed_kobox_backend_pwrite(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    uint64_t offset,
    const void *buffer,
    uint64_t length,
    uint64_t *out_bytes);
int filed_kobox_backend_fsync(
    filed_kobox_backend_t *backend,
    uint64_t object_id);
int filed_kobox_backend_create(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint64_t mode,
    uint64_t *out_object_id);
int filed_kobox_backend_truncate(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    uint64_t size);
int filed_kobox_backend_unlink(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name);
int filed_kobox_backend_mkdir(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint64_t mode,
    uint64_t *out_object_id);
int filed_kobox_backend_rmdir(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name);
int filed_kobox_backend_rename(
    filed_kobox_backend_t *backend,
    uint64_t old_parent_object_id,
    const char *old_name,
    uint64_t new_parent_object_id,
    const char *new_name,
    uint64_t *out_object_id);
int filed_kobox_backend_release_object(
    filed_kobox_backend_t *backend,
    uint64_t object_id);
int filed_kobox_backend_getdents(
    filed_kobox_backend_t *backend,
    uint64_t dir_object_id,
    uint64_t offset,
    koboxd_wire_fs_getdents_t *out_entries);

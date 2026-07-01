#include "filed_direct_backend.h"

#include "fs_backend.h"

#include <stdio.h>
#include <string.h>

static koboxd_fs_backend_t *direct_backend(void *ctx)
{
    return (koboxd_fs_backend_t *)ctx;
}

static int direct_mount_root(void *ctx, uint64_t *out_magic)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL || out_magic == NULL || !backend->mounted) {
        return -22;
    }
    koboxd_fs_backend_lock(backend);
    *out_magic = backend->mount_result.observed_ext4_magic;
    koboxd_fs_backend_unlock(backend);
    return 0;
}

static int direct_lookup(
    void *ctx,
    uint64_t parent_object_id,
    const char *name,
    uint64_t *out_object_id)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL) {
        return -22;
    }
    koboxd_fs_backend_lock(backend);
    const int status = koboxd_fs_backend_lookup(
        backend,
        parent_object_id,
        name,
        out_object_id);
    koboxd_fs_backend_unlock(backend);
    return status;
}

static int direct_statx(void *ctx, uint64_t object_id, koboxd_wire_fs_statx_t *out_stat)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL || out_stat == NULL) {
        return -22;
    }
    koboxd_fs_object_t stat;
    memset(&stat, 0, sizeof(stat));
    koboxd_fs_backend_lock(backend);
    const int status = koboxd_fs_backend_statx(backend, object_id, &stat);
    koboxd_fs_backend_unlock(backend);
    if (status != 0) {
        return status;
    }
    memset(out_stat, 0, sizeof(*out_stat));
    out_stat->object_id = stat.object_id;
    out_stat->mode = stat.mode;
    out_stat->size = stat.size;
    out_stat->blocks = stat.blocks;
    out_stat->nlink = stat.nlink;
    out_stat->kind = stat.mode & 0170000u;
    out_stat->atime_sec = stat.atime_sec;
    out_stat->atime_nsec = stat.atime_nsec;
    out_stat->mtime_sec = stat.mtime_sec;
    out_stat->mtime_nsec = stat.mtime_nsec;
    out_stat->ctime_sec = stat.ctime_sec;
    out_stat->ctime_nsec = stat.ctime_nsec;
    return 0;
}

static int direct_pread(
    void *ctx,
    uint64_t object_id,
    uint64_t offset,
    void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL || buffer == NULL || out_bytes == NULL) {
        return -22;
    }
    koboxd_fs_backend_lock(backend);
    const int status = koboxd_fs_backend_pread(
        backend,
        object_id,
        offset,
        buffer,
        (size_t)length,
        (size_t)length);
    koboxd_fs_backend_unlock(backend);
    if (status < 0) {
        return status;
    }
    *out_bytes = (uint64_t)status;
    return 0;
}

static int direct_pwrite(
    void *ctx,
    uint64_t object_id,
    uint64_t offset,
    const void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL || buffer == NULL || out_bytes == NULL) {
        return -22;
    }
    koboxd_fs_backend_lock(backend);
    const int status = koboxd_fs_backend_pwrite(
        backend,
        object_id,
        offset,
        buffer,
        (size_t)length);
    koboxd_fs_backend_unlock(backend);
    if (status < 0) {
        return status;
    }
    *out_bytes = (uint64_t)status;
    return 0;
}

static int direct_fsync(void *ctx, uint64_t object_id)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL) {
        return -22;
    }
    koboxd_fs_backend_lock(backend);
    const int status = koboxd_fs_backend_fsync(backend, object_id);
    koboxd_fs_backend_unlock(backend);
    return status;
}

static int direct_create(
    void *ctx,
    uint64_t parent_object_id,
    const char *name,
    uint64_t mode,
    uint64_t *out_object_id)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL) {
        return -22;
    }
    koboxd_fs_backend_lock(backend);
    const int status = koboxd_fs_backend_create(
        backend,
        parent_object_id,
        name,
        (uint16_t)mode,
        out_object_id);
    koboxd_fs_backend_unlock(backend);
    return status;
}

static int direct_truncate(void *ctx, uint64_t object_id, uint64_t size)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL) {
        return -22;
    }
    koboxd_fs_backend_lock(backend);
    const int status = koboxd_fs_backend_truncate(backend, object_id, size);
    koboxd_fs_backend_unlock(backend);
    return status;
}

static int direct_utimens(
    void *ctx,
    uint64_t object_id,
    uint32_t mask,
    int64_t atime_sec,
    int64_t atime_nsec,
    int64_t mtime_sec,
    int64_t mtime_nsec)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL) {
        return -22;
    }
    koboxd_fs_backend_lock(backend);
    const int status = koboxd_fs_backend_utimens(
        backend,
        object_id,
        mask,
        atime_sec,
        atime_nsec,
        mtime_sec,
        mtime_nsec);
    koboxd_fs_backend_unlock(backend);
    return status;
}

static int direct_chmod(void *ctx, uint64_t object_id, uint64_t mode)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL) {
        return -22;
    }
    koboxd_fs_backend_lock(backend);
    const int status = koboxd_fs_backend_chmod(backend, object_id, (uint16_t)mode);
    koboxd_fs_backend_unlock(backend);
    return status;
}

static int direct_unlink(void *ctx, uint64_t parent_object_id, const char *name)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL) {
        return -22;
    }
    koboxd_fs_backend_lock(backend);
    const int status = koboxd_fs_backend_unlink(backend, parent_object_id, name);
    koboxd_fs_backend_unlock(backend);
    return status;
}

static int direct_mkdir(
    void *ctx,
    uint64_t parent_object_id,
    const char *name,
    uint64_t mode,
    uint64_t *out_object_id)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL) {
        return -22;
    }
    koboxd_fs_backend_lock(backend);
    const int status = koboxd_fs_backend_mkdir(
        backend,
        parent_object_id,
        name,
        (uint16_t)mode,
        out_object_id);
    koboxd_fs_backend_unlock(backend);
    return status;
}

static int direct_rmdir(void *ctx, uint64_t parent_object_id, const char *name)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL) {
        return -22;
    }
    koboxd_fs_backend_lock(backend);
    const int status = koboxd_fs_backend_rmdir(backend, parent_object_id, name);
    koboxd_fs_backend_unlock(backend);
    return status;
}

static int direct_rename(
    void *ctx,
    uint64_t old_parent_object_id,
    const char *old_name,
    uint64_t new_parent_object_id,
    const char *new_name,
    uint64_t *out_object_id)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL) {
        return -22;
    }
    koboxd_fs_backend_lock(backend);
    const int status = koboxd_fs_backend_rename(
        backend,
        old_parent_object_id,
        old_name,
        new_parent_object_id,
        new_name,
        out_object_id);
    koboxd_fs_backend_unlock(backend);
    return status;
}

static int direct_release_object(void *ctx, uint64_t object_id)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL) {
        return -22;
    }
    koboxd_fs_backend_lock(backend);
    const int status = koboxd_fs_backend_release_object(backend, object_id);
    koboxd_fs_backend_unlock(backend);
    return status;
}

static int direct_getdents(
    void *ctx,
    uint64_t dir_object_id,
    uint64_t offset,
    koboxd_wire_fs_getdents_t *out_entries)
{
    koboxd_fs_backend_t *backend = direct_backend(ctx);
    if (backend == NULL || out_entries == NULL) {
        return -22;
    }

    koboxd_fs_object_t entries[KOBOXD_WIRE_FS_DIRENT_CAPACITY];
    memset(entries, 0, sizeof(entries));
    size_t count = 0;
    size_t capacity = (size_t)out_entries->capacity;
    if (capacity > KOBOXD_WIRE_FS_DIRENT_CAPACITY) {
        capacity = KOBOXD_WIRE_FS_DIRENT_CAPACITY;
    }

    koboxd_fs_backend_lock(backend);
    const int status = koboxd_fs_backend_getdents(
        backend,
        dir_object_id,
        offset,
        entries,
        capacity,
        &count);
    koboxd_fs_backend_unlock(backend);
    if (status != 0) {
        return status;
    }

    memset(out_entries, 0, sizeof(*out_entries));
    out_entries->dir_object_id = dir_object_id;
    out_entries->offset = offset;
    out_entries->capacity = capacity;
    out_entries->count = count;
    for (size_t i = 0; i < count; i++) {
        out_entries->entries[i].object_id = entries[i].object_id;
        out_entries->entries[i].kind = entries[i].mode & 0170000u;
        out_entries->entries[i].name_len = strlen(entries[i].name);
        snprintf(out_entries->entries[i].name, sizeof(out_entries->entries[i].name), "%s", entries[i].name);
    }
    return 0;
}

static const filed_kobox_direct_ops_t direct_ops = {
    .mount_root = direct_mount_root,
    .lookup = direct_lookup,
    .statx = direct_statx,
    .pread = direct_pread,
    .pwrite = direct_pwrite,
    .fsync = direct_fsync,
    .create = direct_create,
    .truncate = direct_truncate,
    .utimens = direct_utimens,
    .chmod = direct_chmod,
    .unlink = direct_unlink,
    .mkdir = direct_mkdir,
    .rmdir = direct_rmdir,
    .rename = direct_rename,
    .release_object = direct_release_object,
    .getdents = direct_getdents,
};

const filed_kobox_direct_ops_t *koboxd_filed_direct_ops(void)
{
    return &direct_ops;
}

#include "dispatch/common.h"

bool filed_backend_object_is_tmpfs(uint64_t backend_object)
{
    return backend_object != 0 && filed_tmpfs_backend_is_object(backend_object);
}

int filed_backend_lookup(
    filed_runtime_t *runtime,
    uint64_t parent_object_id,
    const char *name,
    uint64_t *out_object_id)
{
    if (filed_tmpfs_backend_is_object(parent_object_id)) {
        return filed_tmpfs_backend_lookup(&runtime->tmpfs, parent_object_id, name, out_object_id);
    }
    return filed_kobox_backend_lookup(&runtime->backend, parent_object_id, name, out_object_id);
}

int filed_backend_statx(
    filed_runtime_t *runtime,
    uint64_t object_id,
    storage_statx_reply_t *out_stat)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_statx(&runtime->tmpfs, object_id, out_stat);
    }
    return filed_kobox_backend_statx(&runtime->backend, object_id, out_stat);
}

int filed_backend_pread(
    filed_runtime_t *runtime,
    uint64_t object_id,
    uint64_t offset,
    void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_pread(&runtime->tmpfs, object_id, offset, buffer, length, out_bytes);
    }
    return filed_kobox_backend_pread(&runtime->backend, object_id, offset, buffer, length, out_bytes);
}

int filed_backend_pwrite(
    filed_runtime_t *runtime,
    uint64_t object_id,
    uint64_t offset,
    const void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_pwrite(&runtime->tmpfs, object_id, offset, buffer, length, out_bytes);
    }
    return filed_kobox_backend_pwrite(&runtime->backend, object_id, offset, buffer, length, out_bytes);
}

int filed_backend_fsync(filed_runtime_t *runtime, uint64_t object_id)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_fsync(&runtime->tmpfs, object_id);
    }
    return filed_kobox_backend_fsync(&runtime->backend, object_id);
}

int filed_backend_create(
    filed_runtime_t *runtime,
    uint64_t parent_object_id,
    const char *name,
    uint64_t mode,
    uint64_t *out_object_id)
{
    if (filed_tmpfs_backend_is_object(parent_object_id)) {
        return filed_tmpfs_backend_create(&runtime->tmpfs, parent_object_id, name, mode, out_object_id);
    }
    return filed_kobox_backend_create(&runtime->backend, parent_object_id, name, mode, out_object_id);
}

int filed_backend_truncate(
    filed_runtime_t *runtime,
    uint64_t object_id,
    uint64_t size)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_truncate(&runtime->tmpfs, object_id, size);
    }
    return filed_kobox_backend_truncate(&runtime->backend, object_id, size);
}

int filed_backend_utimens(
    filed_runtime_t *runtime,
    uint64_t object_id,
    uint32_t mask,
    int64_t atime_sec,
    int64_t atime_nsec,
    int64_t mtime_sec,
    int64_t mtime_nsec)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_utimens(
            &runtime->tmpfs,
            object_id,
            mask,
            atime_sec,
            atime_nsec,
            mtime_sec,
            mtime_nsec);
    }
    return filed_kobox_backend_utimens(
        &runtime->backend,
        object_id,
        mask,
        atime_sec,
        atime_nsec,
        mtime_sec,
        mtime_nsec);
}

int filed_backend_chmod(
    filed_runtime_t *runtime,
    uint64_t object_id,
    uint64_t mode)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_chmod(&runtime->tmpfs, object_id, mode);
    }
    return filed_kobox_backend_chmod(&runtime->backend, object_id, mode);
}

int filed_backend_unlink(
    filed_runtime_t *runtime,
    uint64_t parent_object_id,
    const char *name)
{
    if (filed_tmpfs_backend_is_object(parent_object_id)) {
        return filed_tmpfs_backend_unlink(&runtime->tmpfs, parent_object_id, name);
    }
    return filed_kobox_backend_unlink(&runtime->backend, parent_object_id, name);
}

int filed_backend_link(
    filed_runtime_t *runtime,
    uint64_t old_object_id,
    uint64_t new_parent_object_id,
    const char *new_name,
    uint64_t *out_object_id)
{
    if (filed_tmpfs_backend_is_object(old_object_id) &&
        filed_tmpfs_backend_is_object(new_parent_object_id))
    {
        return filed_tmpfs_backend_link(
            &runtime->tmpfs,
            old_object_id,
            new_parent_object_id,
            new_name,
            out_object_id);
    }
    if (filed_tmpfs_backend_is_object(old_object_id) ||
        filed_tmpfs_backend_is_object(new_parent_object_id))
    {
        return -18;
    }
    (void)runtime;
    (void)old_object_id;
    (void)new_parent_object_id;
    (void)new_name;
    (void)out_object_id;
    return -95;
}

int filed_backend_mkdir(
    filed_runtime_t *runtime,
    uint64_t parent_object_id,
    const char *name,
    uint64_t mode,
    uint64_t *out_object_id)
{
    if (filed_tmpfs_backend_is_object(parent_object_id)) {
        return filed_tmpfs_backend_mkdir(&runtime->tmpfs, parent_object_id, name, mode, out_object_id);
    }
    return filed_kobox_backend_mkdir(&runtime->backend, parent_object_id, name, mode, out_object_id);
}

int filed_backend_mknod(
    filed_runtime_t *runtime,
    uint64_t parent_object_id,
    const char *name,
    uint64_t mode,
    uint64_t dev,
    uint64_t *out_object_id)
{
    if (filed_tmpfs_backend_is_object(parent_object_id)) {
        return filed_tmpfs_backend_mknod(
            &runtime->tmpfs, parent_object_id, name, mode, dev, out_object_id);
    }
    return filed_kobox_backend_mknod(
        &runtime->backend, parent_object_id, name, mode, dev, out_object_id);
}

int filed_backend_symlink(
    filed_runtime_t *runtime,
    uint64_t parent_object_id,
    const char *name,
    const char *target,
    uint64_t target_length,
    uint64_t *out_object_id)
{
    if (filed_tmpfs_backend_is_object(parent_object_id)) {
        return filed_tmpfs_backend_symlink(
            &runtime->tmpfs,
            parent_object_id,
            name,
            target,
            target_length,
            out_object_id);
    }
    (void)runtime;
    (void)parent_object_id;
    (void)name;
    (void)target;
    (void)target_length;
    (void)out_object_id;
    return -95;
}

int filed_backend_readlink(
    filed_runtime_t *runtime,
    uint64_t object_id,
    char *out_target,
    uint64_t target_capacity,
    uint64_t *out_length)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_readlink(
            &runtime->tmpfs,
            object_id,
            out_target,
            target_capacity,
            out_length);
    }
    return filed_kobox_backend_readlink(
        &runtime->backend,
        object_id,
        out_target,
        target_capacity,
        out_length);
}

int filed_backend_rmdir(
    filed_runtime_t *runtime,
    uint64_t parent_object_id,
    const char *name)
{
    if (filed_tmpfs_backend_is_object(parent_object_id)) {
        return filed_tmpfs_backend_rmdir(&runtime->tmpfs, parent_object_id, name);
    }
    return filed_kobox_backend_rmdir(&runtime->backend, parent_object_id, name);
}

int filed_backend_rename(
    filed_runtime_t *runtime,
    uint64_t old_parent_object_id,
    const char *old_name,
    uint64_t new_parent_object_id,
    const char *new_name,
    uint64_t *out_object_id)
{
    const bool old_tmpfs = filed_tmpfs_backend_is_object(old_parent_object_id);
    const bool new_tmpfs = filed_tmpfs_backend_is_object(new_parent_object_id);
    if (old_tmpfs != new_tmpfs) {
        return -18;
    }
    if (old_tmpfs) {
        return filed_tmpfs_backend_rename(
            &runtime->tmpfs,
            old_parent_object_id,
            old_name,
            new_parent_object_id,
            new_name,
            out_object_id);
    }
    return filed_kobox_backend_rename(
        &runtime->backend,
        old_parent_object_id,
        old_name,
        new_parent_object_id,
        new_name,
        out_object_id);
}

int filed_backend_release_object(filed_runtime_t *runtime, uint64_t object_id)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_release_object(&runtime->tmpfs, object_id);
    }
    return filed_kobox_backend_release_object(&runtime->backend, object_id);
}

int filed_backend_getdents(
    filed_runtime_t *runtime,
    uint64_t dir_object_id,
    uint64_t offset,
    storage_getdents_request_t *out_entries)
{
    if (filed_tmpfs_backend_is_object(dir_object_id)) {
        return filed_tmpfs_backend_getdents(&runtime->tmpfs, dir_object_id, offset, out_entries);
    }
    return filed_kobox_backend_getdents(&runtime->backend, dir_object_id, offset, out_entries);
}

bool filed_root_getdents_splices_tmpfs(
    filed_runtime_t *runtime,
    uint64_t dir_object_id)
{
    if (runtime == NULL ||
        dir_object_id != runtime->backend.root_object_id ||
        filed_tmpfs_backend_root_object(&runtime->tmpfs) == 0 ||
        runtime->root_tmpfs_synthetic_dirent == 0)
    {
        return false;
    }
    return true;
}

uint64_t filed_root_getdents_backend_offset(
    filed_runtime_t *runtime,
    uint64_t dir_object_id,
    uint64_t logical_offset)
{
    if (filed_root_getdents_splices_tmpfs(runtime, dir_object_id) && logical_offset > 0) {
        return logical_offset - 1u;
    }
    return logical_offset;
}

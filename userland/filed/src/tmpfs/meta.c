#include "private.h"

int filed_tmpfs_backend_statx(filed_tmpfs_backend_t *backend, uint64_t object_id, storage_v2_statx_reply_t *out_stat)
{
    if (backend == NULL || out_stat == NULL) {
        return -22;
    }
    memset(out_stat, 0, sizeof(*out_stat));
    filed_tmpfs_lock_acquire(&backend->lock);
    const filed_tmpfs_inode_t *inode = filed_tmpfs_find_inode(backend, object_id);
    if (inode == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    out_stat->object_id = inode->object_id;
    out_stat->mode = inode->mode;
    out_stat->size = inode->size;
    out_stat->blocks = (uint64_t)inode->allocated_page_count * (FILED_TMPFS_PAGE_BYTES / 512u);
    out_stat->nlink = inode->kind == FILED_VNODE_DIRECTORY ?
        filed_tmpfs_dir_nlink_locked(backend, inode->object_id) :
        inode->nlink;
    out_stat->kind = inode->mode & FILED_TMPFS_MODE_TYPE_MASK;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_fsync(filed_tmpfs_backend_t *backend, uint64_t object_id)
{
    if (backend == NULL) {
        return -22;
    }
    filed_tmpfs_lock_acquire(&backend->lock);
    const filed_tmpfs_inode_t *inode = filed_tmpfs_find_inode(backend, object_id);
    filed_tmpfs_lock_release(&backend->lock);
    return inode == NULL ? -2 : 0;
}

int filed_tmpfs_backend_utimens(
    filed_tmpfs_backend_t *backend,
    uint64_t object_id,
    uint32_t mask,
    int64_t atime_sec,
    int64_t atime_nsec,
    int64_t mtime_sec,
    int64_t mtime_nsec)
{
    (void)mask;
    (void)atime_sec;
    (void)atime_nsec;
    (void)mtime_sec;
    (void)mtime_nsec;
    if (backend == NULL) {
        return -22;
    }
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_inode_t *inode = filed_tmpfs_find_inode(backend, object_id);
    if (inode == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    ++inode->generation;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_chmod(filed_tmpfs_backend_t *backend, uint64_t object_id, uint64_t mode)
{
    if (backend == NULL) {
        return -22;
    }
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_inode_t *inode = filed_tmpfs_find_inode(backend, object_id);
    if (inode == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    inode->mode = filed_tmpfs_mode_for_kind(inode->kind, mode);
    ++inode->generation;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

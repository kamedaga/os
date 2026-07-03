#include "private.h"

filed_tmpfs_inode_t *filed_tmpfs_find_inode(filed_tmpfs_backend_t *backend, uint64_t object_id)
{
    if (backend == NULL || object_id == 0 || !filed_tmpfs_backend_is_object(object_id)) {
        return NULL;
    }
    const uint16_t encoded_slot = (uint16_t)(object_id & FILED_TMPFS_OBJECT_SLOT_MASK);
    if (encoded_slot == 0) {
        return NULL;
    }
    const uint16_t slot = (uint16_t)(encoded_slot - 1u);
    filed_tmpfs_inode_t *inode = filed_tmpfs_inode_by_slot(backend, slot);
    if (inode == NULL || inode->object_id != object_id) {
        return NULL;
    }
    return inode;
}

uint16_t filed_tmpfs_inode_slot(const filed_tmpfs_backend_t *backend, const filed_tmpfs_inode_t *inode)
{
    if (backend == NULL || inode == NULL) {
        return FILED_TMPFS_NO_SLOT;
    }
    const uintptr_t base = (uintptr_t)&backend->inodes[0];
    const uintptr_t ptr = (uintptr_t)inode;
    const uintptr_t end = (uintptr_t)&backend->inodes[FILED_TMPFS_MAX_INODES];
    if (ptr < base || ptr >= end) {
        return FILED_TMPFS_NO_SLOT;
    }
    return (uint16_t)((ptr - base) / sizeof(backend->inodes[0]));
}

filed_tmpfs_inode_t *filed_tmpfs_inode_by_slot(filed_tmpfs_backend_t *backend, uint16_t slot)
{
    if (backend == NULL || slot >= FILED_TMPFS_MAX_INODES) {
        return NULL;
    }
    filed_tmpfs_inode_t *inode = &backend->inodes[slot];
    return inode->used ? inode : NULL;
}

filed_tmpfs_inode_t *filed_tmpfs_alloc_inode(filed_tmpfs_backend_t *backend)
{
    if (backend == NULL || backend->free_inode_count == 0) {
        return NULL;
    }
    const uint16_t slot = backend->free_inode_stack[--backend->free_inode_count];
    if (slot == 0 || slot >= FILED_TMPFS_MAX_INODES || backend->inodes[slot].used) {
        return NULL;
    }
    return &backend->inodes[slot];
}

void filed_tmpfs_free_inode_if_dead(filed_tmpfs_backend_t *backend, filed_tmpfs_inode_t *inode)
{
    if (backend == NULL || inode == NULL || inode->object_id == backend->root_object_id || inode->nlink != 0) {
        return;
    }
    const uint16_t slot = inode->slot_index;
    filed_tmpfs_free_inode_pages(backend, inode, 0);
    memset(inode, 0, sizeof(*inode));
    if (slot != 0 && slot < FILED_TMPFS_MAX_INODES && backend->free_inode_count < FILED_TMPFS_MAX_INODES) {
        backend->free_inode_stack[backend->free_inode_count++] = slot;
    }
}

int filed_tmpfs_backend_release_object(filed_tmpfs_backend_t *backend, uint64_t object_id)
{
    if (backend == NULL) {
        return -22;
    }
    if (object_id == backend->root_object_id) {
        return 0;
    }

    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_inode_t *inode = filed_tmpfs_find_inode(backend, object_id);
    if (inode == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    filed_tmpfs_free_inode_if_dead(backend, inode);
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

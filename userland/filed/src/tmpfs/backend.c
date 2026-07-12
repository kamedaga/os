#include "private.h"

void filed_tmpfs_lock_acquire(filed_lock_t *lock)
{
    if (lock == NULL) {
        return;
    }
    while (atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire)) {
    }
}

void filed_tmpfs_lock_release(filed_lock_t *lock)
{
    if (lock == NULL) {
        return;
    }
    atomic_flag_clear_explicit(&lock->flag, memory_order_release);
}

bool filed_tmpfs_backend_is_object(uint64_t object_id)
{
    return (object_id & FILED_TMPFS_OBJECT_TAG) != 0;
}

uint64_t filed_tmpfs_backend_root_object(const filed_tmpfs_backend_t *backend)
{
    return backend != NULL ? backend->root_object_id : 0;
}

uint64_t filed_tmpfs_make_object_id(uint16_t slot, uint64_t object_generation)
{
    if (slot >= FILED_TMPFS_MAX_INODES) {
        return 0;
    }
    if (object_generation == 0) {
        object_generation = 1;
    }
    return FILED_TMPFS_OBJECT_TAG |
        ((object_generation & 0x00007fffffffffffull) << 16) |
        ((uint64_t)slot + 1u);
}

uint64_t filed_tmpfs_mode_for_kind(filed_vnode_kind_t kind, uint64_t mode)
{
    const uint64_t perms = mode & 07777u;
    if (kind == FILED_VNODE_DIRECTORY) {
        return FILED_TMPFS_MODE_DIRECTORY | (perms == 0 ? 0755u : perms);
    }
    if (kind == FILED_VNODE_SYMLINK) {
        return FILED_TMPFS_MODE_SYMLINK | 0777u;
    }
    if (kind == FILED_VNODE_DEVICE || kind == FILED_VNODE_FIFO ||
        kind == FILED_VNODE_SOCKET) {
        return (mode & FILED_TMPFS_MODE_TYPE_MASK) | perms;
    }
    return FILED_TMPFS_MODE_REGULAR | (perms == 0 ? 0644u : perms);
}

int filed_tmpfs_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    for (size_t i = 0; name[i] != '\0'; ++i) {
        if (name[i] == '/' || i + 1u >= FILED_TMPFS_NAME_BYTES) {
            return 0;
        }
    }
    return 1;
}

void filed_tmpfs_backend_init(filed_tmpfs_backend_t *backend)
{
    if (backend == NULL) {
        return;
    }

    memset(backend, 0, sizeof(*backend));
    atomic_flag_clear(&backend->lock.flag);
    backend->root_object_id = filed_tmpfs_make_object_id(0, FILED_TMPFS_ROOT_OBJECT_GENERATION);
    backend->next_object_generation = FILED_TMPFS_ROOT_OBJECT_GENERATION + 1u;

    backend->free_inode_count = 0;
    for (size_t i = 1; i < FILED_TMPFS_MAX_INODES; ++i) {
        backend->free_inode_stack[backend->free_inode_count++] = (uint16_t)(FILED_TMPFS_MAX_INODES - i);
    }
    backend->free_dentry_count = 0;
    for (size_t i = 1; i < FILED_TMPFS_MAX_DENTRIES; ++i) {
        backend->free_dentry_stack[backend->free_dentry_count++] = (uint16_t)(FILED_TMPFS_MAX_DENTRIES - i);
    }
    backend->free_page_count = FILED_TMPFS_PAGE_POOL_PAGES;
    for (size_t i = 0; i < FILED_TMPFS_PAGE_POOL_PAGES; ++i) {
        backend->free_page_stack[i] = (uint16_t)(FILED_TMPFS_PAGE_POOL_PAGES - i);
    }
    for (size_t i = 0; i < FILED_TMPFS_CHILD_HASH_BUCKETS; ++i) {
        backend->child_hash_buckets[i] = FILED_TMPFS_NO_SLOT;
    }

    filed_tmpfs_inode_t *root = &backend->inodes[0];
    root->used = true;
    root->slot_index = 0;
    root->primary_dentry_slot = 0;
    root->first_child_dentry_slot = FILED_TMPFS_NO_SLOT;
    root->object_id = backend->root_object_id;
    root->mode = FILED_TMPFS_MODE_DIRECTORY | 0755u;
    root->generation = 1;
    root->nlink = 1;
    root->kind = FILED_VNODE_DIRECTORY;

    filed_tmpfs_dentry_t *root_dentry = &backend->dentries[0];
    root_dentry->used = true;
    root_dentry->linked = true;
    root_dentry->slot_index = 0;
    root_dentry->parent_inode_slot = 0;
    root_dentry->inode_slot = 0;
    root_dentry->next_sibling_slot = FILED_TMPFS_NO_SLOT;
    root_dentry->hash_next_slot = FILED_TMPFS_NO_SLOT;
    snprintf(root_dentry->name, sizeof(root_dentry->name), "%s", "/");
}

int filed_tmpfs_backend_mount_root(filed_tmpfs_backend_t *backend, uint64_t *out_root_object_id)
{
    if (backend == NULL || out_root_object_id == NULL) {
        return -22;
    }
    *out_root_object_id = backend->root_object_id;
    return 0;
}

#include "private.h"

static int filed_tmpfs_inode_is_data_file(const filed_tmpfs_inode_t *inode)
{
    return inode != NULL &&
        (inode->kind == FILED_VNODE_REGULAR || inode->kind == FILED_VNODE_SYMLINK);
}

int filed_tmpfs_backend_pread(
    filed_tmpfs_backend_t *backend,
    uint64_t object_id,
    uint64_t offset,
    void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    if (backend == NULL || buffer == NULL || out_bytes == NULL) {
        return -22;
    }
    *out_bytes = 0;
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_inode_t *inode = filed_tmpfs_find_inode(backend, object_id);
    if (inode == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (inode->kind == FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -21;
    }
    if (!filed_tmpfs_inode_is_data_file(inode)) {
        filed_tmpfs_lock_release(&backend->lock);
        return -22;
    }
    if (offset >= inode->size) {
        filed_tmpfs_lock_release(&backend->lock);
        return 0;
    }
    uint64_t available = inode->size - offset;
    if (length > available) {
        length = available;
    }
    uint64_t total = 0;
    while (total < length) {
        const uint64_t absolute = offset + total;
        const uint64_t page_index = absolute / FILED_TMPFS_PAGE_BYTES;
        const uint64_t page_offset = absolute % FILED_TMPFS_PAGE_BYTES;
        uint64_t chunk = length - total;
        if (chunk > FILED_TMPFS_PAGE_BYTES - page_offset) {
            chunk = FILED_TMPFS_PAGE_BYTES - page_offset;
        }
        const uint16_t page_id = filed_tmpfs_inode_page_id(inode, page_index);
        filed_tmpfs_page_t *page = filed_tmpfs_page_by_id(backend, page_id);
        if (page != NULL) {
            memcpy((uint8_t *)buffer + total, page->data + page_offset, (size_t)chunk);
        } else {
            memset((uint8_t *)buffer + total, 0, (size_t)chunk);
        }
        total += chunk;
    }
    *out_bytes = total;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_pwrite(
    filed_tmpfs_backend_t *backend,
    uint64_t object_id,
    uint64_t offset,
    const void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    if (backend == NULL || buffer == NULL || out_bytes == NULL) {
        return -22;
    }
    *out_bytes = 0;
    if (offset > FILED_TMPFS_MAX_FILE_BYTES || length > FILED_TMPFS_MAX_FILE_BYTES - offset) {
        return -27;
    }
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_inode_t *inode = filed_tmpfs_find_inode(backend, object_id);
    if (inode == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (inode->kind == FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -21;
    }
    if (!filed_tmpfs_inode_is_data_file(inode)) {
        filed_tmpfs_lock_release(&backend->lock);
        return -22;
    }
    uint64_t total = 0;
    while (total < length) {
        const uint64_t absolute = offset + total;
        const uint64_t page_index = absolute / FILED_TMPFS_PAGE_BYTES;
        const uint64_t page_offset = absolute % FILED_TMPFS_PAGE_BYTES;
        uint64_t chunk = length - total;
        if (chunk > FILED_TMPFS_PAGE_BYTES - page_offset) {
            chunk = FILED_TMPFS_PAGE_BYTES - page_offset;
        }
        if (page_index >= FILED_TMPFS_MAX_FILE_PAGES) {
            filed_tmpfs_lock_release(&backend->lock);
            return -27;
        }
        uint16_t page_id = filed_tmpfs_inode_page_id(inode, page_index);
        if (page_id == 0) {
            const uint16_t allocated_page_id = filed_tmpfs_alloc_page(backend);
            if (allocated_page_id == 0) {
                filed_tmpfs_lock_release(&backend->lock);
                return -28;
            }
            if (!filed_tmpfs_note_inode_page(inode, page_index, allocated_page_id)) {
                filed_tmpfs_free_page(backend, allocated_page_id);
                filed_tmpfs_lock_release(&backend->lock);
                return -28;
            }
            page_id = allocated_page_id;
        }
        filed_tmpfs_page_t *page = filed_tmpfs_page_by_id(backend, page_id);
        if (page == NULL) {
            filed_tmpfs_lock_release(&backend->lock);
            return -5;
        }
        memcpy(page->data + page_offset, (const uint8_t *)buffer + total, (size_t)chunk);
        total += chunk;
    }
    if (offset + length > inode->size) {
        inode->size = offset + length;
    }
    ++inode->generation;
    *out_bytes = length;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_readlink(
    filed_tmpfs_backend_t *backend,
    uint64_t object_id,
    char *out_target,
    uint64_t target_capacity,
    uint64_t *out_length)
{
    if (backend == NULL || out_target == NULL || out_length == NULL || target_capacity == 0) {
        return -22;
    }
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_inode_t *inode = filed_tmpfs_find_inode(backend, object_id);
    if (inode == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (inode->kind != FILED_VNODE_SYMLINK) {
        filed_tmpfs_lock_release(&backend->lock);
        return -22;
    }
    filed_tmpfs_lock_release(&backend->lock);
    return filed_tmpfs_backend_pread(backend, object_id, 0, out_target, target_capacity, out_length);
}

int filed_tmpfs_backend_truncate(filed_tmpfs_backend_t *backend, uint64_t object_id, uint64_t size)
{
    if (backend == NULL || size > FILED_TMPFS_MAX_FILE_BYTES) {
        return backend == NULL ? -22 : -27;
    }
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_inode_t *inode = filed_tmpfs_find_inode(backend, object_id);
    if (inode == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (inode->kind == FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -21;
    }
    if (inode->kind != FILED_VNODE_REGULAR) {
        filed_tmpfs_lock_release(&backend->lock);
        return -22;
    }
    const uint64_t old_size = inode->size;
    const uint64_t keep_pages = size == 0 ? 0 : ((size - 1u) / FILED_TMPFS_PAGE_BYTES) + 1u;
    if (size < old_size) {
        if ((size % FILED_TMPFS_PAGE_BYTES) != 0 && keep_pages <= FILED_TMPFS_MAX_FILE_PAGES) {
            filed_tmpfs_page_t *last = filed_tmpfs_page_by_id(
                backend,
                filed_tmpfs_inode_page_id(inode, keep_pages - 1u));
            if (last != NULL) {
                memset(
                    last->data + (size % FILED_TMPFS_PAGE_BYTES),
                    0,
                    (size_t)(FILED_TMPFS_PAGE_BYTES - (size % FILED_TMPFS_PAGE_BYTES)));
            }
        }
        filed_tmpfs_free_inode_pages(backend, inode, keep_pages);
    }
    inode->size = size;
    ++inode->generation;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

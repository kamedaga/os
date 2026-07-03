#include "private.h"

uint16_t filed_tmpfs_alloc_page(filed_tmpfs_backend_t *backend)
{
    if (backend == NULL || backend->free_page_count == 0) {
        return 0;
    }
    const uint16_t page_id = backend->free_page_stack[--backend->free_page_count];
    if (page_id == 0 || page_id > FILED_TMPFS_PAGE_POOL_PAGES) {
        return 0;
    }
    filed_tmpfs_page_t *page = &backend->pages[(size_t)page_id - 1u];
    if (page->used) {
        return 0;
    }
    memset(page, 0, sizeof(*page));
    page->used = true;
    return page_id;
}

filed_tmpfs_page_t *filed_tmpfs_page_by_id(filed_tmpfs_backend_t *backend, uint16_t page_id)
{
    if (backend == NULL || page_id == 0 || page_id > FILED_TMPFS_PAGE_POOL_PAGES) {
        return NULL;
    }
    filed_tmpfs_page_t *page = &backend->pages[(size_t)page_id - 1u];
    return page->used ? page : NULL;
}

void filed_tmpfs_free_page(filed_tmpfs_backend_t *backend, uint16_t page_id)
{
    if (backend == NULL || page_id == 0 || page_id > FILED_TMPFS_PAGE_POOL_PAGES) {
        return;
    }
    const uint32_t index = (uint32_t)page_id - 1u;
    if (!backend->pages[index].used) {
        return;
    }
    memset(&backend->pages[index], 0, sizeof(backend->pages[0]));
    if (backend->free_page_count < FILED_TMPFS_PAGE_POOL_PAGES) {
        backend->free_page_stack[backend->free_page_count++] = page_id;
    }
}

int filed_tmpfs_note_inode_page(filed_tmpfs_inode_t *inode, uint64_t page_index)
{
    if (inode == NULL || page_index >= FILED_TMPFS_NODE_MAX_PAGES) {
        return 0;
    }
    if (inode->allocated_page_count >= FILED_TMPFS_NODE_MAX_PAGES) {
        return 0;
    }
    inode->allocated_page_indices[inode->allocated_page_count++] = (uint16_t)page_index;
    return 1;
}

void filed_tmpfs_free_inode_pages(filed_tmpfs_backend_t *backend, filed_tmpfs_inode_t *inode, uint64_t first_page)
{
    if (inode == NULL) {
        return;
    }
    if (first_page > FILED_TMPFS_NODE_MAX_PAGES) {
        first_page = FILED_TMPFS_NODE_MAX_PAGES;
    }
    uint16_t i = 0;
    while (i < inode->allocated_page_count) {
        const uint16_t page_index = inode->allocated_page_indices[i];
        if (page_index >= first_page && page_index < FILED_TMPFS_NODE_MAX_PAGES) {
            filed_tmpfs_free_page(backend, inode->pages[page_index]);
            inode->pages[page_index] = 0;
            inode->allocated_page_indices[i] = inode->allocated_page_indices[--inode->allocated_page_count];
        } else {
            ++i;
        }
    }
}

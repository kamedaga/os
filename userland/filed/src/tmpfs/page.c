#include "private.h"
#include <stdlib.h>

int filed_tmpfs_alloc_page(filed_tmpfs_backend_t *backend)
{
    if (backend == NULL) return -22;
    if (backend->free_page_count == 0) return -28;
    const uint32_t page_id = backend->free_page_stack[backend->free_page_count - 1];
    if (page_id == 0 || page_id > FILED_TMPFS_PAGE_POOL_PAGES) {
        return -5;
    }
    filed_tmpfs_page_t *page = &backend->pages[(size_t)page_id - 1u];
    if (page->used || page->data) return -5;
    uint8_t *data = calloc(1, FILED_TMPFS_PAGE_BYTES);
    if (!data) return -12;
    --backend->free_page_count;
    page->data = data;
    page->used = true;
    return page_id;
}

filed_tmpfs_page_t *filed_tmpfs_page_by_id(filed_tmpfs_backend_t *backend, uint32_t page_id)
{
    if (backend == NULL || page_id == 0 || page_id > FILED_TMPFS_PAGE_POOL_PAGES) {
        return NULL;
    }
    filed_tmpfs_page_t *page = &backend->pages[(size_t)page_id - 1u];
    return page->used ? page : NULL;
}

void filed_tmpfs_free_page(filed_tmpfs_backend_t *backend, uint32_t page_id)
{
    if (backend == NULL || page_id == 0 || page_id > FILED_TMPFS_PAGE_POOL_PAGES) {
        return;
    }
    const uint32_t index = (uint32_t)page_id - 1u;
    if (!backend->pages[index].used) {
        return;
    }
    free(backend->pages[index].data);
    memset(&backend->pages[index], 0, sizeof(backend->pages[0]));
    if (backend->free_page_count < FILED_TMPFS_PAGE_POOL_PAGES) {
        backend->free_page_stack[backend->free_page_count++] = page_id;
    }
}

uint32_t filed_tmpfs_inode_page_id(filed_tmpfs_backend_t *backend, filed_tmpfs_inode_t *inode, uint64_t page_index)
{
    if (inode == NULL || page_index >= FILED_TMPFS_MAX_FILE_PAGES ||
        inode->first_allocated_page == 0) {
        return 0;
    }
    const filed_tmpfs_page_t *last = filed_tmpfs_page_by_id(backend, inode->last_allocated_page);
    if (last == NULL || page_index > last->file_page_index) return 0;
    uint32_t id = inode->first_allocated_page;
    if (inode->lookup_page != 0) {
        const filed_tmpfs_page_t *cached = filed_tmpfs_page_by_id(backend, inode->lookup_page);
        if (cached != NULL && cached->file_page_index <= page_index) id = inode->lookup_page;
    }
    for (; id != 0;) {
        filed_tmpfs_page_t *page = filed_tmpfs_page_by_id(backend, id);
        if (!page) return 0;
        if (page->file_page_index > page_index) return 0;
        inode->lookup_page = id;
        if (page->file_page_index == page_index) return id;
        id = page->next_inode_page;
    }
    return 0;
}

int filed_tmpfs_note_inode_page(
    filed_tmpfs_backend_t *backend,
    filed_tmpfs_inode_t *inode,
    uint64_t page_index,
    uint32_t page_id)
{
    if (inode == NULL ||
        page_index >= FILED_TMPFS_MAX_FILE_PAGES ||
        page_id == 0 ||
        inode->allocated_page_count >= FILED_TMPFS_PAGE_POOL_PAGES)
    {
        return 0;
    }
    filed_tmpfs_page_t *page = filed_tmpfs_page_by_id(backend, page_id);
    if (!page) return 0;
    page->file_page_index = (uint32_t)page_index;
    // Ordered links make both sequential bootfs population and subsequent
    // sequential reads linear, including Mesa's large LLVM shared library.
    page->next_inode_page = 0;
    if (inode->first_allocated_page == 0) {
        inode->first_allocated_page = page_id;
        inode->last_allocated_page = page_id;
    } else {
        filed_tmpfs_page_t *last = filed_tmpfs_page_by_id(backend, inode->last_allocated_page);
        if (last == NULL) return 0;
        if (page_index > last->file_page_index) {
            last->next_inode_page = page_id;
            inode->last_allocated_page = page_id;
        } else {
            uint32_t *link = &inode->first_allocated_page;
            while (*link != 0) {
                filed_tmpfs_page_t *current = filed_tmpfs_page_by_id(backend, *link);
                if (current == NULL) return 0;
                if (current->file_page_index >= page_index) {
                    if (current->file_page_index == page_index) return 0;
                    break;
                }
                link = &current->next_inode_page;
            }
            page->next_inode_page = *link;
            *link = page_id;
        }
    }
    inode->lookup_page = 0;
    ++inode->allocated_page_count;
    return 1;
}

void filed_tmpfs_free_inode_pages(filed_tmpfs_backend_t *backend, filed_tmpfs_inode_t *inode, uint64_t first_page)
{
    if (inode == NULL) {
        return;
    }
    uint32_t *link = &inode->first_allocated_page;
    uint32_t last_kept = 0;
    while (*link) {
        const uint32_t id = *link;
        filed_tmpfs_page_t *page = filed_tmpfs_page_by_id(backend, id);
        if (!page) return;
        if (page->file_page_index >= first_page) {
            *link = page->next_inode_page;
            --inode->allocated_page_count;
            filed_tmpfs_free_page(backend, id);
        } else {
            last_kept = id;
            link = &page->next_inode_page;
        }
    }
    inode->last_allocated_page = last_kept;
    inode->lookup_page = 0;
}

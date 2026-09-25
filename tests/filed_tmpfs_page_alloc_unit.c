#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int fail_allocation, live_pages;
static void *test_calloc(size_t n, size_t size)
{
    if (fail_allocation) return NULL;
    void *p = calloc(n, size);
    if (p) ++live_pages;
    return p;
}
static void test_free(void *p)
{ if (p) --live_pages; free(p); }
#define calloc test_calloc
#define free test_free
#include "../userland/filed/src/tmpfs/page.c"
#undef calloc
#undef free

int main(void)
{
    static filed_tmpfs_backend_t backend;
    backend.free_page_count = 1;
    backend.free_page_stack[0] = 1;
    fail_allocation = 1;
    assert(filed_tmpfs_alloc_page(&backend) == -12);
    assert(backend.free_page_count == 1 && !backend.pages[0].used);
    assert(!backend.pages[0].data && !live_pages);
    fail_allocation = 0;
    assert(filed_tmpfs_alloc_page(&backend) == 1);
    assert(!backend.free_page_count && live_pages == 1);
    assert(filed_tmpfs_alloc_page(&backend) == -28);
    memset(backend.pages[0].data, 0xa5, FILED_TMPFS_PAGE_BYTES);
    filed_tmpfs_free_page(&backend, 1);
    filed_tmpfs_free_page(&backend, 1);
    assert(backend.free_page_count == 1 && !live_pages && !backend.pages[0].data);
    assert(filed_tmpfs_alloc_page(&backend) == 1);
    for (size_t i = 0; i < FILED_TMPFS_PAGE_BYTES; ++i)
        assert(backend.pages[0].data[i] == 0);
    filed_tmpfs_free_page(&backend, 1);
    assert(!live_pages);
    /* One inode can own the entire unchanged global budget. Ownership
     * metadata must not impose the former 4096-page per-file ceiling. */
    filed_tmpfs_inode_t inode = {0};
    backend.free_page_count = FILED_TMPFS_PAGE_POOL_PAGES;
    for (unsigned i = 0; i < FILED_TMPFS_PAGE_POOL_PAGES; ++i)
        backend.free_page_stack[i] = (uint16_t)(i + 1);
    for (unsigned i = 0; i < FILED_TMPFS_PAGE_POOL_PAGES; ++i) {
        int id = filed_tmpfs_alloc_page(&backend);
        assert(id > 0);
        assert(filed_tmpfs_note_inode_page(&backend, &inode, i * 2, (uint16_t)id));
    }
    assert(inode.allocated_page_count == FILED_TMPFS_PAGE_POOL_PAGES);
    assert(filed_tmpfs_alloc_page(&backend) == -28);
    assert(filed_tmpfs_inode_page_id(&backend, &inode, 8192));
    assert(!filed_tmpfs_inode_page_id(&backend, &inode, 8193));
    filed_tmpfs_free_inode_pages(&backend, &inode, 8192);
    assert(inode.allocated_page_count == 4096);
    assert(!filed_tmpfs_inode_page_id(&backend, &inode, 8192));
    assert(filed_tmpfs_inode_page_id(&backend, &inode, 8190));
    int reused = filed_tmpfs_alloc_page(&backend);
    assert(reused > 0);
    assert(filed_tmpfs_note_inode_page(&backend, &inode, 9000, (uint16_t)reused));
    filed_tmpfs_free_inode_pages(&backend, &inode, 0);
    assert(!inode.allocated_page_count && !inode.first_allocated_page);
    assert(backend.free_page_count == FILED_TMPFS_PAGE_POOL_PAGES && !live_pages);
    puts("tmpfs lazy allocation failure, reclaim and zeroed reuse PASS");
    puts("tmpfs full-pool single inode, quota, sparse lookup and partial reclaim PASS");
}

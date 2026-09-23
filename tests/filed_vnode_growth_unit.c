#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userland/filed/src/vfs/private.h"

static int fail_body, fail_directory;
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_body && count == FILED_VNODE_BANK_ENTRIES && size == sizeof(filed_vnode_t)) {
        fail_body = 0;
        return NULL;
    }
    return __real_calloc(count, size);
}
void *__wrap_realloc(void *p, size_t size)
{
    if (fail_directory) { fail_directory = 0; return NULL; }
    return __real_realloc(p, size);
}

static void growth_and_trim(void)
{
    enum { COUNT = 641 };
    filed_vfs_t vfs;
    filed_vfs_init(&vfs);
    filed_mount_id_t mount;
    filed_vfs_open_result_t root;
    assert(filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &mount) == FILED_OK);
    assert(filed_vfs_open_root(&vfs, mount, FILED_RIGHT_LOOKUP | FILED_RIGHT_REMOVE,
        FILED_OPEN_DIRECTORY, &root) == FILED_OK);
    filed_vnode_t *saved[COUNT];
    filed_vnode_id_t ids[COUNT];
    for (unsigned i = 0; i < COUNT; ++i) {
        char name[32];
        filed_vfs_open_result_t file;
        snprintf(name, sizeof(name), "cached-%u", i);
        assert(filed_vfs_open_backend_child(&vfs, root.handle_id, 1000 + i,
            FILED_VNODE_REGULAR, name, FILED_RIGHT_READ, 0, &file) == FILED_OK);
        ids[i] = file.vnode_id;
        saved[i] = filed_find_vnode(&vfs, ids[i]);
        assert(saved[i]);
        assert(filed_vfs_close_handle(&vfs, file.handle_id) == FILED_OK);
    }
    assert(vfs.vnode_capacity >= COUNT + 1);
    assert(filed_vfs_check_basic(&vfs) == FILED_OK);
    /* Free an interior bank. The last bank moves in the directory only. */
    const uint32_t before = vfs.vnode_capacity;
    for (unsigned i = 31; i < 63; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "cached-%u", i);
        assert(filed_vfs_unlink_commit(&vfs, root.handle_id, name) == FILED_OK);
    }
    filed_vfs_trim_vnodes(&vfs);
    assert(vfs.vnode_capacity == before - FILED_VNODE_BANK_ENTRIES);
    for (unsigned i = 0; i < COUNT; ++i) {
        if (i >= 31 && i < 63) {
            assert(filed_find_vnode(&vfs, ids[i]) == NULL);
            continue;
        }
        assert(filed_find_vnode(&vfs, ids[i]) == saved[i]);
        char name[32];
        filed_vfs_open_result_t file;
        snprintf(name, sizeof(name), "cached-%u", i);
        assert(filed_vfs_open_cached_child(&vfs, root.handle_id, name,
            FILED_RIGHT_READ, 0, &file) == FILED_OK);
        assert(file.vnode_id == ids[i] && file.backend_object == 1000 + i);
        assert(filed_vfs_close_handle(&vfs, file.handle_id) == FILED_OK);
    }
    assert(filed_vfs_check_basic(&vfs) == FILED_OK);
    filed_vfs_destroy(&vfs);
    assert(vfs.vnode_capacity == 0 && !vfs.vnode_banks);
}

static void allocation_failures_and_wide_hints(void)
{
    filed_vfs_t vfs;
    filed_vfs_init(&vfs);
    for (unsigned i = 0; i < FILED_VNODE_BANK_ENTRIES; ++i) {
        filed_vnode_t *vnode = filed_alloc_vnode(&vfs);
        assert(vnode);
        vnode->active = true;
        vnode->id = i + 1;
    }
    filed_vnode_t *first = filed_vfs_vnode_at(&vfs, 0);
    fail_body = 1;
    assert(!filed_alloc_vnode(&vfs));
    fail_directory = 1;
    assert(!filed_alloc_vnode(&vfs));
    assert(vfs.vnode_capacity == FILED_VNODE_BANK_ENTRIES);
    assert(filed_vfs_vnode_at(&vfs, 0) == first && first->id == 1);
    for (unsigned i = FILED_VNODE_BANK_ENTRIES; i < 65570; ++i) {
        filed_vnode_t *vnode = filed_alloc_vnode(&vfs);
        assert(vnode);
        vnode->active = true;
        vnode->id = i + 1;
    }
    filed_vnode_t *wide = filed_vfs_vnode_at(&vfs, 65536);
    filed_remember_vnode_slot(&vfs, wide);
    assert(vfs.vnode_slot_hints[filed_id_hint_index(wide->id)] == 65537);
    assert(filed_find_vnode(&vfs, wide->id) == wide);
    assert(filed_vfs_vnode_at(&vfs, 0) == first);
    /* Clear metadata only; these synthetic nodes have no resources/handles. */
    for (uint32_t i = 0; i < vfs.vnode_capacity; ++i)
        filed_vfs_vnode_at(&vfs, i)->active = false;
    filed_vfs_trim_vnodes(&vfs);
    assert(vfs.vnode_capacity == 0 && !vfs.vnode_banks);
    filed_vfs_destroy(&vfs);
}

int main(void)
{
    growth_and_trim();
    allocation_failures_and_wide_hints();
    puts("VNODE_GROWTH PASS: 641 nodes, stable addresses, interior trim, OOM, 32-bit hints");
    return 0;
}

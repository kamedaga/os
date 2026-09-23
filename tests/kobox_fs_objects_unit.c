#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include "../userland/koboxd/src/fs_backend.c"

static int fail_alloc, close_error, sync_error;
static unsigned allocations, iputs, dputs;
static uint8_t inode_bytes[128], dentry_bytes[64];
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t count, size_t size)
{
    ++allocations;
    return fail_alloc ? NULL : __real_calloc(count, size);
}
void kb_fs_subsystem_generic_fillattr(void *idmap, uint32_t mask, void *inode, void *stat)
{
    (void)idmap; (void)mask;
    assert(inode == inode_bytes);
    koboxd_linux_kstat_t *out = stat;
    out->ino = 123;
    out->mode = 0100644;
    out->nlink = 1;
}
unsigned long kb_module_kernel_gs_for_address(const void *address)
{ (void)address; return 0; }
int kb_shim_enter_kernel_gs(unsigned long gs, unsigned long *old)
{ (void)gs; (void)old; assert(0); return -1; }
void kb_shim_leave_kernel_gs(unsigned long old) { (void)old; assert(0); }
int kb_fs_subsystem_file_close(void *file) { assert(file); return close_error; }
void kb_fs_subsystem_dput(void *dentry) { assert(dentry == dentry_bytes); ++dputs; }
void kb_fs_subsystem_iput(void *inode) { assert(inode == inode_bytes); ++iputs; }
int kb_fs_subsystem_sync_filesystem(void *sb) { assert(sb); return sync_error; }
kb_status_t kb_module_find_symbol(kb_module_t *module, const char *name, void **out)
{
    assert(module);
    static uintptr_t operations[32];
    for (unsigned i = 0; i < 32; ++i) operations[i] = (uintptr_t)operations;
    (void)name;
    *out = operations; /* Required symbols exist; sync calls the native stub. */
    return KB_OK;
}

static int add(koboxd_fs_backend_t *backend, uint64_t *id)
{
    char name[32];
    snprintf(name, sizeof(name), "entry-%llu", (unsigned long long)backend->next_object_id);
    return fs_object_register(backend, 1, inode_bytes, dentry_bytes, name, 1, id);
}
static void clear(koboxd_fs_backend_t *backend)
{
    for (fs_object_iterator_t it = fs_objects(backend); it.object; fs_objects_next(&it))
        memset(it.object, 0, sizeof(*it.object));
    fs_objects_trim_empty(backend);
    assert(!backend->object_banks && !backend->object_capacity);
}

int main(void)
{
    koboxd_fs_backend_t backend = { .next_object_id = 2, .mounted = 1 };
    koboxd_fs_object_t *saved[320];
    uint64_t ids[320], id;
    for (unsigned i = 0; i < 320; ++i) {
        assert(add(&backend, &ids[i]) == 0);
        saved[i] = fs_object_by_id(&backend, ids[i]);
        assert(saved[i] && saved[i]->references == 1);
    }
    koboxd_fs_object_stats_t stats;
    koboxd_fs_backend_object_stats(&backend, &stats);
    assert(stats.used == 320 && stats.referenced == 320 && stats.capacity == 320);
    for (unsigned i = 0; i < 320; ++i) {
        assert(fs_object_by_id(&backend, ids[i]) == saved[i]);
        assert(fs_object_by_parent_name(&backend, 1, saved[i]->name) == saved[i]);
    }
    fail_alloc = 1;
    uint64_t next_id = backend.next_object_id;
    assert(add(&backend, &id) == -12);
    assert(backend.next_object_id == next_id && backend.object_capacity == 320);
    assert(fs_object_by_id(&backend, ids[319]) == saved[319]);
    /* Allocation failure may reclaim an idle entry without losing live IDs. */
    saved[12]->references = 0;
    saved[12]->native_read_file = (void *)1;
    close_error = -5;
    assert(add(&backend, &id) == -5 && fs_object_by_id(&backend, ids[12]) == saved[12]);
    close_error = 0;
    unsigned previous_allocations = allocations;
    assert(add(&backend, &id) == 0 && allocations == previous_allocations + 1);
    assert(id == next_id && fs_object_by_id(&backend, id) == saved[12]);
    assert(!fs_object_by_id(&backend, ids[12]));
    fail_alloc = 0;
    assert(add(&backend, &id) == 0 && backend.object_capacity == 352);
    /* Release the peak's entries, keeping one live pointer in the oldest
     * bank. Cache pruning must not reclaim that pointer/ID. */
    for (fs_object_iterator_t it = fs_objects(&backend); it.object; fs_objects_next(&it))
        if (it.object->used) it.object->references = it.object == saved[0];
    assert(fs_objects_trim_cache(&backend) == 0);
    koboxd_fs_backend_object_stats(&backend, &stats);
    assert(stats.cached == KOBOXD_FS_CACHED_OBJECTS && stats.referenced == 1);
    assert(fs_object_by_id(&backend, ids[0]) == saved[0]);
    assert(stats.capacity < 352);
    clear(&backend);

    /* Below the former cache budget, free banks grow rather than forcing
     * every library lookup through a tiny 32-entry cache. */
    backend.next_object_id = 2;
    for (unsigned i = 0; i < KOBOXD_FS_CACHED_OBJECTS; ++i) {
        assert(add(&backend, &id) == 0);
        fs_object_by_id(&backend, id)->references = 0;
    }
    koboxd_fs_backend_object_stats(&backend, &stats);
    assert(stats.used == KOBOXD_FS_CACHED_OBJECTS);
    clear(&backend);

    /* Live entries must not consume the idle-cache budget. Real browsing
     * otherwise evicts warm path lookups as the live descriptor set grows. */
    backend.next_object_id = 2;
    for (unsigned i = 0; i < 128; ++i) {
        assert(add(&backend, &ids[i]) == 0);
        saved[i] = fs_object_by_id(&backend, ids[i]);
    }
    uint64_t first_cached = backend.next_object_id;
    uint64_t previous_evictions = backend.object_evictions;
    for (unsigned i = 0; i < KOBOXD_FS_CACHED_OBJECTS; ++i) {
        assert(add(&backend, &id) == 0);
        fs_object_by_id(&backend, id)->references = 0;
    }
    koboxd_fs_backend_object_stats(&backend, &stats);
    assert(stats.referenced == 128 && stats.cached == KOBOXD_FS_CACHED_OBJECTS);
    assert(stats.used == 128 + KOBOXD_FS_CACHED_OBJECTS);
    assert(backend.object_evictions == previous_evictions);
    assert(fs_object_by_id(&backend, first_cached));
    fs_object_by_id(&backend, first_cached)->native_read_file = (void *)1;
    previous_allocations = allocations;
    next_id = backend.next_object_id;
    close_error = -5;
    assert(add(&backend, &id) == -5);
    assert(backend.next_object_id == next_id && allocations == previous_allocations);
    assert(backend.object_evictions == previous_evictions);
    assert(fs_object_by_id(&backend, first_cached));
    close_error = 0;
    assert(add(&backend, &id) == 0);
    assert(allocations == previous_allocations);
    assert(!fs_object_by_id(&backend, first_cached));
    assert(backend.object_evictions == previous_evictions + 1);
    for (unsigned i = 0; i < 128; ++i)
        assert(fs_object_by_id(&backend, ids[i]) == saved[i]);
    clear(&backend);

    /* More than 256 unlinked entries: existing prepared markers replace the
     * old stack batch. A repeated prepare must not drop inode refs twice. */
    backend.next_object_id = 2;
    backend.deferred_unlinked_count = 0;
    for (unsigned i = 0; i < 320; ++i) assert(add(&backend, &ids[i]) == 0);
    for (fs_object_iterator_t it = fs_objects(&backend); it.object; fs_objects_next(&it)) {
        if (!it.object->used) continue;
        it.object->linked = 0;
        it.object->references = 0;
        ++backend.deferred_unlinked_count;
    }
    koboxd_ext4_operations_t ops = {0};
    unsigned previous_iputs = iputs, previous_dputs = dputs;
    assert(fs_prepare_deferred_unlinked_objects(&backend, &ops) == 0);
    assert(iputs == previous_iputs + 320 && dputs == previous_dputs + 320);
    assert(fs_prepare_deferred_unlinked_objects(&backend, &ops) == 0);
    assert(iputs == previous_iputs + 320 && dputs == previous_dputs + 320);
    assert(backend.deferred_unlinked_count == 320);
    backend.ext4_module = (kb_module_t *)1;
    backend.mount_result.super_block = &backend;
    sync_error = -5;
    koboxd_fs_backend_lock(&backend);
    assert(koboxd_fs_backend_sync_all(&backend) == -5);
    assert(backend.deferred_unlinked_count == 320);
    assert(fs_object_by_id(&backend, ids[0])->release_prepared);
    assert(iputs == previous_iputs + 320 && dputs == previous_dputs + 320);
    sync_error = 0;
    assert(koboxd_fs_backend_sync_all(&backend) == 0);
    koboxd_fs_backend_unlock(&backend);
    assert(!backend.deferred_unlinked_count);
    koboxd_fs_backend_object_stats(&backend, &stats);
    assert(!stats.used);
    clear(&backend);
    puts("kobox filesystem objects: growth, stable pointers, OOM, cache budget and deferred release PASS");
}

#include <assert.h>
#include "../src/dispatch/common.h"

static filed_runtime_t runtime;
static int flushes, drops, releases;
int pacha_fd_close(int fd) { (void)fd; assert(0); return -1; }
int filed_cache_flush_object(filed_runtime_t *rt, uint64_t object)
{ (void)rt; (void)object; ++flushes; return -28; }
void filed_cache_release_object(filed_runtime_t *rt, uint64_t object)
{ (void)rt; assert(object == 100); ++drops; }
int filed_backend_release_object(filed_runtime_t *rt, uint64_t object)
{ (void)rt; assert(object == 100); ++releases; return 0; }
bool filed_cache_object_evictable(filed_runtime_t *rt, uint64_t object)
{ (void)rt; (void)object; return false; }
int main(void)
{
    filed_mount_id_t mount;
    filed_vfs_open_result_t root, file, other;
    filed_vfs_init(&runtime.vfs);
    assert(filed_vfs_mount_root(&runtime.vfs, FILED_FS_SYNTHETIC, 7, 11, &mount) == FILED_OK);
    assert(filed_vfs_open_root(&runtime.vfs, mount,
        FILED_RIGHT_LOOKUP | FILED_RIGHT_REMOVE, FILED_OPEN_DIRECTORY, &root) == FILED_OK);
    assert(filed_vfs_open_backend_child(&runtime.vfs, root.handle_id, 100,
        FILED_VNODE_REGULAR, "memfd", FILED_RIGHT_READ, 0, &file) == FILED_OK);
    assert(filed_vfs_open_cached_child(&runtime.vfs, root.handle_id,
        "memfd", FILED_RIGHT_READ, 0, &other) == FILED_OK);
    assert(filed_vfs_unlink_commit(&runtime.vfs, root.handle_id, "memfd") == FILED_OK);
    assert(filed_close_handle_runtime_deferred(&runtime, file.handle_id) == 0);
    assert(!flushes && !drops && !releases);
    assert(filed_close_handle_runtime_deferred(&runtime, other.handle_id) == 0);
    assert(!flushes && drops == 1 && releases == 1);
    assert(filed_vfs_check_basic(&runtime.vfs) == FILED_OK);
    puts("final unlinked close reclaims without writeback: PASS");
    filed_vfs_destroy(&runtime.vfs);
    return 0;
}

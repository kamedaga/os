#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "filed/vfs.h"

static filed_vfs_t vfs;
static filed_vfs_t unchanged;
static filed_handle_id_t root_handle;
struct policy { unsigned calls; uint64_t pinned; int revoke; int deny_all; };

static bool eligible(void *opaque, filed_backend_object_id_t object)
{
    struct policy *p = opaque;
    ++p->calls;
    return !p->deny_all && object != p->pinned && (!p->revoke || p->calls == 1);
}

static void populate(unsigned count)
{
    filed_mount_id_t mount;
    filed_vfs_open_result_t root;
    filed_vfs_destroy(&vfs);
    filed_vfs_init(&vfs);
    assert(filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &mount) == FILED_OK);
    assert(filed_vfs_open_root(&vfs, mount, FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT,
        FILED_OPEN_DIRECTORY, &root) == FILED_OK);
    root_handle = root.handle_id;
    for (unsigned i = 0; i < count; ++i) {
        char name[32];
        filed_vfs_open_result_t file;
        snprintf(name, sizeof(name), "cached-%u", i);
        assert(filed_vfs_open_backend_child(&vfs, root.handle_id, 1000 + i,
            FILED_VNODE_REGULAR, name, FILED_RIGHT_READ, 0, &file) == FILED_OK);
        assert(filed_vfs_close_handle(&vfs, file.handle_id) == FILED_OK);
    }
}

static filed_vfs_reclaim_result_t scan(unsigned limit, struct policy *p)
{
    filed_vfs_reclaim_result_t result;
    assert(filed_vfs_evict_lru_unused_linked(&vfs, limit, eligible, p, &result) == FILED_OK);
    assert(filed_vfs_check_basic(&vfs) == FILED_OK);
    return result;
}

int main(int argc, char **argv)
{
    const int baseline = argc == 2 && strcmp(argv[1], "--baseline") == 0;
    struct policy p = {0};
    struct timespec start, end;
    populate(48);
    unchanged = vfs;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    for (unsigned i = 0; i < 1000; ++i) assert(!scan(48, &p).released);
    assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    printf("VNODE_SCAN below_limit_calls=%u scans=1000 elapsed_ns=%lld\n", p.calls,
        (long long)(end.tv_sec - start.tv_sec) * 1000000000LL + end.tv_nsec - start.tv_nsec);
    if (!baseline) assert(p.calls == 0);
    assert(memcmp(&unchanged, &vfs, sizeof(vfs)) == 0);

    populate(8); p = (struct policy){0};
    filed_vfs_reclaim_result_t r = scan(4, &p);
    assert(r.released && r.backend_object == 1000);
    printf("VNODE_SCAN oldest_calls=%u\n", p.calls);
    if (!baseline) assert(p.calls == 2); /* Selection and locked revalidation. */

    populate(8); p = (struct policy){ .pinned = 1000 };
    r = scan(4, &p);
    assert(r.released && r.backend_object == 1001);
    printf("VNODE_SCAN pinned_oldest_calls=%u\n", p.calls);
    if (!baseline) assert(p.calls == 3);

    /* Do not drop the final under-lock revalidation when reducing probes. */
    populate(8); p = (struct policy){ .revoke = 1 };
    assert(!scan(4, &p).released);
    if (!baseline) assert(p.calls == 2);

    populate(8); p = (struct policy){ .deny_all = 1 };
    unchanged = vfs;
    assert(!scan(4, &p).released && p.calls == 8);
    assert(memcmp(&unchanged, &vfs, sizeof(vfs)) == 0);

    /* Multiple names for one object count once but remain ineligible. */
    populate(2); p = (struct policy){0};
    filed_vfs_open_result_t alias;
    assert(filed_vfs_open_backend_child(&vfs, root_handle, 1000,
        FILED_VNODE_REGULAR, "alias", FILED_RIGHT_READ, 0, &alias) == FILED_OK);
    assert(filed_vfs_close_handle(&vfs, alias.handle_id) == FILED_OK);
    r = scan(1, &p);
    assert(r.released && r.backend_object == 1001);
    assert(p.calls == 2);

    populate(0); p = (struct policy){0};
    assert(!scan(0, &p).released && p.calls == 0);
    puts("VNODE_SCAN PASS");
    filed_vfs_destroy(&vfs);
    return 0;
}

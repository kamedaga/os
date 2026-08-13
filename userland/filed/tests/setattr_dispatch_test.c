#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/dispatch/common.h"

static filed_runtime_t runtime;
static filed_handle_id_t setattr_handle;
static filed_handle_id_t inspect_handle;
static int failures;
static int statx_calls;
static int chmod_calls;
static int utimens_calls;
static int statx_status;
static int chmod_status;
static int utimens_status;
static bool chmod_observed_cache_valid;
static uint64_t chmod_observed_mode;
static filed_vfs_stat_snapshot_t utimens_observed_snapshot;

static void expect_true(const char *name, bool condition)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

static void expect_status(const char *name, int64_t actual, int64_t expected)
{
    if (actual != expected) {
        fprintf(
            stderr,
            "FAIL %s status=%lld expected=%lld\n",
            name,
            (long long)actual,
            (long long)expected);
        ++failures;
    }
}

static void expect_u64(const char *name, uint64_t actual, uint64_t expected)
{
    if (actual != expected) {
        fprintf(
            stderr,
            "FAIL %s value=%llu expected=%llu\n",
            name,
            (unsigned long long)actual,
            (unsigned long long)expected);
        ++failures;
    }
}

static void test_vmo_fill_window_plan(void)
{
    enum { WINDOW_BYTES = 2u * 1024u * 1024u };
    uint64_t map_offset = 0;
    uint64_t data_offset = 0;
    uint64_t map_length = 0;
    uint64_t chunk = 0;

    expect_status(
        "aligned VMO window plan",
        filed_vmo_fill_window_plan(
            0,
            3u * 1024u * 1024u,
            &map_offset,
            &data_offset,
            &map_length,
            &chunk),
        0);
    expect_u64("aligned map offset", map_offset, 0);
    expect_u64("aligned data offset", data_offset, 0);
    expect_u64("aligned map length", map_length, WINDOW_BYTES);
    expect_u64("aligned chunk", chunk, WINDOW_BYTES);

    expect_status(
        "unaligned VMO window plan",
        filed_vmo_fill_window_plan(
            4096u + 17u,
            3u * 1024u * 1024u,
            &map_offset,
            &data_offset,
            &map_length,
            &chunk),
        0);
    expect_u64("unaligned map offset", map_offset, 4096u);
    expect_u64("unaligned data offset", data_offset, 17u);
    expect_u64("unaligned map length", map_length, WINDOW_BYTES);
    expect_u64("unaligned chunk", chunk, WINDOW_BYTES - 17u);

    expect_status(
        "short VMO window plan",
        filed_vmo_fill_window_plan(
            4095u,
            4u,
            &map_offset,
            &data_offset,
            &map_length,
            &chunk),
        0);
    expect_u64("short map offset", map_offset, 0);
    expect_u64("short data offset", data_offset, 4095u);
    expect_u64("short map length", map_length, 4099u);
    expect_u64("short chunk", chunk, 4u);

    expect_status(
        "zero remaining rejected",
        filed_vmo_fill_window_plan(
            0,
            0,
            &map_offset,
            &data_offset,
            &map_length,
            &chunk),
        -22);
}

static filed_vfs_stat_snapshot_t inspect_snapshot(const char *name)
{
    filed_vfs_stat_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    const filed_status_t status = filed_vfs_get_stat_snapshot(
        &runtime.vfs,
        inspect_handle,
        &snapshot);
    if (status != FILED_OK || !snapshot.valid) {
        fprintf(stderr, "FAIL %s snapshot status=%d valid=%d\n", name, status, snapshot.valid);
        ++failures;
    }
    return snapshot;
}

filed_page_dispatch_result_t filed_page_result(int64_t status, uint64_t result)
{
    filed_page_dispatch_result_t out;
    memset(&out, 0, sizeof(out));
    out.status = status;
    out.result = result;
    out.process_fd = -1;
    out.thread_fd = -1;
    return out;
}

int64_t filed_status_to_wire(filed_status_t status)
{
    switch (status) {
    case FILED_OK:
        return 0;
    case FILED_ERR_NOT_FOUND:
        return -2;
    case FILED_ERR_DENIED:
        return -13;
    case FILED_ERR_INVALID:
        return -22;
    default:
        return -5;
    }
}

filed_vfs_stat_snapshot_t filed_stat_snapshot_from_backend(
    const storage_statx_reply_t *stat,
    uint64_t handle_id,
    uint64_t object_generation,
    uint64_t dir_generation)
{
    filed_vfs_stat_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (stat == NULL) {
        return snapshot;
    }
    snapshot.valid = true;
    snapshot.handle_id = handle_id;
    snapshot.mode = stat->mode;
    snapshot.size = stat->size;
    snapshot.blocks = stat->blocks;
    snapshot.nlink = stat->nlink;
    snapshot.kind = stat->kind;
    snapshot.rdev = stat->rdev;
    snapshot.times_valid = true;
    snapshot.atime_sec = stat->atime_sec;
    snapshot.atime_nsec = stat->atime_nsec;
    snapshot.mtime_sec = stat->mtime_sec;
    snapshot.mtime_nsec = stat->mtime_nsec;
    snapshot.ctime_sec = stat->ctime_sec;
    snapshot.ctime_nsec = stat->ctime_nsec;
    snapshot.object_generation = (filed_generation_t)object_generation;
    snapshot.dir_generation = (filed_generation_t)dir_generation;
    return snapshot;
}

int filed_backend_statx(
    filed_runtime_t *ignored_runtime,
    uint64_t object_id,
    storage_statx_reply_t *out_stat)
{
    (void)ignored_runtime;
    ++statx_calls;
    if (statx_status != 0) {
        return statx_status;
    }
    if (out_stat == NULL) {
        return -22;
    }
    memset(out_stat, 0, sizeof(*out_stat));
    out_stat->object_id = object_id;
    out_stat->mode = 0040755;
    out_stat->nlink = 1;
    out_stat->kind = 0040000;
    out_stat->atime_sec = 10;
    out_stat->atime_nsec = 20;
    out_stat->mtime_sec = 30;
    out_stat->mtime_nsec = 40;
    out_stat->ctime_sec = 50;
    out_stat->ctime_nsec = 60;
    return 0;
}

int filed_backend_chmod(
    filed_runtime_t *ignored_runtime,
    uint64_t object_id,
    uint64_t mode)
{
    (void)ignored_runtime;
    (void)object_id;
    (void)mode;
    ++chmod_calls;
    chmod_observed_cache_valid = false;
    expect_true(
        "query chmod cache state",
        filed_vfs_setattr_snapshot_valid(
            &runtime.vfs,
            setattr_handle,
            &chmod_observed_cache_valid) == FILED_OK);
    if (chmod_observed_cache_valid) {
        const filed_vfs_stat_snapshot_t snapshot =
            inspect_snapshot("chmod backend observes cache");
        chmod_observed_mode = snapshot.mode;
    } else {
        chmod_observed_mode = 0;
    }
    return chmod_status;
}

int filed_backend_utimens(
    filed_runtime_t *ignored_runtime,
    uint64_t object_id,
    uint32_t mask,
    int64_t atime_sec,
    int64_t atime_nsec,
    int64_t mtime_sec,
    int64_t mtime_nsec)
{
    (void)ignored_runtime;
    (void)object_id;
    (void)mask;
    (void)atime_sec;
    (void)atime_nsec;
    (void)mtime_sec;
    (void)mtime_nsec;
    ++utimens_calls;
    utimens_observed_snapshot = inspect_snapshot("utimens backend observes cache");
    return utimens_status;
}

static filed_page_dispatch_result_t dispatch_chmod(uint64_t handle, uint64_t mode)
{
    filed_chmod_t request;
    memset(&request, 0, sizeof(request));
    request.handle = handle;
    request.mode = mode;
    return filed_dispatch_chmod_page(&runtime, &request);
}

static filed_page_dispatch_result_t dispatch_utimens(
    uint64_t handle,
    uint64_t mask,
    int64_t atime_sec,
    int64_t atime_nsec,
    int64_t mtime_sec,
    int64_t mtime_nsec)
{
    filed_utimens_t request;
    memset(&request, 0, sizeof(request));
    request.handle = handle;
    request.mask = mask;
    request.atime_sec = atime_sec;
    request.atime_nsec = atime_nsec;
    request.mtime_sec = mtime_sec;
    request.mtime_nsec = mtime_nsec;
    return filed_dispatch_utimens_page(&runtime, &request);
}

int main(void)
{
    filed_mount_id_t mount_id = 0;
    filed_vfs_open_result_t setattr_root;
    filed_vfs_open_result_t stat_root;
    filed_vfs_open_result_t lookup_root;
    filed_vfs_open_result_t write_file;
    filed_page_dispatch_result_t result;
    filed_vfs_stat_snapshot_t snapshot;

    test_vmo_fill_window_plan();

    memset(&runtime, 0, sizeof(runtime));
    filed_vfs_init(&runtime.vfs);
    expect_true(
        "mount root",
        filed_vfs_mount_root(
            &runtime.vfs,
            FILED_FS_SYNTHETIC,
            7,
            11,
            &mount_id) == FILED_OK);
    expect_true(
        "open SETATTR-only directory",
        filed_vfs_open_root(
            &runtime.vfs,
            mount_id,
            FILED_RIGHT_SETATTR,
            FILED_OPEN_DIRECTORY,
            &setattr_root) == FILED_OK);
    expect_true(
        "open STAT inspection directory",
        filed_vfs_open_root(
            &runtime.vfs,
            mount_id,
            FILED_RIGHT_STAT,
            FILED_OPEN_DIRECTORY,
            &stat_root) == FILED_OK);
    setattr_handle = setattr_root.handle_id;
    inspect_handle = stat_root.handle_id;
    expect_true(
        "open LOOKUP directory",
        filed_vfs_open_root(
            &runtime.vfs,
            mount_id,
            FILED_RIGHT_LOOKUP,
            FILED_OPEN_DIRECTORY,
            &lookup_root) == FILED_OK);
    expect_true(
        "open WRITE-only regular file",
        filed_vfs_open_backend_child(
            &runtime.vfs,
            lookup_root.handle_id,
            42,
            FILED_VNODE_REGULAR,
            "write-only",
            FILED_RIGHT_WRITE,
            0,
            &write_file) == FILED_OK);

    result = dispatch_utimens(0xffffffffu, 0, 0, -1, 0, 1000000000ll);
    expect_status("zero mask is unconditional no-op", result.status, 0);
    expect_true("zero mask avoids all backends", statx_calls == 0 && utimens_calls == 0);

    result = dispatch_chmod(stat_root.handle_id, 0600);
    expect_status("STAT-only chmod denied", result.status, -13);
    result = dispatch_chmod(write_file.handle_id, 0600);
    expect_status("WRITE-only chmod denied", result.status, -13);
    result = dispatch_utimens(write_file.handle_id, FILED_UTIMENS_ATIME, 1, 0, 0, 0);
    expect_status("WRITE-only utimens denied", result.status, -13);
    expect_true(
        "unauthorized mutations avoid backend",
        statx_calls == 0 && chmod_calls == 0 && utimens_calls == 0);

    result = dispatch_chmod(setattr_root.handle_id, 010000);
    expect_status("invalid chmod mode", result.status, -22);
    expect_true("invalid mode avoids backend", statx_calls == 0 && chmod_calls == 0);

    result = dispatch_utimens(setattr_root.handle_id, FILED_UTIMENS_ATIME, 1, -1, 0, 0);
    expect_status("selected atime nsec below range", result.status, -22);
    result = dispatch_utimens(
        setattr_root.handle_id,
        FILED_UTIMENS_ATIME,
        1,
        1000000000ll,
        0,
        0);
    expect_status("selected atime nsec above range", result.status, -22);
    result = dispatch_utimens(setattr_root.handle_id, FILED_UTIMENS_MTIME, 0, 0, 1, -1);
    expect_status("selected mtime nsec below range", result.status, -22);
    result = dispatch_utimens(
        setattr_root.handle_id,
        FILED_UTIMENS_MTIME,
        0,
        0,
        1,
        1000000000ll);
    expect_status("selected mtime nsec above range", result.status, -22);
    expect_true("invalid selected nsec avoids backend", utimens_calls == 0);

    chmod_status = -5;
    result = dispatch_chmod(setattr_root.handle_id, 0700);
    expect_status("uncached chmod backend failure propagated", result.status, -5);
    bool snapshot_valid = true;
    expect_true(
        "query cache after uncached chmod failure",
        filed_vfs_setattr_snapshot_valid(
            &runtime.vfs,
            setattr_root.handle_id,
            &snapshot_valid) == FILED_OK);
    expect_true("uncached chmod failure leaves cache invalid", !snapshot_valid);
    chmod_status = 0;

    result = dispatch_chmod(setattr_root.handle_id, 0700);
    expect_status("SETATTR-only directory chmod", result.status, 0);
    expect_true("uncached chmod fetches stat for each attempt", statx_calls == 2);
    expect_true("valid and failed chmod call backend", chmod_calls == 2);
    expect_true("chmod backend runs before cache population", !chmod_observed_cache_valid);
    snapshot = inspect_snapshot("chmod result");
    expect_true("chmod updates cached permission bits", snapshot.mode == 0040700);

    result = dispatch_utimens(
        setattr_root.handle_id,
        FILED_UTIMENS_ATIME,
        100,
        0,
        999,
        -1);
    expect_status("atime lower boundary ignores unselected garbage", result.status, 0);
    expect_true("one-sided atime calls backend", utimens_calls == 1);
    expect_true(
        "atime backend runs before cache update",
        utimens_observed_snapshot.atime_sec == 10 &&
            utimens_observed_snapshot.mtime_sec == 30);
    snapshot = inspect_snapshot("atime result");
    expect_true(
        "one-sided atime preserves mtime",
        snapshot.atime_sec == 100 && snapshot.atime_nsec == 0 &&
            snapshot.mtime_sec == 30 && snapshot.mtime_nsec == 40);

    result = dispatch_utimens(
        setattr_root.handle_id,
        FILED_UTIMENS_MTIME,
        999,
        1000000000ll,
        200,
        999999999ll);
    expect_status("mtime upper boundary ignores unselected garbage", result.status, 0);
    expect_true("one-sided mtime calls backend", utimens_calls == 2);
    snapshot = inspect_snapshot("mtime result");
    expect_true(
        "one-sided mtime preserves atime",
        snapshot.atime_sec == 100 && snapshot.atime_nsec == 0 &&
            snapshot.mtime_sec == 200 && snapshot.mtime_nsec == 999999999ll);

    chmod_status = -5;
    result = dispatch_chmod(setattr_root.handle_id, 0777);
    expect_status("chmod backend failure propagated", result.status, -5);
    snapshot = inspect_snapshot("failed chmod result");
    expect_true("failed chmod leaves cache unchanged", snapshot.mode == 0040700);
    expect_true("cached chmod failure avoids stat backend", statx_calls == 2);
    chmod_status = 0;

    utimens_status = -5;
    result = dispatch_utimens(
        setattr_root.handle_id,
        FILED_UTIMENS_ATIME,
        300,
        1,
        0,
        -1);
    expect_status("utimens backend failure propagated", result.status, -5);
    snapshot = inspect_snapshot("failed utimens result");
    expect_true(
        "failed utimens leaves cache unchanged",
        snapshot.atime_sec == 100 && snapshot.atime_nsec == 0 &&
            snapshot.mtime_sec == 200 && snapshot.mtime_nsec == 999999999ll);

    expect_true("setattr dispatch preserves VFS invariant", filed_vfs_check_basic(&runtime.vfs) == FILED_OK);
    if (failures != 0) {
        fprintf(stderr, "setattr dispatch tests failed: %d\n", failures);
        return 1;
    }
    puts("setattr dispatch tests passed");
    return 0;
}

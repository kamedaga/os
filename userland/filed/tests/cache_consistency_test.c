#include <stdio.h>
#include <string.h>

#include "filed/cache.h"
#include "filed/tmpfs_internal.h"
#include "../src/cache/internal.h"
#include "../src/internal/dispatch_state.h"

static int failures;

int filed_kobox_backend_lookup(filed_kobox_backend_t *backend, uint64_t parent_object_id, const char *name, uint64_t *out_object_id)
{
    (void)backend;
    (void)parent_object_id;
    (void)name;
    (void)out_object_id;
    return -95;
}

int filed_kobox_backend_statx(filed_kobox_backend_t *backend, uint64_t object_id, storage_v2_statx_reply_t *out_stat)
{
    (void)backend;
    (void)object_id;
    (void)out_stat;
    return -95;
}

int filed_kobox_backend_pread(filed_kobox_backend_t *backend, uint64_t object_id, uint64_t offset, void *buffer, uint64_t length, uint64_t *out_bytes)
{
    (void)backend;
    (void)object_id;
    (void)offset;
    (void)buffer;
    (void)length;
    (void)out_bytes;
    return -95;
}

int filed_kobox_backend_pwrite(filed_kobox_backend_t *backend, uint64_t object_id, uint64_t offset, const void *buffer, uint64_t length, uint64_t *out_bytes)
{
    (void)backend;
    (void)object_id;
    (void)offset;
    (void)buffer;
    (void)length;
    (void)out_bytes;
    return -95;
}

int filed_kobox_backend_fsync(filed_kobox_backend_t *backend, uint64_t object_id)
{
    (void)backend;
    (void)object_id;
    return -95;
}

int filed_kobox_backend_create(filed_kobox_backend_t *backend, uint64_t parent_object_id, const char *name, uint64_t mode, uint64_t *out_object_id)
{
    (void)backend;
    (void)parent_object_id;
    (void)name;
    (void)mode;
    (void)out_object_id;
    return -95;
}

int filed_kobox_backend_truncate(filed_kobox_backend_t *backend, uint64_t object_id, uint64_t size)
{
    (void)backend;
    (void)object_id;
    (void)size;
    return -95;
}

int filed_kobox_backend_utimens(filed_kobox_backend_t *backend, uint64_t object_id, uint32_t mask, int64_t atime_sec, int64_t atime_nsec, int64_t mtime_sec, int64_t mtime_nsec)
{
    (void)backend;
    (void)object_id;
    (void)mask;
    (void)atime_sec;
    (void)atime_nsec;
    (void)mtime_sec;
    (void)mtime_nsec;
    return -95;
}

int filed_kobox_backend_chmod(filed_kobox_backend_t *backend, uint64_t object_id, uint64_t mode)
{
    (void)backend;
    (void)object_id;
    (void)mode;
    return -95;
}

int filed_kobox_backend_unlink(filed_kobox_backend_t *backend, uint64_t parent_object_id, const char *name)
{
    (void)backend;
    (void)parent_object_id;
    (void)name;
    return -95;
}

int filed_kobox_backend_mkdir(filed_kobox_backend_t *backend, uint64_t parent_object_id, const char *name, uint64_t mode, uint64_t *out_object_id)
{
    (void)backend;
    (void)parent_object_id;
    (void)name;
    (void)mode;
    (void)out_object_id;
    return -95;
}

int filed_kobox_backend_rmdir(filed_kobox_backend_t *backend, uint64_t parent_object_id, const char *name)
{
    (void)backend;
    (void)parent_object_id;
    (void)name;
    return -95;
}

int filed_kobox_backend_rename(filed_kobox_backend_t *backend, uint64_t old_parent_object_id, const char *old_name, uint64_t new_parent_object_id, const char *new_name, uint64_t *out_object_id)
{
    (void)backend;
    (void)old_parent_object_id;
    (void)old_name;
    (void)new_parent_object_id;
    (void)new_name;
    (void)out_object_id;
    return -95;
}

int filed_kobox_backend_release_object(filed_kobox_backend_t *backend, uint64_t object_id)
{
    (void)backend;
    (void)object_id;
    return -95;
}

int filed_kobox_backend_getdents(filed_kobox_backend_t *backend, uint64_t dir_object_id, uint64_t offset, storage_v2_getdents_request_t *out_entries)
{
    (void)backend;
    (void)dir_object_id;
    (void)offset;
    (void)out_entries;
    return -95;
}

static void expect_int(const char *name, int got, int expected)
{
    if (got != expected) {
        fprintf(stderr, "%s: got %d expected %d\n", name, got, expected);
        ++failures;
    }
}

static void expect_u64(const char *name, uint64_t got, uint64_t expected)
{
    if (got != expected) {
        fprintf(stderr,
            "%s: got %llu expected %llu\n",
            name,
            (unsigned long long)got,
            (unsigned long long)expected);
        ++failures;
    }
}

static void expect_bytes(const char *name, const void *got, const void *expected, size_t len)
{
    if (memcmp(got, expected, len) != 0) {
        fprintf(stderr, "%s: bytes differ\n", name);
        ++failures;
    }
}

static void expect_true(const char *name, int value)
{
    if (!value) {
        fprintf(stderr, "%s: expected true\n", name);
        ++failures;
    }
}

static void init_runtime(filed_runtime_t *runtime, filed_dispatch_state_t *dispatch)
{
    memset(runtime, 0, sizeof(*runtime));
    memset(dispatch, 0, sizeof(*dispatch));
    runtime->dispatch_state = dispatch;
    filed_tmpfs_backend_init(&runtime->tmpfs);
    filed_cache_configure(runtime, FILED_PAGE_CACHE_SLOTS);
}

static int dirents_contain(const storage_v2_getdents_request_t *entries, const char *name)
{
    if (entries == NULL || name == NULL) {
        return 0;
    }
    for (uint64_t i = 0; i < entries->count; ++i) {
        if (strncmp(entries->entries[i].name, name, sizeof(entries->entries[i].name)) == 0) {
            return 1;
        }
    }
    return 0;
}

static void test_rename_clears_negative_lookup(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    uint64_t root = 0;
    uint64_t file = 0;
    uint64_t renamed = 0;
    int64_t cached_status = 0;

    init_runtime(&runtime, &dispatch);
    expect_int("mount root rename", filed_tmpfs_backend_mount_root(&runtime.tmpfs, &root), 0);
    expect_int("create rename source", filed_tmpfs_backend_create(&runtime.tmpfs, root, "old", 0644, &file), 0);

    filed_negative_lookup_cache_store(&runtime, root, 1, "new", -2);
    expect_true(
        "negative lookup cached before rename",
        filed_negative_lookup_cache_get(&runtime, root, 1, "new", &cached_status));
    expect_int("negative status before rename", (int)cached_status, -2);

    expect_int("backend rename", filed_tmpfs_backend_rename(&runtime.tmpfs, root, "old", root, "new", &renamed), 0);
    filed_cache_invalidate(&runtime, root);

    expect_true(
        "negative lookup invalidated after rename",
        !filed_negative_lookup_cache_get(&runtime, root, 1, "new", &cached_status));
    expect_int("lookup renamed", filed_tmpfs_backend_lookup(&runtime.tmpfs, root, "new", &renamed), 0);
    expect_u64("renamed object", renamed, file);
}

static void test_unlink_clears_dirent_cache(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    uint64_t root = 0;
    uint64_t file = 0;
    storage_v2_getdents_request_t entries;
    storage_v2_getdents_request_t cached;

    init_runtime(&runtime, &dispatch);
    expect_int("mount root unlink", filed_tmpfs_backend_mount_root(&runtime.tmpfs, &root), 0);
    expect_int("create unlink target", filed_tmpfs_backend_create(&runtime.tmpfs, root, "gone", 0644, &file), 0);

    memset(&entries, 0, sizeof(entries));
    entries.capacity = FILED_V2_DIRENT_CAPACITY;
    expect_int("getdents before unlink", filed_tmpfs_backend_getdents(&runtime.tmpfs, root, 0, &entries), 0);
    expect_true("dirents contain before unlink", dirents_contain(&entries, "gone"));
    filed_dir_cache_store(&runtime, root, 0, &entries);
    memset(&cached, 0, sizeof(cached));
    expect_true("dir cache hit before unlink", filed_dir_cache_get(&runtime, root, 0, &cached));

    expect_int("backend unlink", filed_tmpfs_backend_unlink(&runtime.tmpfs, root, "gone"), 0);
    filed_cache_invalidate(&runtime, root);
    filed_cache_invalidate(&runtime, file);

    memset(&cached, 0, sizeof(cached));
    expect_true("dir cache invalidated after unlink", !filed_dir_cache_get(&runtime, root, 0, &cached));
    memset(&entries, 0, sizeof(entries));
    entries.capacity = FILED_V2_DIRENT_CAPACITY;
    expect_int("getdents after unlink", filed_tmpfs_backend_getdents(&runtime.tmpfs, root, 0, &entries), 0);
    expect_true("dirents omit after unlink", !dirents_contain(&entries, "gone"));
}

static void test_truncate_clears_page_and_vmo_cache(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    uint64_t root = 0;
    uint64_t file = 0;
    uint64_t bytes = 0;
    unsigned char buf[32];
    const unsigned char full[] = "abcdefghijklmnop";
    const unsigned char prefix[] = "abcd";
    filed_file_vmo_cache_entry_t *entry = NULL;

    init_runtime(&runtime, &dispatch);
    expect_int("mount root truncate", filed_tmpfs_backend_mount_root(&runtime.tmpfs, &root), 0);
    expect_int("create truncate target", filed_tmpfs_backend_create(&runtime.tmpfs, root, "file", 0644, &file), 0);
    expect_int("write truncate target", filed_tmpfs_backend_pwrite(&runtime.tmpfs, file, 0, full, sizeof(full) - 1u, &bytes), 0);

    memset(buf, 0, sizeof(buf));
    expect_int("cached pread before truncate", filed_cached_pread(&runtime, file, 0, buf, sizeof(full) - 1u, &bytes), 0);
    expect_u64("cached pread full bytes", bytes, sizeof(full) - 1u);
    expect_bytes("cached pread full data", buf, full, sizeof(full) - 1u);

    entry = filed_file_vmo_cache_slot(&runtime);
    memset(entry, 0, sizeof(*entry));
    entry->active = 1;
    entry->vmo_fd = -1;
    entry->backend_object = file;
    entry->object_generation = 1;
    entry->file_offset = 0;
    entry->length = 4;
    filed_cache_note_attachment(&runtime, file, FILED_CACHE_ATTACHMENT_VMO);
    expect_true("vmo cache hit before truncate", filed_file_vmo_cache_lookup(&runtime, file, 1, 0, 4) != NULL);

    expect_int("backend truncate", filed_tmpfs_backend_truncate(&runtime.tmpfs, file, 4), 0);
    filed_cache_invalidate(&runtime, file);

    expect_true("vmo cache invalidated after truncate", filed_file_vmo_cache_lookup(&runtime, file, 1, 0, 4) == NULL);
    memset(buf, 0, sizeof(buf));
    expect_int("cached pread after truncate", filed_cached_pread(&runtime, file, 0, buf, sizeof(buf), &bytes), 0);
    expect_u64("cached pread truncated bytes", bytes, sizeof(prefix) - 1u);
    expect_bytes("cached pread truncated data", buf, prefix, sizeof(prefix) - 1u);
}

int main(void)
{
    test_rename_clears_negative_lookup();
    test_unlink_clears_dirent_cache();
    test_truncate_clears_page_and_vmo_cache();
    if (failures != 0) {
        return 1;
    }
    printf("filed cache consistency tests passed\n");
    return 0;
}

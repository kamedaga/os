#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "filed/cache.h"
#include "filed/tmpfs_internal.h"
#include "../src/cache/internal.h"
#include "../src/internal/dispatch_state.h"

static int failures;
static unsigned char mock_backend_data[64];
static uint64_t mock_backend_size;
static int mock_vmo_revoke_calls;
static int mock_fd_close_calls;

int pacha_vmo_create(uint64_t size, uint64_t rights, uint32_t flags)
{
    (void)size;
    (void)rights;
    (void)flags;
    return -1;
}

int pacha_vmo_revoke(int fd)
{
    (void)fd;
    ++mock_vmo_revoke_calls;
    return 0;
}

void *pacha_mmap(int fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset)
{
    (void)fd;
    (void)size;
    (void)prot;
    (void)flags;
    (void)offset;
    return NULL;
}

int pacha_munmap(void *addr, uint64_t size)
{
    (void)addr;
    (void)size;
    return 0;
}

int pacha_fd_close(int fd)
{
    (void)fd;
    ++mock_fd_close_calls;
    return 0;
}

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
    if (object_id == 42 && buffer != NULL && out_bytes != NULL) {
        *out_bytes = 0;
        if (offset >= mock_backend_size) {
            return 0;
        }
        uint64_t available = mock_backend_size - offset;
        if (length > available) {
            length = available;
        }
        memcpy(buffer, mock_backend_data + offset, (size_t)length);
        *out_bytes = length;
        return 0;
    }
    return -95;
}

int filed_kobox_backend_pwrite(filed_kobox_backend_t *backend, uint64_t object_id, uint64_t offset, const void *buffer, uint64_t length, uint64_t *out_bytes)
{
    (void)backend;
    if (object_id == 42 && buffer != NULL && out_bytes != NULL &&
        offset <= sizeof(mock_backend_data) && length <= sizeof(mock_backend_data) - offset)
    {
        memcpy(mock_backend_data + offset, buffer, (size_t)length);
        if (offset + length > mock_backend_size) {
            mock_backend_size = offset + length;
        }
        *out_bytes = length;
        return 0;
    }
    return -95;
}

int filed_kobox_backend_readlink(filed_kobox_backend_t *backend, uint64_t object_id, char *out_target, uint64_t target_capacity, uint64_t *out_length)
{
    (void)backend;
    (void)object_id;
    (void)out_target;
    (void)target_capacity;
    (void)out_length;
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

static void test_shared_vmo_is_io_source_and_revoke_target(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    unsigned char shared_bytes[16] = "original";
    unsigned char readback[16];
    const unsigned char replacement[] = "mapped!!";
    uint64_t bytes = 0;

    init_runtime(&runtime, &dispatch);
    memset(mock_backend_data, 0, sizeof(mock_backend_data));
    mock_backend_size = 0;
    mock_vmo_revoke_calls = 0;
    mock_fd_close_calls = 0;

    filed_file_vmo_cache_entry_t *entry = filed_file_vmo_cache_slot(&runtime);
    expect_true("shared vmo slot", entry != NULL);
    if (entry == NULL) {
        return;
    }
    memset(entry, 0, sizeof(*entry));
    entry->active = 1;
    entry->shared = 1;
    entry->writable_lent = 1;
    entry->vmo_fd = 33;
    entry->backend_object = 42;
    entry->length = sizeof(shared_bytes);
    entry->logical_size = 8;
    entry->mapped = shared_bytes;

    memcpy(shared_bytes, replacement, sizeof(replacement) - 1u);
    memset(readback, 0, sizeof(readback));
    expect_int(
        "shared vmo cached pread",
        filed_cached_pread(&runtime, 42, 0, readback, sizeof(replacement) - 1u, &bytes),
        0);
    expect_u64("shared vmo cached pread bytes", bytes, sizeof(replacement) - 1u);
    expect_bytes("shared vmo cached pread data", readback, replacement, sizeof(replacement) - 1u);

    expect_int("shared vmo flush", filed_cache_flush_object(&runtime, 42), 0);
    expect_u64("shared vmo backend size", mock_backend_size, 8);
    expect_bytes("shared vmo backend data", mock_backend_data, replacement, sizeof(replacement) - 1u);

    filed_cache_invalidate(&runtime, 42);
    expect_int("shared vmo revoke count", mock_vmo_revoke_calls, 1);
    expect_true("shared vmo invalidated", filed_file_vmo_cache_shared_lookup(&runtime, 42) == NULL);

    entry = filed_file_vmo_cache_slot(&runtime);
    expect_true("shared vmo release slot", entry != NULL);
    if (entry == NULL) {
        return;
    }
    memset(entry, 0, sizeof(*entry));
    entry->active = 1;
    entry->shared = 1;
    entry->vmo_fd = 34;
    entry->backend_object = 42;
    entry->length = sizeof(shared_bytes);
    entry->mapped = shared_bytes;
    filed_cache_release_object(&runtime, 42);
    expect_int("shared vmo release does not revoke", mock_vmo_revoke_calls, 1);
    expect_int("shared vmo release closes owner", mock_fd_close_calls, 1);
}

static void test_file_vmo_cache_byte_budget(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    init_runtime(&runtime, &dispatch);

    expect_true(
        "vmo cache rejects over-budget entry",
        filed_file_vmo_cache_slot_for_length(
            &runtime,
            FILED_FILE_VMO_CACHE_TOTAL_BYTES + 1u) == NULL);

    filed_file_vmo_cache_entry_t *oldest = filed_file_vmo_cache_slot(&runtime);
    expect_true("vmo cache budget oldest slot", oldest != NULL);
    if (oldest == NULL) {
        return;
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->active = 1;
    oldest->vmo_fd = -1;
    oldest->length = 300u * 1024u * 1024u;
    oldest->clock = 1;

    filed_file_vmo_cache_entry_t *newer = filed_file_vmo_cache_slot(&runtime);
    expect_true("vmo cache budget newer slot", newer != NULL);
    if (newer == NULL) {
        return;
    }
    memset(newer, 0, sizeof(*newer));
    newer->active = 1;
    newer->vmo_fd = -1;
    newer->length = 150u * 1024u * 1024u;
    newer->clock = 2;

    filed_file_vmo_cache_entry_t *slot = filed_file_vmo_cache_slot_for_length(
        &runtime,
        100u * 1024u * 1024u);
    expect_true("vmo cache budget admits after eviction", slot != NULL);
    expect_true("vmo cache budget evicts oldest", !oldest->active);
    expect_true("vmo cache budget preserves newer", newer->active);
}

int main(void)
{
    test_rename_clears_negative_lookup();
    test_unlink_clears_dirent_cache();
    test_truncate_clears_page_and_vmo_cache();
    test_shared_vmo_is_io_source_and_revoke_target();
    test_file_vmo_cache_byte_budget();
    if (failures != 0) {
        return 1;
    }
    printf("filed cache consistency tests passed\n");
    return 0;
}

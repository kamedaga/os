#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "filed/cache.h"
#include "filed/tmpfs_internal.h"
#include "../src/cache/internal.h"
#include "../src/dispatch/common.h"
#include "../src/internal/dispatch_state.h"

static int failures;
static unsigned char mock_backend_data[64];
static uint64_t mock_backend_size;
static unsigned char stream_backend_data[5u * 1024u * 1024u];
static uint64_t stream_backend_size;
static int stream_backend_write_error;
static uint64_t stream_backend_read_error_at = UINT64_MAX;
static int mock_vmo_revoke_calls;
static int mock_fd_close_calls;
static int mock_fd_close_error;
static int mock_munmap_error;
static int mock_grow_error = -3;
static int mock_grow_calls;
static uint64_t mock_grow_capacity;
static void *mock_mmap_result;
static int mock_create_result = -1;
static uint64_t mock_create_rights;
static uint32_t mock_create_flags;
static int stream_backend_truncate_error;
static int mock_info_error = -1;
static struct pacha_fd_info mock_info;
static int fail_next_bank_allocation;
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_next_bank_allocation && count == 1 && size == sizeof(filed_file_vmo_bank_t)) {
        fail_next_bank_allocation = 0;
        return NULL;
    }
    return __real_calloc(count, size);
}
static int mock_link_calls;
static const unsigned char *mock_vmo_read_data;
static uint64_t mock_vmo_read_size;
static uint64_t mock_vmo_read_offset;
static unsigned mock_vmo_read_calls;
static long mock_vmo_dup_result = 90;
static long mock_vmo_read_error;
long pacha_fd_fcntl(int fd, uint64_t cmd, uint64_t arg0, uint64_t arg1)
{
    (void)fd;
    if (cmd != PACHA_FD_FCNTL_DUP || arg0 != 16 ||
        arg1 != (PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_CLOSE)) return -1;
    mock_vmo_read_offset = 0;
    return mock_vmo_dup_result;
}
long pacha_fd_read(int fd, void *buffer, uint64_t length)
{
    (void)fd;
    ++mock_vmo_read_calls;
    if (mock_vmo_read_error) return mock_vmo_read_error;
    if (length > mock_vmo_read_size - mock_vmo_read_offset)
        length = mock_vmo_read_size - mock_vmo_read_offset;
    memcpy(buffer, mock_vmo_read_data + mock_vmo_read_offset, (size_t)length);
    mock_vmo_read_offset += length;
    return (long)length;
}
static uint64_t mock_link_old_object_id;
static uint64_t mock_link_new_parent_object_id;
static char mock_link_new_name[STORAGE_NAME_BYTES];

int pacha_vmo_create(uint64_t size, uint64_t rights, uint32_t flags)
{
    (void)size;
    mock_create_rights = rights;
    mock_create_flags = flags;
    return mock_create_result;
}

int pacha_fd_table(uint64_t minimum_capacity, struct pacha_fd_table_info *out)
{
    (void)minimum_capacity;
    (void)out;
    return -1;
}

int pacha_vmo_revoke(int fd)
{
    (void)fd;
    ++mock_vmo_revoke_calls;
    return 0;
}

int pacha_vmo_grow(int fd, uint64_t capacity)
{
    (void)fd;
    ++mock_grow_calls;
    mock_grow_capacity = capacity;
    return mock_grow_error;
}

void *pacha_mmap(int fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset)
{
    (void)fd;
    (void)size;
    (void)prot;
    (void)flags;
    (void)offset;
    return mock_mmap_result;
}

int pacha_munmap(void *addr, uint64_t size)
{
    (void)addr;
    (void)size;
    return mock_munmap_error;
}

int pacha_fd_close(int fd)
{
    (void)fd;
    ++mock_fd_close_calls;
    return mock_fd_close_error;
}

int pacha_fd_get_info(int fd, struct pacha_fd_info *out)
{
    (void)fd;
    *out = mock_info;
    return mock_info_error;
}

int filed_kobox_backend_lookup(filed_kobox_backend_t *backend, uint64_t parent_object_id, const char *name, uint64_t *out_object_id)
{
    (void)backend;
    (void)parent_object_id;
    (void)name;
    (void)out_object_id;
    return -95;
}

int filed_kobox_backend_statx(filed_kobox_backend_t *backend, uint64_t object_id, storage_statx_reply_t *out_stat)
{
    (void)backend;
    (void)object_id;
    (void)out_stat;
    return -95;
}

int filed_kobox_backend_statfs(
    filed_kobox_backend_t *backend,
    storage_statfs_reply_t *out_statfs)
{
    (void)backend;
    (void)out_statfs;
    return -95;
}

int filed_kobox_backend_pread(filed_kobox_backend_t *backend, uint64_t object_id, uint64_t offset, void *buffer, uint64_t length, uint64_t *out_bytes)
{
    (void)backend;
    if (object_id == 43 && buffer != NULL && out_bytes != NULL) {
        if (offset >= stream_backend_read_error_at) { *out_bytes = 0; return -5; }
        const uint64_t available = offset < stream_backend_size ? stream_backend_size - offset : 0;
        *out_bytes = length < available ? length : available;
        if (*out_bytes != 0) memcpy(buffer, stream_backend_data + offset, (size_t)*out_bytes);
        return 0;
    }
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
    if (object_id == 43 && buffer != NULL && out_bytes != NULL &&
        offset <= sizeof(stream_backend_data) && length <= sizeof(stream_backend_data) - offset) {
        if (stream_backend_write_error != 0) return stream_backend_write_error;
        memcpy(stream_backend_data + offset, buffer, (size_t)length);
        if (offset + length > stream_backend_size) stream_backend_size = offset + length;
        *out_bytes = length;
        return 0;
    }
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

int filed_kobox_backend_symlink(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    const char *target,
    uint64_t *out_object_id)
{
    (void)backend;
    (void)parent_object_id;
    (void)name;
    (void)target;
    (void)out_object_id;
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

int filed_kobox_backend_mknod(filed_kobox_backend_t *backend, uint64_t parent_object_id, const char *name, uint64_t mode, uint64_t dev, uint64_t *out_object_id)
{
    (void)backend;
    (void)parent_object_id;
    (void)name;
    (void)mode;
    (void)dev;
    (void)out_object_id;
    return -95;
}

int filed_kobox_backend_truncate(filed_kobox_backend_t *backend, uint64_t object_id, uint64_t size)
{
    (void)backend;
    if (object_id == 43 && size <= sizeof(stream_backend_data)) {
        if (stream_backend_truncate_error != 0) return stream_backend_truncate_error;
        if (size > stream_backend_size)
            memset(stream_backend_data + stream_backend_size, 0,
                (size_t)(size - stream_backend_size));
        stream_backend_size = size;
        return 0;
    }
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

int filed_kobox_backend_link(
    filed_kobox_backend_t *backend,
    uint64_t old_object_id,
    uint64_t new_parent_object_id,
    const char *new_name,
    uint64_t *out_object_id)
{
    (void)backend;
    ++mock_link_calls;
    mock_link_old_object_id = old_object_id;
    mock_link_new_parent_object_id = new_parent_object_id;
    snprintf(mock_link_new_name, sizeof(mock_link_new_name), "%s", new_name);
    *out_object_id = 909;
    return 0;
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

int filed_kobox_backend_getdents(filed_kobox_backend_t *backend, uint64_t dir_object_id, uint64_t offset, storage_getdents_request_t *out_entries)
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
    /* Fixtures own only synthetic descriptors/mappings; release metadata
     * without calling backend flush or touching expired stack buffers. */
    filed_file_vmo_bank_t *bank = dispatch->cache.file_vmo.banks;
    while (bank != NULL) {
        filed_file_vmo_bank_t *next = bank->next;
        free(bank);
        bank = next;
    }
    memset(runtime, 0, sizeof(*runtime));
    memset(dispatch, 0, sizeof(*dispatch));
    runtime->dispatch_state = dispatch;
    filed_tmpfs_backend_init(&runtime->tmpfs);
    filed_cache_configure(runtime, FILED_PAGE_CACHE_SLOTS);
}

static int dirents_contain(const storage_getdents_request_t *entries, const char *name)
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
    storage_getdents_request_t entries;
    storage_getdents_request_t cached;

    init_runtime(&runtime, &dispatch);
    expect_int("mount root unlink", filed_tmpfs_backend_mount_root(&runtime.tmpfs, &root), 0);
    expect_int("create unlink target", filed_tmpfs_backend_create(&runtime.tmpfs, root, "gone", 0644, &file), 0);

    memset(&entries, 0, sizeof(entries));
    entries.capacity = FILED_DIRENT_CAPACITY;
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
    entries.capacity = FILED_DIRENT_CAPACITY;
    expect_int("getdents after unlink", filed_tmpfs_backend_getdents(&runtime.tmpfs, root, 0, &entries), 0);
    expect_true("dirents omit after unlink", !dirents_contain(&entries, "gone"));
}

static void test_link_routes_to_matching_backend(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    uint64_t linked = 0;
    uint64_t tmpfs_root = 0;

    init_runtime(&runtime, &dispatch);
    mock_link_calls = 0;
    mock_link_old_object_id = 0;
    mock_link_new_parent_object_id = 0;
    memset(mock_link_new_name, 0, sizeof(mock_link_new_name));

    expect_int(
        "ext4 link route",
        filed_backend_link(&runtime, 42, 7, "alias", &linked),
        0);
    expect_int("ext4 link call count", mock_link_calls, 1);
    expect_u64("ext4 link old object", mock_link_old_object_id, 42);
    expect_u64("ext4 link new parent", mock_link_new_parent_object_id, 7);
    expect_true("ext4 link new name", strcmp(mock_link_new_name, "alias") == 0);
    expect_u64("ext4 link object", linked, 909);

    expect_int(
        "mount tmpfs root for cross-backend link",
        filed_tmpfs_backend_mount_root(&runtime.tmpfs, &tmpfs_root),
        0);
    expect_int(
        "cross-backend link rejected",
        filed_backend_link(&runtime, 42, tmpfs_root, "cross", &linked),
        -18);
    expect_int("cross-backend link bypasses kobox", mock_link_calls, 1);
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

    filed_negative_lookup_cache_store(&runtime, 42, 1, "removed", -2);
    filed_cache_invalidate_namespace(&runtime, 42);
    expect_int("namespace change does not revoke shared mapping", mock_vmo_revoke_calls, 0);
    expect_true("namespace change preserves shared I/O source",
        filed_file_vmo_cache_shared_lookup(&runtime, 42) == entry);
    int64_t negative_status = 0;
    expect_true("namespace change clears negative lookup",
        !filed_negative_lookup_cache_get(&runtime, 42, 1, "removed", &negative_status));
    shared_bytes[1] = 'Z';
    expect_int("namespace shared pread", filed_cached_pread(&runtime, 42, 1, readback, 1, &bytes), 0);
    expect_int("namespace shared coherent byte", readback[0], 'Z');

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

static void test_shared_vmo_failed_growth_keeps_existing_mappings(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    unsigned char data[4096];
    memset(data, 0x5a, sizeof(data));
    init_runtime(&runtime, &dispatch);
    stream_backend_size = sizeof(data);
    stream_backend_write_error = 0;
    stream_backend_read_error_at = UINT64_MAX;
    mock_info_error = -1;
    mock_vmo_revoke_calls = 0;
    filed_file_vmo_cache_entry_t *entry = filed_file_vmo_cache_slot(&runtime);
    expect_true("shared growth initial slot", entry != NULL);
    if (!entry) return;
    *entry = (filed_file_vmo_cache_entry_t){
        .active = 1, .shared = 1, .writable_lent = 1, .vmo_fd = 33,
        .backend_object = 43, .length = sizeof(data),
        .logical_size = sizeof(data), .mapped = data,
    };
    filed_file_vmo_cache_entry_t *grown = NULL;
    /* Existing aliases must survive even when allocating the extension fails.
     * The VMO allocator mock fails; losing those aliases turns a recoverable
     * mmap error into a later unrelated reader/writer's native page fault. */
    expect_int("shared growth reports allocation failure",
        filed_cache_create_shared_vmo(&runtime, 43, 1, sizeof(data),
            2 * sizeof(data), &grown), -12);
    expect_true("failed growth has no new mapping", grown == NULL);
    expect_int("failed shared growth must not revoke", mock_vmo_revoke_calls, 0);
    expect_true("failed shared growth keeps old VMO",
        filed_file_vmo_cache_shared_lookup(&runtime, 43) == entry &&
        entry->vmo_fd == 33 && entry->mapped == data &&
        entry->length == sizeof(data));
}

static void test_shared_vmo_growth_preserves_identity_and_failures(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    unsigned char data[16384];
    memset(data, 0x5a, sizeof(data));
    init_runtime(&runtime, &dispatch);
    stream_backend_size = 4000;
    stream_backend_write_error = stream_backend_truncate_error = 0;
    mock_info_error = -1;
    mock_vmo_revoke_calls = mock_grow_calls = 0;
    mock_grow_error = 0;
    mock_mmap_result = data;
    mock_munmap_error = 0;
    filed_file_vmo_cache_entry_t *entry = filed_file_vmo_cache_slot(&runtime);
    expect_true("shared resize slot", entry != NULL);
    if (!entry) return;
    *entry = (filed_file_vmo_cache_entry_t){
        .active = 1, .shared = 1, .writable_lent = 1, .vmo_fd = 33,
        .backend_object = 43, .length = 4096, .logical_size = 4000, .mapped = data,
    };
    expect_int("truncate within capacity", filed_cache_truncate(&runtime, 43, 4050), 0);
    expect_int("within capacity does not grow", mock_grow_calls, 0);
    expect_int("within capacity keeps prefix", data[48], 0x5a);
    expect_int("truncate partial-page tail zero", data[4000], 0);
    expect_int("truncate leaves beyond EOF alone", data[4050], 0x5a);
    expect_int("truncate true growth", filed_cache_truncate(&runtime, 43, 8192), 0);
    expect_int("truncate grows once", mock_grow_calls, 1);
    expect_u64("native grow capacity", mock_grow_capacity, 8192);
    expect_true("truncate keeps VMO identity", entry->vmo_fd == 33 && entry->mapped == data);
    expect_u64("truncate logical size", entry->logical_size, 8192);
    expect_int("truncate full tail zero", data[8191], 0);
    expect_int("truncate never revokes", mock_vmo_revoke_calls, 0);

    mock_mmap_result = NULL;
    filed_file_vmo_cache_entry_t *grown = NULL;
    expect_int("local remap failure", filed_cache_create_shared_vmo(&runtime,
        43, 1, 8192, 12288, &grown), -12);
    expect_true("local remap failure preserves prefix", entry->mapped == data && entry->length == 8192);
    expect_u64("failed remap retains capacity charge", filed_file_vmo_cache_bytes(entry), 12288);
    expect_u64("failed remap logical size unchanged", entry->logical_size, 8192);
    const int grows_before_retry = mock_grow_calls;
    mock_mmap_result = data;
    expect_int("remap retry", filed_cache_create_shared_vmo(&runtime,
        43, 1, 8192, 12288, &grown), 0);
    expect_int("remap retry reuses backing", mock_grow_calls, grows_before_retry);
    expect_true("remap retry same entry", grown == entry);
    expect_u64("reserve does not extend file", entry->logical_size, 8192);

    stream_backend_truncate_error = -28;
    expect_int("backend truncate failure", filed_cache_truncate(&runtime, 43, 10000), -28);
    expect_u64("backend failure retains old logical size", entry->logical_size, 8192);
    expect_int("backend failure keeps prefix", data[48], 0x5a);
    stream_backend_truncate_error = 0;
    const unsigned char written = 0x37;
    uint64_t bytes = 0;
    expect_int("pwrite extends same shared backing", filed_cached_pwrite(&runtime,
        43, 13000, &written, 1, &bytes), 0);
    expect_u64("pwrite actual bytes", bytes, 1);
    expect_u64("pwrite aligned backing", mock_grow_capacity, 16384);
    expect_u64("pwrite logical size", entry->logical_size, 13001);
    expect_int("pwrite zeroes hole", data[10000], 0);
    expect_int("pwrite visible through old alias", data[13000], 0x37);
    expect_int("all growth never revokes", mock_vmo_revoke_calls, 0);
    mock_mmap_result = NULL;
    mock_grow_error = -3;
}

static void test_shared_vmo_growth_budget_and_cleanup(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    unsigned char old_data[4096], wider_data[12288];
    init_runtime(&runtime, &dispatch);
    mock_info_error = -1;
    mock_vmo_revoke_calls = mock_grow_calls = 0;
    mock_grow_error = 0;
    mock_mmap_result = wider_data;
    filed_file_vmo_cache_entry_t *entry = filed_file_vmo_cache_slot(&runtime);
    *entry = (filed_file_vmo_cache_entry_t){
        .active = 1, .shared = 1, .vmo_fd = 33,
        .backend_object = 43, .length = 4096, .logical_size = 4096, .mapped = old_data,
    };
    mock_munmap_error = -1;
    filed_file_vmo_cache_entry_t *grown = NULL;
    expect_int("failed old alias cleanup preserves both mappings",
        filed_cache_create_shared_vmo(&runtime, 43, 1, 4096, 8192, &grown), 0);
    expect_true("old alias tracked until cleanup", entry->retired_mapping == old_data &&
        entry->retired_length == 4096 && entry->mapped == wider_data);
    expect_int("do not accumulate untracked aliases",
        filed_cache_create_shared_vmo(&runtime, 43, 1, 4096, 12288, &grown), -5);
    expect_int("pending cleanup prevents another grow", mock_grow_calls, 1);
    mock_munmap_error = 0;
    expect_int("old alias cleanup retry",
        filed_cache_create_shared_vmo(&runtime, 43, 1, 4096, 12288, &grown), 0);
    expect_true("old alias cleanup completed", entry->retired_mapping == NULL);

    filed_file_vmo_cache_entry_t *held = filed_file_vmo_cache_slot(&runtime);
    *held = (filed_file_vmo_cache_entry_t){
        .active = 1, .shared = 1, .vmo_fd = 34, .backend_object = 44,
        .length = FILED_FILE_VMO_CACHE_TOTAL_BYTES - 12288,
    };
    expect_int("held backing enforces growth budget",
        filed_cache_create_shared_vmo(&runtime, 43, 1, 4096, 16384, &grown), -28);
    expect_true("budget failure retains both owners", entry->active && held->active);
    expect_int("budget failure never revokes", mock_vmo_revoke_calls, 0);
    mock_grow_error = -3;
    mock_mmap_result = NULL;
}

static void test_global_sync_keeps_tmpfs_shared_vmo_authoritative(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    unsigned char data[4096], readback[4096];
    memset(data, 0x5a, sizeof(data));
    init_runtime(&runtime, &dispatch);
    uint64_t root = 0, object = 0, bytes = 0;
    expect_int("sync tmpfs root", filed_tmpfs_backend_mount_root(&runtime.tmpfs, &root), 0);
    expect_int("sync tmpfs file", filed_tmpfs_backend_create(&runtime.tmpfs, root,
        "shared", 0600, &object), 0);
    expect_int("sync tmpfs size", filed_tmpfs_backend_truncate(&runtime.tmpfs,
        object, sizeof(data)), 0);
    filed_file_vmo_cache_entry_t *entry = filed_file_vmo_cache_slot(&runtime);
    expect_true("sync tmpfs VMO slot", entry != NULL);
    if (entry == NULL) return;
    *entry = (filed_file_vmo_cache_entry_t){
        .active = 1, .shared = 1, .dirty = 1, .writable_lent = 1, .vmo_fd = -1,
        .backend_object = object, .length = sizeof(data),
        .logical_size = sizeof(data), .mapped = data,
    };
    const uint32_t free_pages = runtime.tmpfs.free_page_count;
    /* A global disk sync must not need a second RAM copy, even at quota. */
    runtime.tmpfs.free_page_count = 0;
    expect_int("global sync needs no tmpfs backing", filed_cache_flush_object(&runtime, 0), 0);
    expect_true("global sync preserves pending tmpfs backing", entry->dirty);
    expect_true("global sync keeps shared source",
        filed_file_vmo_cache_shared_lookup(&runtime, object) == entry);
    expect_int("tmpfs coherent read after sync", filed_cached_pread(&runtime,
        object, 0, readback, sizeof(readback), &bytes), 0);
    expect_u64("tmpfs coherent read length", bytes, sizeof(readback));
    expect_bytes("tmpfs coherent read data", readback, data, sizeof(data));
    /* Explicit object flush (resize/invalidation/exec) still materializes it. */
    expect_int("explicit flush reports quota", filed_cache_flush_object(&runtime, object), -28);
    runtime.tmpfs.free_page_count = free_pages;
    expect_int("explicit flush materializes data", filed_cache_flush_object(&runtime, object), 0);
    expect_u64("one backing page on demand", runtime.tmpfs.free_page_count, free_pages - 1u);
    memset(entry, 0, sizeof(*entry));
    expect_int("backend read after explicit flush", filed_tmpfs_backend_pread(&runtime.tmpfs,
        object, 0, readback, sizeof(readback), &bytes), 0);
    expect_bytes("explicit flush backend data", readback, data, sizeof(data));
    expect_int("sync tmpfs cleanup", filed_tmpfs_backend_truncate(&runtime.tmpfs, object, 0), 0);
}

static void test_flush_all_continues_after_shared_error(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    unsigned char failed_data[] = "pending";
    unsigned char healthy_data[] = "persist";
    init_runtime(&runtime, &dispatch);
    memset(mock_backend_data, 0, sizeof(mock_backend_data));
    mock_backend_size = 0;
    filed_file_vmo_cache_entry_t *entry = filed_file_vmo_cache_slot(&runtime);
    *entry = (filed_file_vmo_cache_entry_t){
        .active = 1, .shared = 1, .dirty = 1, .vmo_fd = -1,
        .backend_object = 43, .length = sizeof(failed_data),
        .logical_size = sizeof(failed_data), .mapped = failed_data,
    };
    filed_file_vmo_cache_entry_t *healthy = filed_file_vmo_cache_slot(&runtime);
    *healthy = (filed_file_vmo_cache_entry_t){
        .active = 1, .shared = 1, .dirty = 1, .vmo_fd = -1,
        .backend_object = 42, .length = sizeof(healthy_data),
        .logical_size = sizeof(healthy_data), .mapped = healthy_data,
    };
    stream_backend_write_error = -28;
    expect_int("flush-all retains first failure", filed_cache_flush_object(&runtime, 0), -28);
    expect_true("failed shared data stays dirty", entry->dirty);
    expect_bytes("other file flushed despite shared error", mock_backend_data,
        healthy_data, sizeof(healthy_data));
    stream_backend_write_error = 0;
    expect_int("failed shared data can retry", filed_cache_flush_object(&runtime, 0), 0);
    expect_true("retry clears shared dirty", !entry->dirty);
    expect_bytes("retried shared data preserved", stream_backend_data,
        failed_data, sizeof(failed_data));
    memset(entry, 0, sizeof(*entry));
    memset(healthy, 0, sizeof(*healthy));
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
    expect_true("vmo cache budget preserves costly snapshot", oldest->active);
    expect_true("vmo cache budget evicts cheaper snapshot", !newer->active);
    expect_true("vmo cache budget reuses cheaper slot", slot == newer);
    expect_u64("vmo cache counts successful budget victims",
        runtime.dispatch_state->cache.file_vmo.byte_budget_evictions, 1);
}

static void test_snapshot_vmo_pins_backend_object(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    init_runtime(&runtime, &dispatch);

    filed_file_vmo_cache_entry_t *entry = filed_file_vmo_cache_slot(&runtime);
    expect_true("snapshot vmo cache slot", entry != NULL);
    if (entry == NULL) {
        return;
    }
    memset(entry, 0, sizeof(*entry));
    entry->active = 1;
    entry->vmo_fd = -1;
    entry->backend_object = 900;
    entry->length = 4096;

    expect_true(
        "snapshot vmo pins backend object",
        !filed_cache_object_evictable(&runtime, 900));

    entry->shared = 1;
    expect_true(
        "shared vmo pins backend object",
        !filed_cache_object_evictable(&runtime, 900));

    entry->shared = 0;
    filed_cache_release_object(&runtime, 900);
    expect_true("snapshot vmo released with backend", !entry->active);
}

static void test_pinned_file_vmo_limit_preserves_costly_snapshot(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    init_runtime(&runtime, &dispatch);

    filed_file_vmo_cache_entry_t *shared = filed_file_vmo_cache_slot(&runtime);
    expect_true("pinned limit shared slot", shared != NULL);
    if (shared == NULL) {
        return;
    }
    memset(shared, 0, sizeof(*shared));
    shared->active = 1;
    shared->shared = 1;
    shared->vmo_fd = -1;
    shared->backend_object = 1;
    shared->clock = 1;

    filed_file_vmo_cache_entry_t *costly_oldest = NULL;
    filed_file_vmo_cache_entry_t *cheapest = NULL;
    for (uint32_t i = 0; i < FILED_FILE_VMO_PINNED_SNAPSHOT_LIMIT; ++i) {
        filed_file_vmo_cache_entry_t *entry = filed_file_vmo_cache_slot(&runtime);
        expect_true("pinned limit fill slot", entry != NULL);
        if (entry == NULL) {
            return;
        }
        memset(entry, 0, sizeof(*entry));
        entry->active = 1;
        entry->vmo_fd = -1;
        entry->backend_object = 100 + i;
        entry->clock = i + 2;
        entry->length = i == 0 ? 178u * 1024u * 1024u : (uint64_t)(i + 1u) * 4096u;
        if (costly_oldest == NULL) {
            costly_oldest = entry;
        }
        if (i == 1) {
            cheapest = entry;
        }
    }

    filed_file_vmo_cache_entry_t *replacement =
        filed_file_vmo_cache_pinned_slot_for_length(&runtime, 4096);
    expect_true("pinned limit replacement slot", replacement != NULL);
    expect_true("pinned limit preserves shared", shared->active);
    expect_true(
        "pinned limit preserves costly oldest snapshot",
        costly_oldest != NULL && costly_oldest->active);
    expect_true(
        "pinned limit evicts cheapest snapshot",
        cheapest != NULL && !cheapest->active);
    expect_true("pinned limit returns reusable metadata", replacement != NULL && !replacement->active);
}

static void test_many_library_snapshots_reused(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    init_runtime(&runtime, &dispatch);
    /* A multi-process GTK/WebKit dependency set exceeds 24 files even
     * while well below the byte budget. Eviction creates another physical
     * image while the first process still maps the previous snapshot. */
    for (unsigned i = 0; i < 64; ++i) {
        filed_file_vmo_cache_entry_t *entry =
            filed_file_vmo_cache_pinned_slot_for_length(&runtime, 4u * 1024u * 1024u);
        expect_true("library snapshot slot", entry != NULL);
        if (!entry) return;
        *entry = (filed_file_vmo_cache_entry_t){
            .active = 1, .vmo_fd = -1, .backend_object = 100 + i,
            .object_generation = 1, .length = 4u * 1024u * 1024u,
            .clock = i + 1,
        };
    }
    for (unsigned round = 0; round < 3; ++round)
        for (unsigned i = 0; i < 64; ++i)
            expect_true("next process reuses library snapshot",
                filed_file_vmo_cache_lookup(&runtime, 100 + i, 1, 0,
                    4u * 1024u * 1024u) != NULL);
}

static void test_snapshot_exact_range_reuse(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    init_runtime(&runtime, &dispatch);
    filed_file_vmo_cache_entry_t *entry = filed_file_vmo_cache_slot(&runtime);
    expect_true("prefix slot", entry != NULL);
    if (!entry) return;
    *entry = (filed_file_vmo_cache_entry_t){ .active = 1, .vmo_fd = -1,
        .backend_object = 91, .object_generation = 3, .file_offset = 4096,
        .length = 65536 };
    expect_true("shorter snapshot is not an exact range hit",
        filed_file_vmo_cache_lookup(&runtime, 91, 3, 4096, 8192) == NULL);
    expect_true("exact snapshot still reused",
        filed_file_vmo_cache_lookup(&runtime, 91, 3, 4096, 65536) == entry);
    expect_true("larger snapshot is not a hit",
        filed_file_vmo_cache_lookup(&runtime, 91, 3, 4096, 65537) == NULL);
    expect_true("different start is not a hit",
        filed_file_vmo_cache_lookup(&runtime, 91, 3, 8192, 8192) == NULL);
    expect_true("different generation is not a hit",
        filed_file_vmo_cache_lookup(&runtime, 91, 4, 4096, 8192) == NULL);
    entry->shared = 1;
    expect_true("mutable shared image is not a snapshot",
        filed_file_vmo_cache_lookup(&runtime, 91, 3, 4096, 8192) == NULL);
}

static void test_snapshot_vmo_pressure_reclaim_preserves_shared(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    init_runtime(&runtime, &dispatch);
    mock_fd_close_calls = 0;

    filed_file_vmo_cache_entry_t *first = filed_file_vmo_cache_slot(&runtime);
    expect_true("pressure reclaim first slot", first != NULL);
    if (first == NULL) {
        return;
    }
    memset(first, 0, sizeof(*first));
    first->active = 1;
    first->vmo_fd = 40;
    first->length = 4096;

    filed_file_vmo_cache_entry_t *second = filed_file_vmo_cache_slot(&runtime);
    expect_true("pressure reclaim second slot", second != NULL);
    if (second == NULL) {
        return;
    }
    memset(second, 0, sizeof(*second));
    second->active = 1;
    second->vmo_fd = 41;
    second->length = 8192;

    filed_file_vmo_cache_entry_t *shared = filed_file_vmo_cache_slot(&runtime);
    expect_true("pressure reclaim shared slot", shared != NULL);
    if (shared == NULL) {
        return;
    }
    memset(shared, 0, sizeof(*shared));
    shared->active = 1;
    shared->shared = 1;
    shared->vmo_fd = 42;
    shared->length = 16384;

    uint64_t reclaimed_bytes = 0;
    expect_u64(
        "pressure reclaim snapshot count",
        filed_file_vmo_cache_reclaim_snapshots(&runtime, &reclaimed_bytes),
        2);
    expect_u64("pressure reclaim snapshot bytes", reclaimed_bytes, 12288);
    expect_true("pressure reclaim clears first", !first->active);
    expect_true("pressure reclaim clears second", !second->active);
    expect_true("pressure reclaim preserves shared", shared->active);
    expect_int("pressure reclaim closes owners", mock_fd_close_calls, 2);
}

static void test_partial_write_cache_is_not_eof(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    unsigned char input[4096];
    unsigned char output[8192];
    for (int flush = 0; flush < 2; ++flush) {
        init_runtime(&runtime, &dispatch);
        stream_backend_size = 2u * FILED_PAGE_CACHE_BYTES;
        memset(stream_backend_data, 'a', (size_t)stream_backend_size);
        memset(input, 'b', sizeof(input));
        uint64_t bytes = 0;
        expect_int("partial overwrite", filed_cached_pwrite_ex(
            &runtime, 43, 0, input, sizeof(input), &bytes, true), 0);
        expect_u64("partial overwrite bytes", bytes, sizeof(input));
        if (flush) expect_int("partial overwrite flush", filed_cache_flush_object(&runtime, 43), 0);
        expect_int("read beyond cached write", filed_cached_pread(
            &runtime, 43, sizeof(input), output, sizeof(output), &bytes), 0);
        expect_u64("cached write end is not EOF", bytes, sizeof(output));
        if (bytes == sizeof(output)) {
            memset(input, 'a', sizeof(input));
            expect_bytes("existing suffix preserved", output, input, sizeof(input));
        }
        expect_int("read across cached write", filed_cached_pread(
            &runtime, 43, 0, output, sizeof(output), &bytes), 0);
        expect_u64("read across cached write bytes", bytes, sizeof(output));
        memset(input, 'b', sizeof(input));
        expect_bytes("dirty prefix preserved", output, input, sizeof(input));
    }
}

static void test_large_stream_cache_roundtrip(uint64_t request_bytes)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    unsigned char input[FILED_IO_BYTES];
    unsigned char output[FILED_IO_BYTES];
    const uint64_t size = 4770488;
    init_runtime(&runtime, &dispatch);
    stream_backend_size = 0;
    memset(stream_backend_data, 0, sizeof(stream_backend_data));
    for (uint64_t offset = 0; offset < size;) {
        uint64_t chunk = size - offset < sizeof(input) ? size - offset : sizeof(input);
        const uint64_t request_remaining = request_bytes - offset % request_bytes;
        if (chunk > request_remaining) chunk = request_remaining;
        for (uint64_t i = 0; i < chunk; ++i) input[i] = (unsigned char)((offset + i) * 17u + (offset + i) / 4096u);
        uint64_t bytes = 0;
        expect_int("stream write", filed_cached_pwrite_ex(&runtime, 43, offset, input, chunk, &bytes, true), 0);
        expect_u64("stream write bytes", bytes, chunk);
        if (bytes != chunk) return;
        offset += chunk;
    }
    expect_int("stream flush", filed_cache_flush_object(&runtime, 43), 0);
    expect_u64("stream backing size", stream_backend_size, size);
    for (uint64_t offset = 0; offset < size;) {
        const uint64_t chunk = size - offset < sizeof(output) ? size - offset : sizeof(output);
        uint64_t bytes = 0;
        expect_int("stream read", filed_cached_pread(&runtime, 43, offset, output, chunk, &bytes), 0);
        expect_u64("stream read bytes", bytes, chunk);
        if (bytes != chunk) return;
        for (uint64_t i = 0; i < chunk; ++i) input[i] = (unsigned char)((offset + i) * 17u + (offset + i) / 4096u);
        expect_bytes("stream read contents", output, input, (size_t)chunk);
        offset += chunk;
    }
}

static void test_sparse_dirty_cache_read_hole(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    unsigned char input[4096];
    unsigned char output[4096];
    init_runtime(&runtime, &dispatch);
    stream_backend_size = 0;
    memset(stream_backend_data, 0, sizeof(stream_backend_data));
    memset(input, 'p', sizeof(input));
    uint64_t bytes = 0;
    expect_int("sparse prefix write", filed_cached_pwrite_ex(
        &runtime, 43, 0, input, sizeof(input), &bytes, true), 0);
    expect_u64("sparse prefix bytes", bytes, sizeof(input));
    memset(input, 's', sizeof(input));
    expect_int("sparse suffix write", filed_cached_pwrite_ex(
        &runtime, 43, 3u * FILED_PAGE_CACHE_BYTES, input, sizeof(input), &bytes, true), 0);
    expect_u64("sparse suffix bytes", bytes, sizeof(input));
    /* Both writes are still dirty. A miss in the hole must not mistake the
     * backend's pre-writeback size for the current file's EOF. */
    stream_backend_write_error = -5;
    expect_int("sparse read propagates flush failure", filed_cached_pread(
        &runtime, 43, FILED_PAGE_CACHE_BYTES, output, sizeof(output), &bytes), -5);
    expect_u64("failed sparse read bytes", bytes, 0);
    expect_true("failed sparse flush retains dirty data", filed_cache_object_dirty(&runtime, 43));
    stream_backend_write_error = 0;
    expect_int("read sparse hole", filed_cached_pread(
        &runtime, 43, FILED_PAGE_CACHE_BYTES, output, sizeof(output), &bytes), 0);
    expect_u64("sparse hole is not EOF", bytes, sizeof(output));
    if (bytes == sizeof(output)) {
        memset(input, 0, sizeof(input));
        expect_bytes("sparse hole zeros", output, input, sizeof(output));
    }
    expect_int("read sparse suffix", filed_cached_pread(
        &runtime, 43, 3u * FILED_PAGE_CACHE_BYTES, output, sizeof(output), &bytes), 0);
    expect_u64("sparse suffix read bytes", bytes, sizeof(output));
    memset(input, 's', sizeof(input));
    expect_bytes("sparse suffix preserved", output, input, sizeof(output));
}

static void test_partial_read_before_io_error(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    for (unsigned dirty = 0; dirty < 2; ++dirty) {
        init_runtime(&runtime, &dispatch);
        stream_backend_size = 2u * FILED_PAGE_CACHE_BYTES;
        memset(stream_backend_data, 'a', (size_t)stream_backend_size);
        uint64_t bytes = 0;
        if (dirty) {
            expect_int("partial-error dirty write", filed_cached_pwrite_ex(
                &runtime,43,FILED_PAGE_CACHE_BYTES+1,"z",1,&bytes,true),0);
            stream_backend_write_error = -5;
        } else {
            stream_backend_read_error_at = FILED_PAGE_CACHE_BYTES;
        }
        unsigned char output[8];
        memset(output,'?',sizeof(output));
        expect_int("partial-error returns completed prefix", filed_cached_pread(
            &runtime,43,FILED_PAGE_CACHE_BYTES-4,output,sizeof(output),&bytes),0);
        expect_u64("partial-error prefix length",bytes,4);
        expect_bytes("partial-error prefix contents",output,"aaaa????",8);
        expect_int("partial-error retry reports failure", filed_cached_pread(
            &runtime,43,FILED_PAGE_CACHE_BYTES,output,4,&bytes),-5);
        expect_u64("partial-error retry transferred nothing",bytes,0);
        if (dirty) expect_true("partial-error retains dirty data",filed_cache_object_dirty(&runtime,43));
        stream_backend_write_error = 0;
        stream_backend_read_error_at = UINT64_MAX;
        expect_int("partial-error recovered retry",filed_cached_pread(
            &runtime,43,FILED_PAGE_CACHE_BYTES,output,4,&bytes),0);
        expect_u64("partial-error recovered bytes",bytes,4);
        expect_bytes("partial-error recovered contents",output,dirty ? "azaa" : "aaaa",4);
    }
}

static void test_shared_registry_grows_and_trims(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    filed_file_vmo_cache_entry_t *saved[641];
    init_runtime(&runtime, &dispatch);
    mock_info_error = -1;
    mock_vmo_revoke_calls = 0;
    for (unsigned i = 0; i < 641; ++i) {
        saved[i] = filed_file_vmo_cache_slot_for_length(&runtime, 4096);
        expect_true("dynamic registry accepts over 128 live entries", saved[i] != NULL);
        if (saved[i] == NULL) return;
        *saved[i] = (filed_file_vmo_cache_entry_t){
            .active = 1, .shared = 1, .vmo_fd = 16 + (int)i,
            .backend_object = 1000 + i, .length = 4096,
        };
    }
    for (unsigned i = 0; i < 641; ++i) {
        expect_true("registry growth preserves entry address",
            filed_file_vmo_cache_shared_lookup(&runtime, 1000 + i) == saved[i]);
        expect_true("all live entries pin their backend key",
            !filed_cache_object_evictable(&runtime, 1000 + i));
    }
    expect_int("growth never revokes", mock_vmo_revoke_calls, 0);
    /* Drop complete banks, leaving one live entry and stable address. */
    for (unsigned i = 0; i < 640; ++i) saved[i]->active = 0;
    filed_file_vmo_cache_trim_empty_banks(&runtime);
    expect_true("trim preserves live entry", saved[640]->active);
    expect_true("trim frees all empty banks",
        dispatch.cache.file_vmo.banks && !dispatch.cache.file_vmo.banks->next);
    saved[640]->active = 0;
    filed_file_vmo_cache_trim_empty_banks(&runtime);
    expect_true("empty registry returns all metadata", dispatch.cache.file_vmo.banks == NULL);
}

static void test_shared_idle_retirement(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    unsigned char data[4096] = "shared write";
    init_runtime(&runtime, &dispatch);
    filed_file_vmo_cache_entry_t *entry = filed_file_vmo_cache_slot(&runtime);
    expect_true("retirement entry", entry != NULL);
    if (entry == NULL) return;
    *entry = (filed_file_vmo_cache_entry_t){
        .active = 1, .shared = 1, .writable_lent = 1, .vmo_fd = 40,
        .backend_object = 43, .length = sizeof(data),
        .logical_size = sizeof(data), .mapped = data,
    };
    mock_info = (struct pacha_fd_info){
        .kind = PACHA_FD_KIND_VMO, .rights = PACHA_FD_RIGHT_INSPECT,
        .extra = 1ull | (2ull << PACHA_VMO_INFO_NATIVE_REFS_SHIFT),
    };
    mock_info_error = -1;
    expect_u64("failed query preserves shared entry",
        filed_file_vmo_cache_reclaim_idle_shared(&runtime), 0);
    mock_info_error = 0;
    const uint64_t exclusive = mock_info.extra;
    const uint64_t protected_counts[] = {
        0, /* Old kernel. */
        2ull | (2ull << 32), /* Duplicated FD or queued transfer. */
        1ull | (3ull << 32), /* Mapping without FD or backing view. */
        1ull | (4ull << 32), /* Fork/split mappings. */
    };
    for (unsigned i = 0; i < sizeof(protected_counts) / sizeof(protected_counts[0]); ++i) {
        mock_info.extra = protected_counts[i];
        expect_u64("external/unknown refs prevent retirement",
            filed_file_vmo_cache_reclaim_idle_shared(&runtime), 0);
        expect_true("protected shared data retained", entry->active && entry->mapped == data);
    }
    mock_info.extra = exclusive;
    mock_info.rights = 0;
    expect_u64("missing INSPECT prevents retirement",
        filed_file_vmo_cache_reclaim_idle_shared(&runtime), 0);
    mock_info.rights = PACHA_FD_RIGHT_INSPECT;
    stream_backend_write_error = -5;
    expect_u64("flush failure prevents retirement",
        filed_file_vmo_cache_reclaim_idle_shared(&runtime), 0);
    expect_true("flush failure preserves writable data", entry->active && entry->mapped == data);
    stream_backend_write_error = 0;
    mock_munmap_error = -1;
    expect_u64("unmap failure prevents retirement",
        filed_file_vmo_cache_reclaim_idle_shared(&runtime), 0);
    expect_true("unmap failure retains descriptor", entry->active && entry->mapped == data);
    mock_munmap_error = 0;
    mock_fd_close_error = -1;
    expect_u64("close failure retained for retry",
        filed_file_vmo_cache_reclaim_idle_shared(&runtime), 0);
    expect_true("failed close cannot lend unmapped entry",
        entry->active && entry->retiring &&
        filed_file_vmo_cache_shared_lookup(&runtime, 43) == NULL);
    mock_fd_close_error = 0;
    mock_info.extra = 1ull | (1ull << 32);
    mock_vmo_revoke_calls = 0;
    expect_u64("exclusive shared entry retired",
        filed_file_vmo_cache_reclaim_idle_shared(&runtime), 1);
    expect_true("retirement clears metadata", !entry->active);
    expect_bytes("retirement writes back contents", stream_backend_data, data, sizeof(data));
    expect_int("idle retirement never revokes", mock_vmo_revoke_calls, 0);
    filed_file_vmo_cache_trim_empty_banks(&runtime);
    mock_info_error = -1;
}

static void test_registry_growth_failure_and_idle_reuse(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    unsigned char data[4096] = {0};
    init_runtime(&runtime, &dispatch);
    mock_info_error = -1;
    for (unsigned i = 0; i < FILED_FILE_VMO_BANK_ENTRIES; ++i) {
        filed_file_vmo_cache_entry_t *entry = filed_file_vmo_cache_slot(&runtime);
        expect_true("growth failure fill", entry != NULL);
        if (!entry) return;
        *entry = (filed_file_vmo_cache_entry_t){
            .active = 1, .shared = 1, .vmo_fd = 40 + (int)i,
            .backend_object = 1000 + i, .length = sizeof(data), .mapped = data,
        };
    }
    filed_file_vmo_bank_t *original = dispatch.cache.file_vmo.banks;
    fail_next_bank_allocation = 1;
    expect_true("metadata OOM returns failure", filed_file_vmo_cache_slot(&runtime) == NULL);
    expect_true("metadata OOM retains bank", dispatch.cache.file_vmo.banks == original && !original->next);
    for (unsigned i = 0; i < FILED_FILE_VMO_BANK_ENTRIES; ++i)
        expect_true("metadata OOM preserves live entries",
            filed_file_vmo_cache_shared_lookup(&runtime, 1000 + i) != NULL);
    mock_info_error = 0;
    mock_info = (struct pacha_fd_info){ .kind = PACHA_FD_KIND_VMO,
        .rights = PACHA_FD_RIGHT_INSPECT, .extra = 1ull | (2ull << 32) };
    mock_vmo_revoke_calls = 0;
    expect_true("idle entries reused before growth", filed_file_vmo_cache_slot(&runtime) != NULL);
    expect_true("idle reuse avoids new bank", dispatch.cache.file_vmo.banks == original && !original->next);
    expect_int("idle reuse never revokes", mock_vmo_revoke_calls, 0);
    filed_file_vmo_cache_trim_empty_banks(&runtime);
    mock_info_error = -1;
}

static void test_sparse_shared_vmo_import_and_flush(void)
{
    static filed_runtime_t runtime;
    static filed_dispatch_state_t dispatch;
    static unsigned char mapping[3 * 4096], backing[3 * 4096];
    init_runtime(&runtime, &dispatch);
    uint64_t root = 0, object = 0, bytes = 0;
    expect_int("sparse shared root", filed_tmpfs_backend_mount_root(&runtime.tmpfs, &root), 0);
    expect_int("sparse shared file", filed_tmpfs_backend_create(&runtime.tmpfs, root, "import", 0600, &object), 0);
    expect_int("sparse shared content", filed_tmpfs_backend_pwrite(&runtime.tmpfs, object, 4096 + 7, "ok", 2, &bytes), 0);
    expect_int("sparse shared EOF", filed_tmpfs_backend_truncate(&runtime.tmpfs, object, sizeof(mapping)), 0);
    memset(mapping, 0xa5, sizeof(mapping));
    mock_create_result = 33;
    mock_mmap_result = mapping;
    filed_file_vmo_cache_entry_t *entry = NULL;
    expect_int("sparse shared create", filed_cache_create_shared_vmo(&runtime, object, 1, sizeof(mapping), sizeof(mapping), &entry), 0);
    expect_u64("sparse create flag", mock_create_flags, PACHA_VMO_CREATE_ZERO_ON_DEMAND);
    expect_true("sparse owner can read", (mock_create_rights & PACHA_FD_RIGHT_READ) != 0);
    expect_true("sparse entry mode", entry && entry->zero_on_demand);
    expect_bytes("sparse initial bytes", mapping + 4096 + 7, (const unsigned char *)"ok", 2);
    expect_int("sparse initial first hole untouched", mapping[0], 0xa5);
    expect_int("sparse initial last hole untouched", mapping[8192], 0xa5);
    if (entry) {
        memset(backing, 0, sizeof(backing));
        memcpy(backing + 4096 + 7, "new", 3);
        mock_vmo_read_data = backing;
        mock_vmo_read_size = sizeof(backing);
        mock_vmo_read_calls = 0;
        entry->dirty = 1;
        entry->writable_lent = 1;
        /* A non-dereferenceable mapping proves flush only uses FD_READ. */
        entry->mapped = (void *)(uintptr_t)1;
        const uint32_t free_before = runtime.tmpfs.free_page_count;
        expect_int("sparse explicit flush", filed_cache_flush_object(&runtime, object), 0);
        expect_u64("sparse bounded reads", mock_vmo_read_calls, 3);
        expect_u64("sparse flush does not allocate zero backend pages", runtime.tmpfs.free_page_count, free_before);
        unsigned char out[3];
        expect_int("sparse flush data read", filed_tmpfs_backend_pread(&runtime.tmpfs, object, 4103, out, 3, &bytes), 0);
        expect_bytes("sparse flush new bytes", out, (const unsigned char *)"new", 3);
        expect_int("sparse repeat starts at zero", filed_cache_flush_object(&runtime, object), 0);
        entry->dirty = 1;
        const int closes = mock_fd_close_calls;
        mock_vmo_read_error = -3;
        expect_int("sparse read failure", filed_cache_flush_object(&runtime, object), -5);
        expect_true("sparse failed flush keeps dirty", entry->dirty);
        expect_int("sparse failed read closes duplicate", mock_fd_close_calls, closes + 1);
        mock_vmo_read_error = 0;
        mock_vmo_dup_result = -3;
        expect_int("sparse dup failure", filed_cache_flush_object(&runtime, object), -5);
        expect_true("sparse dup failure keeps dirty", entry->dirty);
        mock_vmo_dup_result = 90;
        entry->mapped = mapping;
    }
    expect_int("sparse test cleanup", filed_tmpfs_backend_truncate(&runtime.tmpfs, object, 0), 0);
    mock_create_result = -1;
    mock_mmap_result = NULL;
}

int main(void)
{
    test_rename_clears_negative_lookup();
    test_unlink_clears_dirent_cache();
    test_link_routes_to_matching_backend();
    test_truncate_clears_page_and_vmo_cache();
    test_shared_vmo_is_io_source_and_revoke_target();
    test_shared_vmo_failed_growth_keeps_existing_mappings();
    test_shared_vmo_growth_preserves_identity_and_failures();
    test_shared_vmo_growth_budget_and_cleanup();
    test_global_sync_keeps_tmpfs_shared_vmo_authoritative();
    test_sparse_shared_vmo_import_and_flush();
    test_flush_all_continues_after_shared_error();
    test_file_vmo_cache_byte_budget();
    test_snapshot_vmo_pins_backend_object();
    test_pinned_file_vmo_limit_preserves_costly_snapshot();
    test_many_library_snapshots_reused();
    test_snapshot_exact_range_reuse();
    test_snapshot_vmo_pressure_reclaim_preserves_shared();
    test_shared_registry_grows_and_trims();
    test_shared_idle_retirement();
    test_registry_growth_failure_and_idle_reuse();
    test_partial_write_cache_is_not_eof();
    test_sparse_dirty_cache_read_hole();
    test_partial_read_before_io_error();
    test_large_stream_cache_roundtrip(4096);
    test_large_stream_cache_roundtrip(FILED_IO_BYTES);
    test_large_stream_cache_roundtrip(32768);
    test_large_stream_cache_roundtrip(65536);
    test_large_stream_cache_roundtrip(131072);
    if (failures != 0) {
        return 1;
    }
    printf("filed cache consistency tests passed\n");
    return 0;
}

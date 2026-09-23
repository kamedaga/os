#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../userland/filed/src/vfs/private.h"

static size_t fail_body_size;
static int fail_directory;
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_body_size == size) { fail_body_size = 0; return NULL; }
    return __real_calloc(count, size);
}
void *__wrap_realloc(void *ptr, size_t size)
{
    if (fail_directory) { fail_directory = 0; return NULL; }
    return __real_realloc(ptr, size);
}

static void growth_failure_and_trim(void)
{
    enum { COUNT = 320 };
    filed_vfs_t vfs;
    filed_vfs_init(&vfs);
    filed_mount_id_t mount;
    filed_vfs_open_result_t opened[COUNT], extra;
    assert(filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &mount) == FILED_OK);
    for (unsigned i = 0; i < COUNT; ++i)
        assert(filed_vfs_open_root(&vfs, mount, FILED_RIGHT_LOOKUP,
            FILED_OPEN_DIRECTORY, &opened[i]) == FILED_OK);
    filed_handle_t *first = filed_find_handle(&vfs, opened[0].handle_id);
    filed_file_t *file = filed_find_file(&vfs, first->target_id);
    assert(vfs.file_capacity == COUNT && vfs.handle_capacity == COUNT);
    assert(filed_vfs_check_basic(&vfs) == FILED_OK);

    fail_body_size = sizeof(filed_file_t);
    assert(filed_vfs_open_root(&vfs, mount, FILED_RIGHT_LOOKUP,
        FILED_OPEN_DIRECTORY, &extra) == FILED_ERR_FULL);
    assert(!fail_body_size);
    filed_vfs_trim_open_objects(&vfs);
    fail_directory = 1;
    assert(filed_vfs_open_root(&vfs, mount, FILED_RIGHT_LOOKUP,
        FILED_OPEN_DIRECTORY, &extra) == FILED_ERR_FULL);
    assert(!fail_directory);
    filed_vfs_trim_open_objects(&vfs);
    fail_body_size = sizeof(filed_handle_t);
    assert(filed_vfs_open_root(&vfs, mount, FILED_RIGHT_LOOKUP,
        FILED_OPEN_DIRECTORY, &extra) == FILED_ERR_FULL);
    assert(!fail_body_size);
    assert(filed_vfs_check_basic(&vfs) == FILED_OK);
    assert(filed_find_handle(&vfs, first->id) == first);
    assert(filed_find_file(&vfs, file->id) == file);

    assert(filed_vfs_open_root(&vfs, mount, FILED_RIGHT_LOOKUP,
        FILED_OPEN_DIRECTORY, &extra) == FILED_OK);
    assert(filed_vfs_set_handle_lease(&vfs, extra.handle_id, 999) == FILED_OK);
    assert(filed_vfs_set_handle_lease(&vfs, first->id, 1000) == FILED_OK);
    filed_vfs_trim_open_objects(&vfs);
    assert(filed_vfs_check_basic(&vfs) == FILED_OK);
    assert(filed_vfs_close_handle(&vfs, extra.handle_id) == FILED_OK);
    for (unsigned i = 1; i < COUNT; ++i)
        assert(filed_vfs_close_handle(&vfs, opened[i].handle_id) == FILED_OK);
    filed_vfs_trim_open_objects(&vfs);
    assert(vfs.file_capacity == FILED_FILE_BANK_ENTRIES);
    assert(vfs.handle_capacity == FILED_HANDLE_BANK_ENTRIES);
    assert(filed_find_handle(&vfs, first->id) == first);
    assert(filed_find_file(&vfs, file->id) == file);
    assert(filed_vfs_check_basic(&vfs) == FILED_OK);
    assert(filed_vfs_close_handle(&vfs, first->id) == FILED_OK);
    assert(filed_vfs_check_basic(&vfs) == FILED_OK);
    filed_vfs_destroy(&vfs);
    assert(!vfs.file_banks && !vfs.handle_banks);
}

static void wide_slots(void)
{
    filed_vfs_t vfs;
    filed_vfs_init(&vfs);
    /* Exercise 32-bit hint/link storage without quadratic allocation scans. */
    const unsigned banks = 65536 / FILED_HANDLE_BANK_ENTRIES + 1;
    vfs.handle_banks = calloc(banks, sizeof(*vfs.handle_banks));
    assert(vfs.handle_banks);
    for (unsigned b = 0; b < banks; ++b) {
        vfs.handle_banks[b] = calloc(FILED_HANDLE_BANK_ENTRIES, sizeof(filed_handle_t));
        assert(vfs.handle_banks[b]);
    }
    vfs.handle_capacity = banks * FILED_HANDLE_BANK_ENTRIES;
    filed_handle_t *wide = filed_vfs_handle_at(&vfs, 65536);
    *wide = (filed_handle_t){ .active = true, .id = 99, .lease_fd = -1 };
    filed_remember_handle_slot(&vfs, wide);
    assert(vfs.handle_slot_hints[filed_id_hint_index(99)] == 65537);
    assert(filed_find_handle(&vfs, 99) == wide);
    assert(filed_vfs_set_handle_lease(&vfs, 99, 100) == FILED_OK);
    assert(vfs.lease_head == 65537);
    assert(filed_vfs_close_handle(&vfs, 99) == FILED_OK);
    assert(!vfs.lease_head && !vfs.lease_handle_count);
    filed_vfs_trim_open_objects(&vfs);
    assert(vfs.handle_capacity == FILED_HANDLE_BANK_ENTRIES);
    filed_vfs_destroy(&vfs);
}

int main(void)
{
    growth_failure_and_trim();
    wide_slots();
    puts("FileD open objects: dynamic growth, stable pointers, OOM rollback, leases, trim and wide slots PASS");
}

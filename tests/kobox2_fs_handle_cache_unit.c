/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define GFP_KERNEL 0
struct file { unsigned int identity; };
struct xarray { unsigned long handles[128]; struct file *files[128]; };
static unsigned int inserts, erases;
static bool fail_insert;
static struct file *xa_load(struct xarray *xa, unsigned long handle)
{
    for (unsigned int i = 0; i < 128; i++)
        if (xa->files[i] && xa->handles[i] == handle) return xa->files[i];
    return NULL;
}
static int xa_insert(struct xarray *xa, unsigned long handle, struct file *file, int flags)
{
    (void)flags;
    inserts++;
    if (fail_insert) return -ENOMEM;
    if (xa_load(xa, handle)) return -EBUSY;
    for (unsigned int i = 0; i < 128; i++)
        if (!xa->files[i]) { xa->handles[i] = handle; xa->files[i] = file; return 0; }
    return -ENOMEM;
}
static struct file *xa_erase(struct xarray *xa, unsigned long handle)
{
    erases++;
    for (unsigned int i = 0; i < 128; i++)
        if (xa->files[i] && xa->handles[i] == handle) {
            struct file *file = xa->files[i]; xa->files[i] = NULL; return file;
        }
    return NULL;
}
#include "../kobox2/linux-sandbox/kobox/boot/fs_handle_cache.h"

int main(void)
{
    struct xarray tree = {0};
    struct kobox_fs_handle_cache cache = {0};
    struct file files[100];
    for (unsigned long i = 1; i <= 10000; i++) {
        assert(!kobox_fs_handle_insert(&cache, &tree, i, &files[0]));
        assert(kobox_fs_handle_load(&cache, &tree, i) == &files[0]);
        if (i > 1) assert(!kobox_fs_handle_load(&cache, &tree, i - 1));
        assert(kobox_fs_handle_erase(&cache, &tree, i) == &files[0]);
        assert(!kobox_fs_handle_load(&cache, &tree, i));
    }
    assert(!inserts && !erases && !cache.file && !cache.handle);
    for (unsigned long i = 0; i < 100; i++)
        assert(!kobox_fs_handle_insert(&cache, &tree, i + 10001, &files[i]));
    assert(inserts == 99);
    for (unsigned long i = 0; i < 100; i++)
        assert(kobox_fs_handle_load(&cache, &tree, i + 10001) == &files[i]);
    /* Failed spill consumes neither reference and changes neither map nor
     * slot. The caller must not advance its handle allocator on this error. */
    fail_insert = true;
    assert(kobox_fs_handle_insert(&cache, &tree, 10101, &files[0]) == -ENOMEM);
    assert(cache.handle == 10100 && cache.file == &files[99]);
    assert(!xa_load(&tree, 10100));
    assert(!kobox_fs_handle_load(&cache, &tree, 10101));
    fail_insert = false;
    for (unsigned long i = 0; i < 100; i++)
        assert(kobox_fs_handle_erase(&cache, &tree, i + 10001) == &files[i]);
    assert(!cache.file);
    /* A dup is a separate handle even when both entries own the same OFD. */
    assert(!kobox_fs_handle_insert(&cache, &tree, ULONG_MAX - 1, &files[0]));
    assert(!kobox_fs_handle_insert(&cache, &tree, ULONG_MAX, &files[0]));
    assert(kobox_fs_handle_erase(&cache, &tree, ULONG_MAX - 1) == &files[0]);
    assert(kobox_fs_handle_load(&cache, &tree, ULONG_MAX) == &files[0]);
    assert(kobox_fs_handle_erase(&cache, &tree, ULONG_MAX) == &files[0]);
    assert(!cache.file);
    puts("KOBOX2_FS_HANDLE_CACHE_UNIT_OK");
}

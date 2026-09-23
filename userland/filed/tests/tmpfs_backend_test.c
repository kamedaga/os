#include "filed/tmpfs_internal.h"

#include <stdio.h>
#include <string.h>

static int failures;

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
        fprintf(stderr, "%s: got %llu expected %llu\n",
            name,
            (unsigned long long)got,
            (unsigned long long)expected);
        ++failures;
    }
}

static void expect_bytes(const char *name, const unsigned char *got, const unsigned char *expected, size_t len)
{
    if (memcmp(got, expected, len) != 0) {
        fprintf(stderr, "%s: bytes differ\n", name);
        ++failures;
    }
}

static void test_create_sparse_truncate_release(void)
{
    static filed_tmpfs_backend_t tmpfs;
    uint64_t root = 0;
    uint64_t file = 0;
    uint64_t lookup = 0;
    uint64_t bytes = 0;
    unsigned char read_buf[8];
    const unsigned char expected[6] = {0, 0, 0, 0, 'o', 'k'};
    storage_statx_reply_t stat;

    filed_tmpfs_backend_init(&tmpfs);
    expect_int("mount root", filed_tmpfs_backend_mount_root(&tmpfs, &root), 0);
    expect_int("create", filed_tmpfs_backend_create(&tmpfs, root, "file", 0644, &file), 0);
    expect_int("sparse write", filed_tmpfs_backend_pwrite(&tmpfs, file, 4, "ok", 2, &bytes), 0);
    expect_u64("sparse write bytes", bytes, 2);
    memset(read_buf, 0xaa, sizeof(read_buf));
    expect_int("sparse read", filed_tmpfs_backend_pread(&tmpfs, file, 0, read_buf, sizeof(read_buf), &bytes), 0);
    expect_u64("sparse read bytes", bytes, 6);
    expect_bytes("sparse read data", read_buf, expected, sizeof(expected));
    memset(&stat, 0, sizeof(stat));
    expect_int("stat after write", filed_tmpfs_backend_statx(&tmpfs, file, &stat), 0);
    expect_u64("size after write", stat.size, 6);
    expect_u64("blocks after sparse write", stat.blocks, FILED_TMPFS_PAGE_BYTES / 512u);
    const uint64_t high_offset = 128u * 1024u * 1024u;
    expect_int("high sparse write", filed_tmpfs_backend_pwrite(&tmpfs, file, high_offset, "z", 1, &bytes), 0);
    memset(read_buf, 0xaa, sizeof(read_buf));
    expect_int("high sparse read", filed_tmpfs_backend_pread(&tmpfs, file, high_offset - 1u, read_buf, 2, &bytes), 0);
    expect_u64("high sparse read bytes", bytes, 2);
    expect_bytes("high sparse read data", read_buf, (const unsigned char *)"\0z", 2);
    expect_int("large sparse truncate", filed_tmpfs_backend_truncate(&tmpfs, file, FILED_TMPFS_MAX_FILE_BYTES), 0);
    expect_int("truncate", filed_tmpfs_backend_truncate(&tmpfs, file, 1), 0);
    memset(&stat, 0, sizeof(stat));
    expect_int("stat after truncate", filed_tmpfs_backend_statx(&tmpfs, file, &stat), 0);
    expect_u64("size after truncate", stat.size, 1);
    expect_int("rewrite freed page", filed_tmpfs_backend_pwrite(&tmpfs, file, FILED_TMPFS_PAGE_BYTES, "x", 1, &bytes), 0);
    expect_int("unlink", filed_tmpfs_backend_unlink(&tmpfs, root, "file"), 0);
    expect_int("lookup unlinked", filed_tmpfs_backend_lookup(&tmpfs, root, "file", &lookup), -2);
    memset(read_buf, 0xaa, sizeof(read_buf));
    expect_int("read unlinked before release", filed_tmpfs_backend_pread(&tmpfs, file, 0, read_buf, 1, &bytes), 0);
    expect_u64("read unlinked bytes", bytes, 1);
    expect_int("release", filed_tmpfs_backend_release_object(&tmpfs, file), 0);
    expect_int("stat released", filed_tmpfs_backend_statx(&tmpfs, file, &stat), -2);
}

static void test_directory_rules_and_rename(void)
{
    static filed_tmpfs_backend_t tmpfs;
    uint64_t root = 0;
    uint64_t dir = 0;
    uint64_t child = 0;
    uint64_t renamed = 0;

    filed_tmpfs_backend_init(&tmpfs);
    expect_int("mount root 2", filed_tmpfs_backend_mount_root(&tmpfs, &root), 0);
    expect_int("mkdir", filed_tmpfs_backend_mkdir(&tmpfs, root, "dir", 0755, &dir), 0);
    expect_int("create child", filed_tmpfs_backend_create(&tmpfs, dir, "child", 0644, &child), 0);
    expect_int("rmdir nonempty", filed_tmpfs_backend_rmdir(&tmpfs, root, "dir"), -39);
    expect_int("rename child", filed_tmpfs_backend_rename(&tmpfs, dir, "child", root, "moved", &renamed), 0);
    expect_u64("renamed object", renamed, child);
    expect_int("lookup old child", filed_tmpfs_backend_lookup(&tmpfs, dir, "child", &renamed), -2);
    expect_int("lookup moved", filed_tmpfs_backend_lookup(&tmpfs, root, "moved", &renamed), 0);
    expect_int("rmdir empty", filed_tmpfs_backend_rmdir(&tmpfs, root, "dir"), 0);
}

static void test_hardlink_lifetime_and_symlink_alias(void)
{
    static filed_tmpfs_backend_t tmpfs;
    uint64_t root = 0;
    uint64_t file = 0;
    uint64_t alias = 0;
    uint64_t lookup = 0;
    uint64_t bytes = 0;
    unsigned char buf[4] = {0};
    storage_statx_reply_t stat;
    uint64_t symlink = 0;
    uint64_t symlink_alias = 0;
    char target[16];
    char long_target[256];
    char long_read[sizeof(long_target)];
    uint64_t target_len = 0;
    uint64_t dir = 0;

    filed_tmpfs_backend_init(&tmpfs);
    expect_int("mount hardlink", filed_tmpfs_backend_mount_root(&tmpfs, &root), 0);
    expect_int("create hardlink source", filed_tmpfs_backend_create(&tmpfs, root, "file", 0644, &file), 0);
    expect_int("write hardlink source", filed_tmpfs_backend_pwrite(&tmpfs, file, 0, "ok", 2, &bytes), 0);
    expect_int("hardlink file", filed_tmpfs_backend_link(&tmpfs, file, root, "alias", &alias), 0);
    expect_u64("hardlink same object", alias, file);
    memset(&stat, 0, sizeof(stat));
    expect_int("stat hardlink nlink", filed_tmpfs_backend_statx(&tmpfs, file, &stat), 0);
    expect_u64("hardlink inode identity", stat.inode_number, file);
    expect_u64("hardlink nlink 2", stat.nlink, 2);
    expect_int("lookup alias", filed_tmpfs_backend_lookup(&tmpfs, root, "alias", &lookup), 0);
    expect_u64("lookup alias object", lookup, file);
    expect_int("rename hardlink onto alias no-op", filed_tmpfs_backend_rename(&tmpfs, root, "file", root, "alias", &lookup), 0);
    expect_u64("rename hardlink no-op object", lookup, file);
    expect_int("lookup original after hardlink rename no-op", filed_tmpfs_backend_lookup(&tmpfs, root, "file", &lookup), 0);
    expect_u64("lookup original no-op object", lookup, file);
    expect_int("lookup alias after hardlink rename no-op", filed_tmpfs_backend_lookup(&tmpfs, root, "alias", &lookup), 0);
    expect_u64("lookup alias no-op object", lookup, file);
    expect_int("unlink original hardlink", filed_tmpfs_backend_unlink(&tmpfs, root, "file"), 0);
    expect_int("lookup original gone", filed_tmpfs_backend_lookup(&tmpfs, root, "file", &lookup), -2);
    memset(&stat, 0, sizeof(stat));
    expect_int("stat alias nlink", filed_tmpfs_backend_statx(&tmpfs, alias, &stat), 0);
    expect_u64("hardlink nlink 1", stat.nlink, 1);
    expect_int("read alias", filed_tmpfs_backend_pread(&tmpfs, alias, 0, buf, 2, &bytes), 0);
    expect_bytes("read alias data", buf, (const unsigned char *)"ok", 2);
    expect_int("unlink alias", filed_tmpfs_backend_unlink(&tmpfs, root, "alias"), 0);
    expect_int("read last unlinked before release", filed_tmpfs_backend_pread(&tmpfs, alias, 0, buf, 2, &bytes), 0);
    expect_int("release last hardlink", filed_tmpfs_backend_release_object(&tmpfs, alias), 0);
    expect_int("stat released hardlink", filed_tmpfs_backend_statx(&tmpfs, alias, &stat), -2);

    expect_int("mkdir hardlink reject source", filed_tmpfs_backend_mkdir(&tmpfs, root, "dir", 0755, &dir), 0);
    expect_int("hardlink directory rejected", filed_tmpfs_backend_link(&tmpfs, dir, root, "dir-link", &lookup), -1);
    expect_int("hardlink missing source", filed_tmpfs_backend_link(&tmpfs, 0x1234u, root, "missing-link", &lookup), -2);

    expect_int("create symlink source", filed_tmpfs_backend_symlink(&tmpfs, root, "sym", "file-target", 11, &symlink), 0);
    expect_int("hardlink symlink", filed_tmpfs_backend_link(&tmpfs, symlink, root, "sym-hard", &symlink_alias), 0);
    expect_u64("hardlink symlink object", symlink_alias, symlink);
    memset(target, 0, sizeof(target));
    expect_int("readlink symlink alias", filed_tmpfs_backend_readlink(&tmpfs, symlink_alias, target, sizeof(target), &target_len), 0);
    expect_u64("readlink symlink alias len", target_len, 11);
    expect_bytes("readlink symlink alias data", (const unsigned char *)target, (const unsigned char *)"file-target", 11);
    expect_int("hardlink existing destination", filed_tmpfs_backend_link(&tmpfs, symlink, root, "sym-hard", &lookup), -17);

    for (size_t i = 0; i < sizeof(long_target) - 1u; ++i) {
        long_target[i] = (char)('a' + (i % 26u));
    }
    long_target[sizeof(long_target) - 1u] = '\0';
    expect_int(
        "create long symlink",
        filed_tmpfs_backend_symlink(&tmpfs, root, "long-sym", long_target, sizeof(long_target) - 1u, &symlink),
        0);
    memset(long_read, 0, sizeof(long_read));
    expect_int("readlink long symlink", filed_tmpfs_backend_readlink(&tmpfs, symlink, long_read, sizeof(long_read), &target_len), 0);
    expect_u64("readlink long len", target_len, sizeof(long_target) - 1u);
    expect_bytes("readlink long data", (const unsigned char *)long_read, (const unsigned char *)long_target, sizeof(long_target) - 1u);
}

static void test_executable_sized_file(void)
{
    static filed_tmpfs_backend_t tmpfs;
    uint64_t root = 0;
    uint64_t file = 0;
    uint64_t bytes = 0;
    unsigned char page[FILED_TMPFS_PAGE_BYTES];
    unsigned char read_page[FILED_TMPFS_PAGE_BYTES];
    storage_statx_reply_t stat;
    const uint64_t file_size = FILED_TMPFS_PAGE_BYTES * 257u;

    filed_tmpfs_backend_init(&tmpfs);
    expect_int("mount root large", filed_tmpfs_backend_mount_root(&tmpfs, &root), 0);
    expect_int("create large", filed_tmpfs_backend_create(&tmpfs, root, "busybox", 0755, &file), 0);
    for (size_t i = 0; i < sizeof(page); ++i) {
        page[i] = (unsigned char)(i & 0xffu);
    }
    for (uint64_t offset = 0; offset < file_size; offset += sizeof(page)) {
        expect_int("write large page", filed_tmpfs_backend_pwrite(&tmpfs, file, offset, page, sizeof(page), &bytes), 0);
        expect_u64("write large bytes", bytes, sizeof(page));
    }
    memset(&stat, 0, sizeof(stat));
    expect_int("stat large", filed_tmpfs_backend_statx(&tmpfs, file, &stat), 0);
    expect_u64("large size", stat.size, file_size);
    memset(read_page, 0, sizeof(read_page));
    expect_int(
        "read large tail",
        filed_tmpfs_backend_pread(&tmpfs, file, file_size - sizeof(read_page), read_page, sizeof(read_page), &bytes),
        0);
    expect_u64("read large tail bytes", bytes, sizeof(read_page));
    expect_bytes("read large tail data", read_page, page, sizeof(page));
    expect_int(
        "write too large",
        filed_tmpfs_backend_pwrite(&tmpfs, file, FILED_TMPFS_MAX_FILE_BYTES, page, 1, &bytes),
        -27);
}

static void test_backend_instances_are_isolated(void)
{
    static filed_tmpfs_backend_t first;
    static filed_tmpfs_backend_t second;
    uint64_t first_root = 0;
    uint64_t second_root = 0;
    uint64_t first_file = 0;
    uint64_t second_lookup = 0;
    uint64_t bytes = 0;
    unsigned char buf[2] = {0};

    filed_tmpfs_backend_init(&first);
    filed_tmpfs_backend_init(&second);
    expect_int("mount first", filed_tmpfs_backend_mount_root(&first, &first_root), 0);
    expect_int("mount second", filed_tmpfs_backend_mount_root(&second, &second_root), 0);
    expect_int("create isolated", filed_tmpfs_backend_create(&first, first_root, "only-first", 0644, &first_file), 0);
    expect_int("write isolated", filed_tmpfs_backend_pwrite(&first, first_file, 0, "x", 1, &bytes), 0);
    expect_int("lookup isolated miss", filed_tmpfs_backend_lookup(&second, second_root, "only-first", &second_lookup), -2);
    expect_int("cross backend stale object miss", filed_tmpfs_backend_pread(&second, first_file, 0, buf, sizeof(buf), &bytes), -2);
}

static void test_rename_replace_and_getdents_offsets(void)
{
    static filed_tmpfs_backend_t tmpfs;
    uint64_t root = 0;
    uint64_t a = 0;
    uint64_t b = 0;
    uint64_t out = 0;
    uint64_t bytes = 0;
    unsigned char buf[2] = {0};
    storage_getdents_request_t dents;

    filed_tmpfs_backend_init(&tmpfs);
    expect_int("mount rename replace", filed_tmpfs_backend_mount_root(&tmpfs, &root), 0);
    expect_int("create a", filed_tmpfs_backend_create(&tmpfs, root, "a", 0644, &a), 0);
    expect_int("create b", filed_tmpfs_backend_create(&tmpfs, root, "b", 0644, &b), 0);
    expect_int("write a", filed_tmpfs_backend_pwrite(&tmpfs, a, 0, "A", 1, &bytes), 0);
    expect_int("write b", filed_tmpfs_backend_pwrite(&tmpfs, b, 0, "B", 1, &bytes), 0);
    expect_int("rename replace", filed_tmpfs_backend_rename(&tmpfs, root, "a", root, "b", &out), 0);
    expect_u64("rename replace object", out, a);
    expect_int("old a gone", filed_tmpfs_backend_lookup(&tmpfs, root, "a", &out), -2);
    expect_int("new b exists", filed_tmpfs_backend_lookup(&tmpfs, root, "b", &out), 0);
    expect_u64("new b object", out, a);
    expect_int("read replaced", filed_tmpfs_backend_pread(&tmpfs, out, 0, buf, 1, &bytes), 0);
    expect_u64("read replaced bytes", bytes, 1);
    expect_bytes("read replaced data", buf, (const unsigned char *)"A", 1);
    expect_int("release replaced target", filed_tmpfs_backend_release_object(&tmpfs, b), 0);

    expect_int("mkdir d", filed_tmpfs_backend_mkdir(&tmpfs, root, "d", 0755, &out), 0);
    memset(&dents, 0, sizeof(dents));
    expect_int("getdents root", filed_tmpfs_backend_getdents(&tmpfs, root, 0, &dents), 0);
    expect_u64("getdents root count", dents.count, 2);
    memset(&dents, 0, sizeof(dents));
    expect_int("getdents root offset", filed_tmpfs_backend_getdents(&tmpfs, root, 1, &dents), 0);
    expect_u64("getdents offset count", dents.count, 1);
}

static void test_metadata_capacity_and_reuse(void)
{
    static filed_tmpfs_backend_t tmpfs;
    uint64_t root = 0, extra = 0;
    uint64_t ids[FILED_TMPFS_MAX_INODES];
    storage_statfs_reply_t before, full, after;
    storage_statx_reply_t stat;
    filed_tmpfs_backend_init(&tmpfs);
    expect_int("capacity root", filed_tmpfs_backend_mount_root(&tmpfs, &root), 0);
    expect_int("capacity before", filed_tmpfs_backend_statfs(&tmpfs, &before), 0);
    /* Reserve enough distinct live objects for the observed desktop load. */
    expect_int("desktop metadata headroom", before.files_free >= 180, 1);
    for (uint64_t i = 0; i < before.files_free; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "capacity-%llu", (unsigned long long)i);
        expect_int("capacity create", filed_tmpfs_backend_create(&tmpfs, root, name, 0600, &ids[i]), 0);
    }
    expect_int("capacity full", filed_tmpfs_backend_statfs(&tmpfs, &full), 0);
    expect_u64("no free inodes", full.files_free, 0);
    expect_u64("metadata does not allocate data pages", full.blocks_free, before.blocks_free);
    expect_int("bounded exhaustion", filed_tmpfs_backend_create(&tmpfs, root, "overflow", 0600, &extra), -28);
    for (uint64_t i = 0; i < before.files_free; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "capacity-%llu", (unsigned long long)i);
        expect_int("capacity unlink", filed_tmpfs_backend_unlink(&tmpfs, root, name), 0);
        expect_int("unlinked retained until release", filed_tmpfs_backend_statx(&tmpfs, ids[i], &stat), 0);
        expect_int("capacity release", filed_tmpfs_backend_release_object(&tmpfs, ids[i]), 0);
        expect_int("released object stale", filed_tmpfs_backend_statx(&tmpfs, ids[i], &stat), -2);
    }
    expect_int("capacity after", filed_tmpfs_backend_statfs(&tmpfs, &after), 0);
    expect_u64("all inodes recovered", after.files_free, before.files_free);
    expect_int("reuse after exhaustion", filed_tmpfs_backend_create(&tmpfs, root, "reuse", 0600, &extra), 0);
    expect_int("old generation remains stale", filed_tmpfs_backend_statx(&tmpfs, ids[before.files_free - 1], &stat), -2);
}

static void test_zero_writes_preserve_holes(void)
{
    static filed_tmpfs_backend_t tmpfs;
    uint64_t root = 0, file = 0, bytes = 0;
    unsigned char zeros[FILED_TMPFS_PAGE_BYTES] = {0};
    unsigned char readback[FILED_TMPFS_PAGE_BYTES];
    storage_statx_reply_t stat;
    const uint64_t length = 24u * 1024u * 1024u;
    filed_tmpfs_backend_init(&tmpfs);
    expect_int("zero root", filed_tmpfs_backend_mount_root(&tmpfs, &root), 0);
    expect_int("zero file", filed_tmpfs_backend_create(&tmpfs, root, "zero", 0600, &file), 0);
    const uint32_t free_before = tmpfs.free_page_count;
    for (uint64_t offset = 0; offset < length; offset += sizeof(zeros)) {
        int status = filed_tmpfs_backend_pwrite(&tmpfs, file, offset, zeros, sizeof(zeros), &bytes);
        if (status) { expect_int("zero write beyond pool capacity", status, 0); break; }
        expect_u64("zero write bytes", bytes, sizeof(zeros));
    }
    expect_u64("zero writes do not consume backing", tmpfs.free_page_count, free_before);
    expect_int("zero stat", filed_tmpfs_backend_statx(&tmpfs, file, &stat), 0);
    expect_u64("zero logical size", stat.size, length);
    expect_u64("zero allocated blocks", stat.blocks, 0);
    expect_int("zero tail read", filed_tmpfs_backend_pread(&tmpfs, file,
        length - sizeof(readback), readback, sizeof(readback), &bytes), 0);
    expect_u64("zero tail bytes", bytes, sizeof(readback));
    expect_bytes("zero tail contents", readback, zeros, sizeof(zeros));
    expect_int("zero nonzero write", filed_tmpfs_backend_pwrite(&tmpfs, file, 10, "abc", 3, &bytes), 0);
    expect_int("zero partial overwrite", filed_tmpfs_backend_pwrite(&tmpfs, file, 11, zeros, 1, &bytes), 0);
    expect_int("zero neighbors read", filed_tmpfs_backend_pread(&tmpfs, file, 10, readback, 3, &bytes), 0);
    expect_bytes("zero preserves neighbors", readback, (const unsigned char *)"a\0c", 3);
    expect_u64("zero partial allocates only data page", tmpfs.free_page_count, free_before - 1);
    expect_int("zero partial hole extends", filed_tmpfs_backend_pwrite(&tmpfs, file,
        length + 17, zeros, 5, &bytes), 0);
    expect_int("zero extended stat", filed_tmpfs_backend_statx(&tmpfs, file, &stat), 0);
    expect_u64("zero partial logical size", stat.size, length + 22);
    expect_u64("zero partial hole remains sparse", tmpfs.free_page_count, free_before - 1);
}

static void test_multiple_large_files_reclaim_pages(void)
{
    static filed_tmpfs_backend_t tmpfs;
    enum { PAGES_PER_FILE = 5120, FILES = 2 };
    unsigned char page[FILED_TMPFS_PAGE_BYTES];
    unsigned char readback[FILED_TMPFS_PAGE_BYTES];
    uint64_t files[FILES] = {0}, bytes;
    filed_tmpfs_backend_init(&tmpfs);
    const uint32_t initial_free = tmpfs.free_page_count;
    for (unsigned i = 0; i < FILED_TMPFS_PAGE_POOL_PAGES; ++i)
        expect_int("no backing allocation at init", tmpfs.pages[i].data == NULL, 1);
    if (initial_free < PAGES_PER_FILE * FILES) {
        expect_int("two 20 MiB files fit the tmpfs budget", 0, 1);
        return;
    }
    uint64_t root = filed_tmpfs_backend_root_object(&tmpfs);
    for (unsigned f = 0; f < FILES; ++f) {
        expect_int("large concurrent create", filed_tmpfs_backend_create(
            &tmpfs, root, f ? "second" : "first", 0600, &files[f]), 0);
        memset(page, 0x51 + f, sizeof(page));
        for (unsigned i = 0; i < PAGES_PER_FILE; ++i) {
            int status = filed_tmpfs_backend_pwrite(
                &tmpfs, files[f], (uint64_t)i * sizeof(page), page, sizeof(page), &bytes);
            expect_int("large concurrent write", status, 0);
            if (status) goto reclaim;
            expect_u64("large concurrent write size", bytes, sizeof(page));
        }
        expect_int("large concurrent read", filed_tmpfs_backend_pread(
            &tmpfs, files[f], (PAGES_PER_FILE - 1) * sizeof(page), readback, sizeof(readback), &bytes), 0);
        expect_bytes("large concurrent data", readback, page, sizeof(page));
    }
    expect_u64("only actual pages charged", tmpfs.free_page_count,
        initial_free - PAGES_PER_FILE * FILES);
    /* Drop the newer head entries and retain the older tail, then reuse
     * returned pool slots without exposing bytes from the other inode. */
    expect_int("large partial truncate", filed_tmpfs_backend_truncate(
        &tmpfs, files[0], FILED_TMPFS_PAGE_BYTES + 7), 0);
    expect_int("large partial read", filed_tmpfs_backend_pread(
        &tmpfs, files[0], FILED_TMPFS_PAGE_BYTES, readback, 7, &bytes), 0);
    memset(page, 0x51, sizeof(page));
    expect_bytes("large retained tail", readback, page, 7);
reclaim:
    for (unsigned f = 0; f < FILES; ++f)
        if (files[f]) expect_int("large concurrent truncate", filed_tmpfs_backend_truncate(&tmpfs, files[f], 0), 0);
    expect_u64("all backing pages reclaimed", tmpfs.free_page_count, initial_free);
    for (unsigned i = 0; i < FILED_TMPFS_PAGE_POOL_PAGES; ++i)
        expect_int("no retained page after truncate", tmpfs.pages[i].used, 0);
    for (unsigned i = 0; i < FILED_TMPFS_PAGE_POOL_PAGES; ++i)
        expect_int("backing memory freed after truncate", tmpfs.pages[i].data == NULL, 1);
}

static void test_copy_present_leaves_holes_untouched(void)
{
    static filed_tmpfs_backend_t tmpfs;
    uint64_t root = 0, file = 0, bytes = 0;
    unsigned char imported[3 * FILED_TMPFS_PAGE_BYTES + 7];
    filed_tmpfs_backend_init(&tmpfs);
    expect_int("import mount", filed_tmpfs_backend_mount_root(&tmpfs, &root), 0);
    expect_int("import create", filed_tmpfs_backend_create(&tmpfs, root, "sparse", 0644, &file), 0);
    expect_int("import first page", filed_tmpfs_backend_pwrite(&tmpfs, file, 3, "abc", 3, &bytes), 0);
    expect_int("import last page", filed_tmpfs_backend_pwrite(&tmpfs, file, sizeof(imported) - 2, "xy", 2, &bytes), 0);
    const uint32_t free_before = tmpfs.free_page_count;
    /* Sentinel holes prove no read/zero/write of those destination pages. */
    memset(imported, 0xa5, sizeof(imported));
    expect_int("import present", filed_tmpfs_backend_copy_present(&tmpfs, file, imported, sizeof(imported)), 0);
    expect_bytes("import first bytes", imported + 3, (const unsigned char *)"abc", 3);
    expect_bytes("import last bytes", imported + sizeof(imported) - 2, (const unsigned char *)"xy", 2);
    expect_int("import allocated page zeros", imported[0], 0);
    expect_int("import last partial page zeros", imported[3 * FILED_TMPFS_PAGE_BYTES], 0);
    for (size_t i = FILED_TMPFS_PAGE_BYTES; i < 3 * FILED_TMPFS_PAGE_BYTES; ++i)
        expect_int("import hole untouched", imported[i], 0xa5);
    expect_u64("import no backend allocation", tmpfs.free_page_count, free_before);
    expect_int("import rejects directory", filed_tmpfs_backend_copy_present(&tmpfs, root, imported, 1), -22);
    expect_int("import rejects past EOF", filed_tmpfs_backend_copy_present(&tmpfs, file, imported, sizeof(imported) + 1), -22);
    expect_int("import cleanup", filed_tmpfs_backend_truncate(&tmpfs, file, 0), 0);
    expect_u64("import pages freed", tmpfs.free_page_count, free_before + 2);
}

int main(void)
{
    test_create_sparse_truncate_release();
    test_directory_rules_and_rename();
    test_hardlink_lifetime_and_symlink_alias();
    test_executable_sized_file();
    test_backend_instances_are_isolated();
    test_rename_replace_and_getdents_offsets();
    test_metadata_capacity_and_reuse();
    test_zero_writes_preserve_holes();
    test_multiple_large_files_reclaim_pages();
    test_copy_present_leaves_holes_untouched();
    if (failures != 0) {
        return 1;
    }
    printf("tmpfs backend tests passed\n");
    return 0;
}

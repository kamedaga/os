#include "filed/live_bootfs.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

static filed_tmpfs_backend_t tmpfs;

static void put16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static void put64(unsigned char *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

int main(void)
{
    static const char marker[] = "/etc/pacha/boot-profile";
    static const char shell[] = "/bin/ash";
    unsigned char archive[256] = {0};
    const uint32_t marker_len = sizeof(marker) - 1u;
    const uint32_t shell_len = sizeof(shell) - 1u;
    put32(archive, 0x53465442u);
    put16(archive + 4, 1);
    put16(archive + 6, 104);
    put64(archive + 8, sizeof(archive));
    put32(archive + 16, 2);
    put32(archive + 20, 96);
    put64(archive + 24, 104);
    put64(archive + 32, 200);
    put64(archive + 40, marker_len + shell_len);
    put16(archive + 104 + 4, marker_len);
    archive[104 + 6] = 1;
    put64(archive + 104 + 8, 240);
    put64(archive + 104 + 16, 1);
    put32(archive + 104 + 24, 0444);
    put32(archive + 152, marker_len);
    put16(archive + 152 + 4, shell_len);
    archive[152 + 6] = 1;
    put64(archive + 152 + 8, 241);
    put64(archive + 152 + 16, 3);
    put32(archive + 152 + 24, 0555);
    memcpy(archive + 200, marker, marker_len);
    memcpy(archive + 200 + marker_len, shell, shell_len);
    memcpy(archive + 241, "ash", 3);

    filed_tmpfs_backend_init(&tmpfs);
    uint32_t files = 0;
    uint64_t bytes = 0;
    assert(filed_live_bootfs_populate(&tmpfs, archive, sizeof(archive),
        &files, &bytes) == 0);
    assert(files == 2 && bytes == 4);
    uint64_t root = filed_tmpfs_backend_root_object(&tmpfs);
    uint64_t bin = 0, ash = 0, dev = 0, tmp = 0, run = 0, shm = 0;
    assert(filed_tmpfs_backend_lookup(&tmpfs, root, "bin", &bin) == 0);
    assert(filed_tmpfs_backend_lookup(&tmpfs, bin, "ash", &ash) == 0);
    assert(filed_tmpfs_backend_lookup(&tmpfs, root, "dev", &dev) == 0);
    assert(filed_tmpfs_backend_lookup(&tmpfs, root, "tmp", &tmp) == 0);
    assert(filed_tmpfs_backend_lookup(&tmpfs, root, "run", &run) == 0);
    assert(filed_tmpfs_backend_lookup(&tmpfs, dev, "shm", &shm) == 0);
    storage_statx_reply_t stat;
    assert(filed_tmpfs_backend_statx(&tmpfs, ash, &stat) == 0);
    assert(stat.mode == (0100000u | 0555u) && stat.size == 3);
    char data[4] = {0};
    uint64_t read = 0;
    assert(filed_tmpfs_backend_pread(&tmpfs, ash, 0, data, 3, &read) == 0);
    assert(read == 3 && memcmp(data, "ash", 3) == 0);
    uint64_t etc = 0, pacha = 0, missing = 0;
    assert(filed_tmpfs_backend_lookup(&tmpfs, root, "etc", &etc) == 0);
    assert(filed_tmpfs_backend_lookup(&tmpfs, etc, "pacha", &pacha) == 0);
    assert(filed_tmpfs_backend_lookup(&tmpfs, root, "missing", &missing) == -2);

    /* The larger USB live root is carried as one bounded zlib payload while
     * the kernel-staged outer archive remains within the boot scratch budget. */
    static const char packed_path[] = "/some/other-name.bin";
    unsigned char outer[1024] = {0};
    const uint64_t data_offset = 192;
    uLongf compressed_size = sizeof(outer) - data_offset - 16;
    assert(compress2(outer + data_offset + 16, &compressed_size,
        archive, sizeof(archive), Z_BEST_COMPRESSION) == Z_OK);
    const uint64_t outer_size = data_offset + 16 + compressed_size;
    put32(outer, 0x53465442u);
    put16(outer + 4, 1);
    put16(outer + 6, 104);
    put64(outer + 8, outer_size);
    put32(outer + 16, 1);
    put32(outer + 20, 48);
    put64(outer + 24, 104);
    put64(outer + 32, 152);
    put64(outer + 40, sizeof(packed_path) - 1u);
    put16(outer + 104 + 4, sizeof(packed_path) - 1u);
    outer[104 + 6] = 1;
    put64(outer + 104 + 8, data_offset);
    put64(outer + 104 + 16, 16 + compressed_size);
    put32(outer + 104 + 24, 0444);
    memcpy(outer + 152, packed_path, sizeof(packed_path) - 1u);
    memcpy(outer + data_offset, "PACHAZ1", 8);
    put64(outer + data_offset + 8, sizeof(archive));
    filed_tmpfs_backend_init(&tmpfs);
    assert(filed_live_bootfs_populate(&tmpfs, outer, outer_size,
        &files, &bytes) == 0);
    assert(files == 2 && bytes == 4);
    outer[outer_size - 1u] ^= 0x55;
    filed_tmpfs_backend_init(&tmpfs);
    assert(filed_live_bootfs_populate(&tmpfs, outer, outer_size,
        &files, &bytes) == -22);
    outer[outer_size - 1u] ^= 0x55;
    put64(outer + data_offset + 8, 64u * 1024u * 1024u + 1u);
    filed_tmpfs_backend_init(&tmpfs);
    assert(filed_live_bootfs_populate(&tmpfs, outer, outer_size,
        &files, &bytes) == -22);

    /* Reject traversal before creating any file from an untrusted archive. */
    memcpy(archive + 200 + marker_len, "/../ash", 7);
    filed_tmpfs_backend_init(&tmpfs);
    assert(filed_live_bootfs_populate(&tmpfs, archive, sizeof(archive),
        &files, &bytes) == -22);
    puts("live bootfs tests passed");
    return 0;
}

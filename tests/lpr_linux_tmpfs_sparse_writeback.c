#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/statfs.h>
#include <sys/stat.h>
#include <unistd.h>

static int global_sync_test(void)
{
    enum { FILES = 4, SIZE = 20 * 1024 * 1024 };
    int fds[FILES] = {-1, -1, -1, -1};
    unsigned char *maps[FILES] = {0};
    int result = 1;
    for (int i = 0; i < FILES; ++i) {
        fds[i] = memfd_create("global-sync-memory", MFD_CLOEXEC);
        if (fds[i] < 0 || ftruncate(fds[i], SIZE)) goto done;
        maps[i] = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
        if (maps[i] == MAP_FAILED) { maps[i] = NULL; goto done; }
        memset(maps[i], 0x41 + i, SIZE);
    }
    sync();
    for (int i = 0; i < FILES; ++i) {
        unsigned char buffer[4096];
        for (size_t offset = 0; offset < SIZE; offset += sizeof(buffer)) {
            if (pread(fds[i], buffer, sizeof(buffer), offset) != sizeof(buffer)) goto done;
            for (size_t j = 0; j < sizeof(buffer); ++j)
                if (buffer[j] != 0x41 + i || maps[i][offset + j] != 0x41 + i) goto done;
        }
    }
    puts("GLOBAL_SYNC=PASS 4x20MiB mapped memfds coherent after sync");
    result = 0;
done:
    if (result) perror("global-sync");
    for (int i = 0; i < FILES; ++i) {
        if (maps[i]) munmap(maps[i], SIZE);
        if (fds[i] >= 0) close(fds[i]);
    }
    sync();
    return result;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "global-sync") == 0) return global_sync_test();
    const int dense = argc > 1 && strcmp(argv[1], "dense") == 0;
    const size_t size = 24u * 1024u * 1024u;
    struct statfs before, after;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (statfs("/dev/shm", &before)) return 1;
    int fd = memfd_create("sparse-writeback", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size)) { perror("create"); return 2; }
    unsigned char *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 3; }
    p[0] = 42;
    p[size - 1] = 99;
    if (dense) memset(p, 0x5a, size);
    if (msync(p, size, MS_SYNC) || fsync(fd)) { perror("writeback"); return 4; }
    unsigned char value = 0;
    if (pread(fd, &value, 1, size - 1) != 1 ||
        value != (dense ? 0x5a : 99) || p[0] != (dense ? 0x5a : 42)) return 5;
    if (statfs("/dev/shm", &after)) return 6;
    printf("SPARSE_WRITEBACK size=%zu before_free=%lu after_free=%lu\n", size,
        (unsigned long)before.f_bfree, (unsigned long)after.f_bfree);
    if (!dense && before.f_bfree > after.f_bfree + 4) return 7;
    if (munmap(p, size) || close(fd)) return 8;
    puts(dense ? "DENSE_WRITEBACK=PASS" : "SPARSE_WRITEBACK=PASS");
    return 0;
}

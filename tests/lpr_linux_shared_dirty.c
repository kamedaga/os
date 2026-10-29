#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

enum { PAGE = 4096, PAGES = 513 };
static const char path[] = "/var/cache/capos-shared-dirty-20261027.bin";
static unsigned char *mapping;
#define CHECK(x) do { if (!(x)) { printf("SHARED_DIRTY=FAIL line=%u errno=%d\n",__LINE__,errno); return 1; } } while (0)
static void *writer(void *unused)
{
    (void)unused;
    for (unsigned round = 0; round < 32; ++round) {
        memset(mapping + 128 * PAGE, 0x40 + round, PAGE);
        memset(mapping + 512 * PAGE, 0x80 + round, PAGE);
        __sync_synchronize();
    }
    return NULL;
}
static int check_page(int fd, unsigned index, unsigned char value)
{
    unsigned char bytes[PAGE];
    if (pread(fd, bytes, sizeof(bytes), (off_t)index * PAGE) != sizeof(bytes)) return -1;
    for (unsigned i = 0; i < sizeof(bytes); ++i) if (bytes[i] != value) return -1;
    return 0;
}
int main(int argc, char **argv)
{
    const int write_mode = argc == 2 && !strcmp(argv[1], "write");
    CHECK(write_mode || (argc == 2 && !strcmp(argv[1], "read")));
    const size_t bytes = (size_t)PAGES * PAGE;
    int fd = open(path, write_mode ? O_CREAT | O_EXCL | O_RDWR : O_RDONLY, 0600);
    CHECK(fd >= 0);
    if (write_mode) {
        CHECK(!ftruncate(fd, bytes));
        mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        CHECK(mapping != MAP_FAILED);
        unsigned char *alias = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        CHECK(alias != MAP_FAILED);
        memset(mapping, 0x33, PAGE);
        pid_t child = fork();
        CHECK(child >= 0);
        if (!child) {
            memset(alias + 64 * PAGE, 0x4d, PAGE);
            if (munmap(alias, bytes) || munmap(mapping, bytes)) _exit(2);
            _exit(0);
        }
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        pthread_t thread;
        CHECK(!pthread_create(&thread, NULL, writer, NULL));
        for (unsigned i = 0; i < 4; ++i) CHECK(!msync(alias, bytes, MS_SYNC));
        CHECK(!pthread_join(thread, NULL));
        CHECK(!msync(mapping, bytes, MS_SYNC) && !fsync(fd));
        CHECK(!munmap(alias, bytes) && !munmap(mapping, bytes));
    }
    CHECK(!check_page(fd, 0, 0x33));
    CHECK(!check_page(fd, 1, 0));
    CHECK(!check_page(fd, 64, 0x4d));
    CHECK(!check_page(fd, 128, 0x5f));
    CHECK(!check_page(fd, 512, 0x9f));
    CHECK(!close(fd));
    printf("SHARED_DIRTY_%s=PASS fork/aliases/concurrent-writeback/final-bytes\n",write_mode ? "WRITE" : "READ");
    return 0;
}

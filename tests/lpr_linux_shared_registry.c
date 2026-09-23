#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Keep >128 distinct shared file VMOs alive, including FD-less mappings.
 * Only this test's mkdtemp files are removed; no desktop/profile changes. */
int main(void)
{
    enum { COUNT = 144, BYTES = 4096, ROUNDS = 3 };
    unsigned char *maps[COUNT] = {0};
    char directory[] = "/tmp/shared-registry-XXXXXX";
    char path[128];
    unsigned created = 0;
    int result = 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!mkdtemp(directory)) { perror("mkdtemp"); return 1; }
    for (unsigned round = 0; round < ROUNDS; ++round) {
        for (unsigned i = 0; i < COUNT; ++i) {
            snprintf(path, sizeof(path), "%s/%u", directory, i);
            int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (fd < 0) {
                printf("SHARED_REGISTRY FAIL stage=open round=%u index=%u errno=%d\n", round, i, errno);
                goto cleanup;
            }
            created = i + 1;
            if (ftruncate(fd, BYTES)) { perror("ftruncate"); close(fd); goto cleanup; }
            void *p = mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            int error = errno;
            close(fd);
            if (p == MAP_FAILED) {
                printf("SHARED_REGISTRY FAIL stage=mmap round=%u index=%u errno=%d\n", round, i, error);
                goto cleanup;
            }
            maps[i] = p;
            maps[i][0] = (unsigned char)(i + round + 1);
            maps[i][BYTES - 1] = (unsigned char)(i ^ 0xa5);
        }
        for (unsigned i = 0; i < COUNT; ++i) {
            if (maps[i][0] != (unsigned char)(i + round + 1) ||
                maps[i][BYTES - 1] != (unsigned char)(i ^ 0xa5)) {
                printf("SHARED_REGISTRY FAIL stage=retained-data round=%u index=%u\n", round, i);
                goto cleanup;
            }
            snprintf(path, sizeof(path), "%s/%u", directory, i);
            int fd = open(path, O_RDONLY);
            unsigned char byte = 0;
            if (fd < 0 || pread(fd, &byte, 1, 0) != 1 || byte != maps[i][0]) {
                printf("SHARED_REGISTRY FAIL stage=reopen-data round=%u index=%u\n", round, i);
                if (fd >= 0) close(fd);
                goto cleanup;
            }
            close(fd);
        }
        printf("SHARED_REGISTRY round=%u live=%u fdless-data=PASS\n", round, COUNT);
        for (unsigned i = 0; i < COUNT; ++i) {
            if (munmap(maps[i], BYTES)) { perror("munmap"); goto cleanup; }
            maps[i] = NULL;
            snprintf(path, sizeof(path), "%s/%u", directory, i);
            if (unlink(path)) { perror("unlink"); goto cleanup; }
        }
        created = 0;
    }
    result = 0;
cleanup:
    for (unsigned i = 0; i < created; ++i) {
        if (maps[i]) munmap(maps[i], BYTES);
        snprintf(path, sizeof(path), "%s/%u", directory, i);
        unlink(path);
    }
    if (rmdir(directory)) { perror("rmdir"); result = 1; }
    printf("SHARED_REGISTRY=%s\n", result ? "FAIL" : "PASS");
    return result;
}

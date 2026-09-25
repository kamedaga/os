/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* Reproduce WebKit's virtual reservations without allocating their size in
 * RAM. Exercise distant sparse pages, protection splits, replacement and COW. */
static int probe(size_t size, int prot, int full)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *base = mmap(NULL, size, prot,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) {
        printf("LARGE_RESERVE size=%zu prot=%d FAIL errno=%d\n", size, prot, errno);
        return 1;
    }
    printf("LARGE_RESERVE size=%zu prot=%d address=%p OK\n", size, prot, (void *)base);
    if (full) {
        size_t middle = (size / 2) & ~(page - 1);
        volatile unsigned char *bytes = base;
        if (bytes[0] || bytes[middle] || bytes[size - page]) goto fail;
        bytes[0] = 11;
        bytes[middle] = 22;
        bytes[size - page] = 33;
        if (mprotect(base + middle, page, PROT_NONE) ||
            mprotect(base + middle, page, PROT_READ | PROT_WRITE)) goto fail;
        if (bytes[middle] != 22) goto fail;
        pid_t child = fork();
        if (child < 0) goto fail;
        if (!child) {
            if (bytes[0] != 11 || bytes[middle] != 22 || bytes[size - page] != 33)
                _exit(2);
            bytes[middle] = 44;
            _exit(bytes[middle] == 44 ? 0 : 3);
        }
        int status;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) || bytes[middle] != 22) goto fail;
        void *replacement = mmap(base + middle, page, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (replacement != base + middle || bytes[middle] != 0 ||
            bytes[0] != 11 || bytes[size - page] != 33) goto fail;
        puts("LARGE_RESERVE touch/protect/fork/replace OK");
    }
    if (munmap(base, size)) goto fail;
    puts("LARGE_RESERVE unmap OK");
    return 0;
fail:
    printf("LARGE_RESERVE lifecycle FAIL errno=%d\n", errno);
    (void)munmap(base, size);
    return 1;
}

int main(int argc, char **argv)
{
    int full = argc == 2 && !strcmp(argv[1], "--full");
    int failed = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    failed |= probe(UINT64_C(512) << 20, PROT_READ | PROT_WRITE, full);
    failed |= probe((UINT64_C(1) << 30) + 8192, PROT_READ | PROT_WRITE | PROT_EXEC, full);
    failed |= probe(UINT64_C(8) << 30, PROT_READ | PROT_WRITE, full);
    failed |= probe(UINT64_C(128) << 30, PROT_READ | PROT_WRITE, full);
    /* Individually small requests must also escape the original 1 GiB arena
     * when several reservations coexist, as in a WebProcess. */
    void *arenas[3] = { MAP_FAILED, MAP_FAILED, MAP_FAILED };
    for (unsigned i = 0; i < 3; ++i) {
        arenas[i] = mmap(NULL, UINT64_C(512) << 20, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (arenas[i] == MAP_FAILED) {
            printf("LARGE_RESERVE concurrent[%u] FAIL errno=%d\n", i, errno);
            failed = 1;
        } else {
            printf("LARGE_RESERVE concurrent[%u] address=%p OK\n", i, arenas[i]);
            volatile unsigned char *bytes = arenas[i];
            bytes[0] = (unsigned char)(11 + i);
            bytes[(UINT64_C(512) << 20) - 1] = (unsigned char)(21 + i);
        }
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (arenas[i] != MAP_FAILED) {
            volatile unsigned char *bytes = arenas[i];
            if (bytes[0] != 11 + i || bytes[(UINT64_C(512) << 20) - 1] != 21 + i)
                failed = 1;
            if (munmap(arenas[i], UINT64_C(512) << 20)) failed = 1;
        }
    }
    printf("LARGE_RESERVATION=%s\n", failed ? "FAIL" : "OK");
    return failed;
}

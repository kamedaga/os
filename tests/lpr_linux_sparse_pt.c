/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* 640 different 2 MiB regions require more than 512 live leaf page tables,
 * while touching only 640 data pages (2.5 MiB on x86_64). Do not replace
 * this with a dense mapping: sparse virtual placement is the regression. */
enum { slots = 640, rounds = 3 };
static const size_t stride = 2u * 1024u * 1024u;

static unsigned char marker(unsigned i, unsigned round)
{
    return (unsigned char)(1u + (i + round * 17u) % 251u);
}

static int run(unsigned round)
{
    const size_t length = slots * stride;
    unsigned char *base = mmap(NULL, length, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) { perror("sparse mmap"); return 1; }
    volatile unsigned char *bytes = base;
    int failed = 0;
    for (unsigned i = 0; i < slots; ++i) {
        if (bytes[i * stride] != 0) { failed = 1; goto done; }
        bytes[i * stride] = marker(i, round);
    }
    for (unsigned i = 0; i < slots; ++i)
        if (bytes[i * stride] != marker(i, round)) { failed = 1; goto done; }
    printf("SPARSE_PT round=%u live=%u touched_bytes=%u OK\n",
        round, slots, slots * 4096u);

    /* Protection changes must preserve pages on both sides of the former
     * inline PT limit. Retouch forces any revoked translations to return. */
    if (mprotect(base, length, PROT_NONE) ||
        mprotect(base, length, PROT_READ | PROT_WRITE)) {
        perror("sparse mprotect"); failed = 1; goto done;
    }
    for (unsigned i = 0; i < slots; ++i)
        if (bytes[i * stride] != marker(i, round)) { failed = 1; goto done; }

    pid_t child = fork();
    if (child < 0) { perror("sparse fork"); failed = 1; goto done; }
    if (child == 0) {
        for (unsigned i = 0; i < slots; ++i) {
            if (bytes[i * stride] != marker(i, round)) _exit(2);
            bytes[i * stride] ^= 0xff;
            if (bytes[i * stride] != (unsigned char)(marker(i, round) ^ 0xffu))
                _exit(3);
        }
        _exit(0);
    }
    int status = 0;
    errno = 0;
    pid_t waited = waitpid(child, &status, 0);
    int wait_errno = errno;
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "SPARSE_PT child failed child=%ld waited=%ld errno=%d status=%d\n",
            (long)child, (long)waited, wait_errno, status);
        failed = 1; goto done;
    }
    for (unsigned i = 0; i < slots; ++i)
        if (bytes[i * stride] != marker(i, round)) { failed = 1; goto done; }

    /* Release and replace a region past the old PT limit, preserving its
     * neighbors. The new anonymous page must not expose old backing. */
    const unsigned hole = 600;
    if (munmap(base + hole * stride, stride) ||
        mmap(base + hole * stride, stride, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0)
            != base + hole * stride) {
        perror("sparse replace"); failed = 1; goto done;
    }
    for (unsigned i = 0; i < slots; ++i)
        if (bytes[i * stride] != (i == hole ? 0 : marker(i, round))) {
            failed = 1; goto done;
        }
    printf("SPARSE_PT round=%u protect/cow/replace OK\n", round);
done:
    if (munmap(base, length)) { perror("sparse unmap"); failed = 1; }
    return failed;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (sysconf(_SC_PAGESIZE) != 4096) {
        fputs("SPARSE_PT requires 4 KiB pages\n", stderr);
        return 2;
    }
    for (unsigned round = 0; round < rounds; ++round) {
        if (run(round)) { puts("SPARSE_PT FAIL"); return 1; }
    }
    puts("SPARSE_PT PASS");
    return 0;
}

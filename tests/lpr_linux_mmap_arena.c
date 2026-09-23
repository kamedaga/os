/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

/* Hold more than the preferred 1 GiB arena using ordinary anonymous maps,
 * then exercise private and shared file maps. Only boundary pages are
 * touched, so the virtual-address pressure does not require 1 GiB of RAM. */
enum { MAP_COUNT = 272, MAP_BYTES = 4 * 1024 * 1024 };

int main(void)
{
    void *held[MAP_COUNT];
    unsigned count = 0, outside = 0;
    int failed = 0, fd = -1;
    void *private = MAP_FAILED, *shared = MAP_FAILED;
    char path[] = "/tmp/mmap-arena-XXXXXX";
    setvbuf(stdout, NULL, _IONBF, 0);
    for (; count < MAP_COUNT; ++count) {
        held[count] = mmap(NULL, MAP_BYTES, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (held[count] == MAP_FAILED) {
            printf("MMAP_ARENA anonymous[%u] FAIL errno=%d\n", count, errno);
            failed = 1;
            goto cleanup;
        }
        uintptr_t address = (uintptr_t)held[count];
        outside += address < UINT64_C(0x10000000000) ||
                   address >= UINT64_C(0x10040000000);
        volatile unsigned char *p = held[count];
        p[0] = (unsigned char)count;
        p[MAP_BYTES - 1] = (unsigned char)(count + 7);
    }
    printf("MMAP_ARENA anonymous count=%u outside_preferred=%u OK\n", count, outside);
    fd = mkstemp(path);
    if (fd < 0) goto fail;
    (void)unlink(path);
    if (ftruncate(fd, MAP_BYTES)) goto fail;
    shared = mmap(NULL, MAP_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shared == MAP_FAILED) goto fail;
    volatile unsigned char *s = shared;
    s[0] = 31;
    s[MAP_BYTES - 1] = 47;
    private = mmap(NULL, MAP_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (private == MAP_FAILED) goto fail;
    volatile unsigned char *p = private;
    if (p[0] != 31 || p[MAP_BYTES - 1] != 47) goto fail;
    p[0] = 63;
    p[MAP_BYTES - 1] = 79;
    if (s[0] != 31 || s[MAP_BYTES - 1] != 47) goto fail;
    if (mprotect(private, MAP_BYTES, PROT_READ) ||
        mprotect(private, MAP_BYTES, PROT_READ | PROT_WRITE)) goto fail;
    if (p[0] != 63 || p[MAP_BYTES - 1] != 79) goto fail;
    printf("MMAP_ARENA file private=%p shared=%p COW/protect OK\n", private, shared);
    goto cleanup;
fail:
    printf("MMAP_ARENA file FAIL errno=%d\n", errno);
    failed = 1;
cleanup:
    if (private != MAP_FAILED && munmap(private, MAP_BYTES)) failed = 1;
    if (shared != MAP_FAILED && munmap(shared, MAP_BYTES)) failed = 1;
    if (fd >= 0 && close(fd)) failed = 1;
    for (unsigned i = 0; i < count; ++i) {
        volatile unsigned char *p = held[i];
        if (p[0] != (unsigned char)i ||
            p[MAP_BYTES - 1] != (unsigned char)(i + 7)) failed = 1;
        if (munmap(held[i], MAP_BYTES)) failed = 1;
    }
    printf("MMAP_ARENA=%s\n", failed ? "FAIL" : "OK");
    return failed;
}

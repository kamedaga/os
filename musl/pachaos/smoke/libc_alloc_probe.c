#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

long syscall(long, ...);
int brk(void *);
void *sbrk(intptr_t);

static void metric(const char *name, unsigned long long value)
{
    printf("[libc-alloc-probe] metric op=%s value=%llu\n", name, value);
}

static void metric_status(const char *name, long status, int err)
{
    printf("[libc-alloc-probe] metric op=%s status=%ld errno=%d\n", name, status, err);
}

static int probe_mmap_size(size_t size)
{
    errno = 0;
    unsigned char *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        metric_status("mmap_rw_failed", -1, errno);
        return -1;
    }
    p[0] = 0x5a;
    p[size - 1] = 0xa5;
    metric("mmap_rw_size", (unsigned long long)size);
    if (munmap(p, size) != 0) {
        metric_status("munmap_failed", -1, errno);
        return -1;
    }
    return 0;
}

static int probe_partial_munmap(void)
{
    const size_t page = 4096;
    unsigned char *p = mmap(NULL, page * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        metric_status("partial_munmap_mmap_failed", -1, errno);
        return -1;
    }
    p[0] = 0x11;
    p[page] = 0x22;
    p[(page * 2) + 17] = 0x33;

    errno = 0;
    int status = munmap(p + page, page);
    metric_status("partial_munmap_middle", status, errno);
    if (status != 0) {
        (void)munmap(p, page * 3);
        return -1;
    }

    p[1] = 0x44;
    p[(page * 2) + 18] = 0x55;
    errno = 0;
    status = munmap(p, page);
    metric_status("partial_munmap_left", status, errno);
    if (status != 0) {
        (void)munmap(p + page * 2, page);
        return -1;
    }
    errno = 0;
    status = munmap(p + page * 2, page);
    metric_status("partial_munmap_right", status, errno);
    return status == 0 ? 0 : -1;
}

static int probe_map_fixed_replace(void)
{
    const size_t page = 4096;
    unsigned char *p = mmap(NULL, page * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        metric_status("map_fixed_base_failed", -1, errno);
        return -1;
    }
    p[0] = 0x61;
    p[page] = 0x62;
    p[page * 2] = 0x63;

    errno = 0;
    unsigned char *middle = mmap(p + page, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    metric_status("map_fixed_replace", middle == MAP_FAILED ? -1 : (long)(uintptr_t)middle, errno);
    if (middle != p + page) {
        (void)munmap(p, page * 3);
        return -1;
    }
    if (p[0] != 0x61 || p[page * 2] != 0x63) {
        metric_status("map_fixed_preserve_edges", -1, 0);
        (void)munmap(p, page * 3);
        return -1;
    }
    middle[0] = 0x7a;
    errno = 0;
    int status = munmap(p, page * 3);
    metric_status("map_fixed_unmap_combined", status, errno);
    return status == 0 ? 0 : -1;
}

static int probe_madvise_mremap(void)
{
    const size_t page = 4096;
    unsigned char *p = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        metric_status("madvise_mmap_failed", -1, errno);
        return -1;
    }
    p[0] = 0x91;
    p[page] = 0x92;
    errno = 0;
    int status = madvise(p, page * 2, MADV_NORMAL);
    metric_status("madvise_normal", status, errno);
    if (status == 0) {
        errno = 0;
        status = madvise(p, page * 2, MADV_DONTNEED);
        metric_status("madvise_dontneed", status, errno);
    }
    (void)munmap(p, page * 2);
    if (status != 0) return -1;

    p = mmap(NULL, page * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        metric_status("mremap_shrink_mmap_failed", -1, errno);
        return -1;
    }
    p[0] = 0xa1;
    p[page * 2] = 0xa3;
    errno = 0;
    unsigned char *shrunk = mremap(p, page * 3, page, 0);
    metric_status("mremap_shrink", shrunk == MAP_FAILED ? -1 : (long)(uintptr_t)shrunk, errno);
    if (shrunk != p || shrunk[0] != 0xa1) {
        if (shrunk != MAP_FAILED) (void)munmap(shrunk, page);
        return -1;
    }
    (void)munmap(shrunk, page);

    p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        metric_status("mremap_grow_mmap_failed", -1, errno);
        return -1;
    }
    p[0] = 0xb1;
    errno = 0;
    unsigned char *grown = mremap(p, page, page * 2, MREMAP_MAYMOVE);
    metric_status("mremap_grow_move", grown == MAP_FAILED ? -1 : (long)(uintptr_t)grown, errno);
    if (grown == MAP_FAILED || grown[0] != 0xb1) {
        if (grown != MAP_FAILED) (void)munmap(grown, page * 2);
        return -1;
    }
    grown[page] = 0xb2;
    (void)munmap(grown, page * 2);

    p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *target = mmap(NULL, page * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED || target == MAP_FAILED) {
        metric_status("mremap_fixed_mmap_failed", -1, errno);
        if (p != MAP_FAILED) (void)munmap(p, page);
        if (target != MAP_FAILED) (void)munmap(target, page * 2);
        return -1;
    }
    p[0] = 0xc1;
    errno = 0;
    unsigned char *fixed = mremap(p, page, page * 2, MREMAP_MAYMOVE | MREMAP_FIXED, target);
    metric_status("mremap_fixed_move", fixed == MAP_FAILED ? -1 : (long)(uintptr_t)fixed, errno);
    if (fixed != target || fixed[0] != 0xc1) {
        if (fixed != MAP_FAILED) (void)munmap(fixed, page * 2);
        return -1;
    }
    fixed[page] = 0xc2;
    (void)munmap(fixed, page * 2);
    return 0;
}

int main(void)
{
    printf("[libc-alloc-probe] start\n");

    errno = 0;
    long brk0 = syscall(SYS_brk, 0);
    metric_status("sys_brk0", brk0, errno);

    errno = 0;
    void *sbrk0 = sbrk(0);
    metric_status("sbrk0", sbrk0 == (void *)-1 ? -1 : (long)(uintptr_t)sbrk0, errno);
    if (sbrk0 != (void *)-1) {
        unsigned char *old = (unsigned char *)sbrk0;
        unsigned char *next = old + 8192;
        errno = 0;
        int brk_status = brk(next);
        metric_status("brk_grow_8k", brk_status, errno);
        if (brk_status == 0) {
            old[0] = 0x44;
            next[-1] = 0x55;
            errno = 0;
            int shrink_status = brk(old + 4096);
            metric_status("brk_shrink_4k", shrink_status, errno);
            if (shrink_status == 0) {
                errno = 0;
                int regrow_status = brk(next);
                metric_status("brk_regrow_4k", regrow_status, errno);
                if (regrow_status == 0) {
                    next[-1] = 0x56;
                }
            }
            (void)brk(old);
        }
    }
    errno = 0;
    void *sbrk_grow = sbrk(4096);
    metric_status("sbrk_grow_4k", sbrk_grow == (void *)-1 ? -1 : (long)(uintptr_t)sbrk_grow, errno);
    if (sbrk_grow != (void *)-1) {
        ((unsigned char *)sbrk_grow)[0] = 0x66;
        ((unsigned char *)sbrk_grow)[4095] = 0x77;
        (void)brk(sbrk_grow);
    }

    errno = 0;
    void *guard = mmap(NULL, 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    metric_status("mmap_prot_none", guard == MAP_FAILED ? -1 : (long)(uintptr_t)guard, errno);
    if (guard != MAP_FAILED) {
        errno = 0;
        int mp = mprotect(guard, 4096, PROT_READ | PROT_WRITE);
        metric_status("mprotect_first_page_rw", mp, errno);
        if (mp == 0) {
            ((unsigned char *)guard)[0] = 0x33;
        }
        (void)munmap(guard, 8192);
    }

    if (probe_partial_munmap() != 0) {
        return 3;
    }
    if (probe_map_fixed_replace() != 0) {
        return 4;
    }
    if (probe_madvise_mremap() != 0) {
        return 5;
    }

    (void)probe_mmap_size(4096);
    (void)probe_mmap_size(65536);
    (void)probe_mmap_size(1048576);
    (void)probe_mmap_size(4194304);

    enum { MAX_PTRS = 32768 };
    void **ptrs = calloc(MAX_PTRS, sizeof(void *));
    if (ptrs == NULL) {
        printf("[libc-alloc-probe] ptr table calloc failed errno=%d\n", errno);
        return 2;
    }

    unsigned small_count = 0;
    for (; small_count < MAX_PTRS; small_count++) {
        ptrs[small_count] = calloc(1, 128);
        if (ptrs[small_count] == NULL) break;
        memset(ptrs[small_count], (int)(small_count & 255u), 128);
    }
    metric("calloc_128_count", small_count);
    for (unsigned i = 0; i < small_count; i++) {
        free(ptrs[i]);
    }

    unsigned page_count = 0;
    for (; page_count < MAX_PTRS; page_count++) {
        ptrs[page_count] = malloc(4096);
        if (ptrs[page_count] == NULL) break;
        memset(ptrs[page_count], (int)(page_count & 255u), 4096);
    }
    metric("malloc_4k_count", page_count);
    for (unsigned i = 0; i < page_count; i++) {
        free(ptrs[i]);
    }

    unsigned large_count = 0;
    for (; large_count < 4096; large_count++) {
        ptrs[large_count] = malloc(131072);
        if (ptrs[large_count] == NULL) break;
        ((unsigned char *)ptrs[large_count])[0] = (unsigned char)large_count;
    }
    metric("malloc_128k_count", large_count);
    for (unsigned i = 0; i < large_count; i++) {
        free(ptrs[i]);
    }

    unsigned mmap_hold_count = 0;
    int mmap_hold_errno = 0;
    for (; mmap_hold_count < 4096; mmap_hold_count++) {
        errno = 0;
        ptrs[mmap_hold_count] = mmap(NULL, 131072, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptrs[mmap_hold_count] == MAP_FAILED) {
            ptrs[mmap_hold_count] = NULL;
            mmap_hold_errno = errno;
            break;
        }
        ((unsigned char *)ptrs[mmap_hold_count])[0] = (unsigned char)mmap_hold_count;
    }
    metric("mmap_128k_hold_count", mmap_hold_count);
    metric_status("mmap_128k_hold_fail", mmap_hold_count < 4096 ? -1 : 0, mmap_hold_errno);
    for (unsigned i = 0; i < mmap_hold_count; i++) {
        (void)munmap(ptrs[i], 131072);
    }

    unsigned mmap_reuse_count = 0;
    int mmap_reuse_errno = 0;
    for (; mmap_reuse_count < 512; mmap_reuse_count++) {
        errno = 0;
        void *p = mmap(NULL, 131072, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            mmap_reuse_errno = errno;
            break;
        }
        ((unsigned char *)p)[0] = (unsigned char)mmap_reuse_count;
        (void)munmap(p, 131072);
    }
    metric("mmap_128k_reuse_count", mmap_reuse_count);
    metric_status("mmap_128k_reuse_fail", mmap_reuse_count < 512 ? -1 : 0, mmap_reuse_errno);

    free(ptrs);
    printf("[libc-alloc-probe] ok\n");
    return 0;
}

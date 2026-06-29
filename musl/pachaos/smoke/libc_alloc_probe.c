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

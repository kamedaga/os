#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static int inspect(const char *name)
{
    pthread_attr_t attr;
    void *base = 0;
    size_t size = 0;
    int status = pthread_getattr_np(pthread_self(), &attr);
    if (!status) {
        status = pthread_attr_getstack(&attr, &base, &size);
        pthread_attr_destroy(&attr);
    }
    uintptr_t here = (uintptr_t)&attr;
    int inside = !status && here >= (uintptr_t)base && here < (uintptr_t)base + size;
    printf("STACK_BOUNDS %s pid=%ld tid=%ld local=%p base=%p size=%zu status=%d inside=%d\n",
        name, (long)getpid(), syscall(SYS_gettid), (void *)here, base, size, status, inside);
    return inside;
}

__attribute__((noinline)) static int deep(void)
{
    volatile char pad[32768];
    for (size_t i = 0; i < sizeof(pad); i += 4096) pad[i] = 1;
    /* Force the full frame to exist even when Clang scalarizes sparse stores. */
    __asm__ volatile("" : : "r"(pad) : "memory");
    int result = inspect("deep-main");
    return result && pad[0] == 1;
}

static void *worker(void *unused)
{
    (void)unused;
    return (void *)(uintptr_t)!inspect("worker");
}

static int rejected(unsigned char *base, size_t old_size, size_t new_size,
                    int flags, int expected_oom)
{
    errno = 0;
    void *result = mremap(base, old_size, new_size, flags);
    int ok = result == MAP_FAILED && (expected_oom < 0 ||
        (expected_oom ? errno == ENOMEM : errno != ENOMEM));
    printf("MREMAP_PROBE old=%zu new=%zu flags=%d errno=%d ok=%d\n",
           old_size, new_size, flags, errno, ok);
    return ok;
}

static int mapping_probe(int file_backed)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    FILE *file = file_backed ? tmpfile() : NULL;
    if (file_backed && (!file || ftruncate(fileno(file), (off_t)(page * 2)))) return 0;
    unsigned char *base = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | (file_backed ? 0 : MAP_ANONYMOUS), file ? fileno(file) : -1, 0);
    if (file) fclose(file);
    if (base == MAP_FAILED) return 0;
    base[0] = 41;
    base[page] = 73;
    int ok = rejected(base, page, page * 2, 0, 1);
    ok = rejected(base + 1, page, page * 2, 0, 0) && ok;
    ok = rejected(base, page, page * 2, 0x40000000, 0) && ok;
    ok = rejected(base, page, page * 2, MREMAP_FIXED, 0) && ok;
    /* Linux reports ENOMEM for this size; the native ABI rejects overflow. */
    ok = rejected(base, page, SIZE_MAX, 0, -1) && ok;
    ok = (base[0] == 41 && base[page] == 73) && ok;
    if (munmap(base, page * 2)) return 0;
    ok = rejected(base, page, page * 2, 0, 0) && ok;
    printf("MREMAP_PROBE %s contents-and-errors=%s\n", file_backed ? "file" : "anonymous", ok ? "OK" : "FAIL");
    return ok;
}

int main(void)
{
    int ok = inspect("main");
    ok = deep() && ok;
    pthread_t thread;
    void *result = (void *)1;
    if (pthread_create(&thread, 0, worker, 0) || pthread_join(thread, &result)) return 2;
    ok = mapping_probe(0) && ok;
    ok = mapping_probe(1) && ok;
    puts(ok && !result ? "STACK_BOUNDS=OK" : "STACK_BOUNDS=FAIL");
    return !(ok && !result);
}

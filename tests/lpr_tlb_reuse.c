#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Keep both backing pages live while reusing one virtual address. Reusing
 * freshly freed anonymous memory alone can mask stale TLB entries when the
 * allocator immediately returns the same physical page. Readers are quiescent
 * during unmap/remap; any stale value after publication is therefore a bug,
 * not an intentional access to an unmapped address. */
enum { READERS = 3, ROUNDS = 128, PAGE = 4096 };
static unsigned char *view;
static atomic_uint epoch, done[READERS], failed;

static void *reader(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    for (unsigned round = 1; round <= ROUNDS; ++round) {
        while (atomic_load_explicit(&epoch, memory_order_acquire) < round)
            __asm__ volatile("pause" ::: "memory");
        unsigned char expected = (round & 1) ? 0x35 : 0xa7;
        volatile unsigned char *sample = view;
        if (sample[0] != expected || sample[PAGE - 1] != expected)
            atomic_store_explicit(&failed, 1, memory_order_relaxed);
        atomic_store_explicit(&done[id], round, memory_order_release);
    }
    return NULL;
}

static void fail(const char *where)
{
    fprintf(stderr, "TLB_REUSE FAIL %s errno=%d\n", where, errno);
    /* Do not let readers access a partially restored mapping on failure. */
    _exit(1);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(60);
    char path[] = "/tmp/tlb-reuse-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) fail("mkstemp");
    if (unlink(path) || ftruncate(fd, PAGE * 2)) fail("prepare");
    unsigned char *alias = mmap(NULL, PAGE * 2, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
    if (alias == MAP_FAILED) fail("alias");
    for (unsigned i = 0; i < PAGE; ++i) {
        alias[i] = 0x35;
        alias[PAGE + i] = 0xa7;
    }
    view = mmap(NULL, PAGE, PROT_READ, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) fail("initial view");
    if (syscall(SYS_membarrier, 1 << 4, 0, 0)) fail("register membarrier");
    pthread_t threads[READERS];
    for (unsigned i = 0; i < READERS; ++i) {
        int result = pthread_create(&threads[i], NULL, reader, (void *)(uintptr_t)i);
        if (result) { errno = result; fail("pthread_create"); }
    }
    for (unsigned round = 1; round <= ROUNDS; ++round) {
        if (munmap(view, PAGE)) fail("munmap");
        void *mapped = mmap(view, PAGE, PROT_READ, MAP_SHARED | MAP_FIXED,
                            fd, (round & 1) ? 0 : PAGE);
        if (mapped != view) fail("same-address mmap");
        /* Do not issue membarrier here: its own CR3 flush could hide a
         * missing munmap shootdown. Readers also avoid syscall transitions. */
        atomic_store_explicit(&epoch, round, memory_order_release);
        for (unsigned i = 0; i < READERS; ++i)
            while (atomic_load_explicit(&done[i], memory_order_acquire) != round)
                __asm__ volatile("pause" ::: "memory");
    }
    for (unsigned i = 0; i < READERS; ++i) pthread_join(threads[i], NULL);
    /* Separate API smoke only; this is not a memory-ordering litmus test. */
    for (unsigned i = 0; i < 16; ++i)
        if (syscall(SYS_membarrier, 1 << 3, 0, 0)) fail("membarrier");
    if (alias[0] != 0x35 || alias[PAGE] != 0xa7)
        atomic_store(&failed, 1);
    if (munmap(view, PAGE) || munmap(alias, PAGE * 2) || close(fd)) fail("cleanup");
    printf("TLB_REUSE=%s readers=%u rounds=%u\n",
           atomic_load(&failed) ? "FAIL" : "OK", READERS, ROUNDS);
    return atomic_load(&failed) ? 1 : 0;
}

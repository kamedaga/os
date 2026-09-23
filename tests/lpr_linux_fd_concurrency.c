#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

enum { THREADS = 6, ROUNDS = 300 };
static atomic_uint failures;
static pthread_barrier_t gate;

static void fail(unsigned thread, unsigned round, const char *op)
{
    unsigned count = atomic_fetch_add(&failures, 1);
    if (count < 30) fprintf(stderr, "FD_CONCURRENCY thread=%u round=%u op=%s errno=%d\n",
        thread, round, op, errno);
}

static void *worker(void *arg)
{
    unsigned thread = (unsigned)(uintptr_t)arg;
    pthread_barrier_wait(&gate);
    for (unsigned i = 0; i < ROUNDS; ++i) {
        int fd = memfd_create("concurrent-fd", MFD_CLOEXEC);
        if (fd < 0) { fail(thread, i, "memfd"); continue; }
        unsigned value = thread * ROUNDS + i, read_value = ~value;
        if (write(fd, &value, sizeof(value)) != sizeof(value)) fail(thread, i, "write");
        if (pread(fd, &read_value, sizeof(read_value), 0) != sizeof(read_value) || value != read_value)
            fail(thread, i, "contents");
        int ev = eventfd(0, EFD_CLOEXEC);
        if (ev < 0) fail(thread, i, "eventfd");
        else if (close(ev)) fail(thread, i, "close-eventfd");
        if (close(fd)) fail(thread, i, "close-memfd");
    }
    return NULL;
}

int main(void)
{
    pthread_t threads[THREADS];
    pthread_barrier_init(&gate, NULL, THREADS);
    for (unsigned i = 0; i < THREADS; ++i)
        if (pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i)) return 2;
    for (unsigned i = 0; i < THREADS; ++i) pthread_join(threads[i], NULL);
    printf("FD_CONCURRENCY failures=%u\n", atomic_load(&failures));
    return atomic_load(&failures) ? 1 : 0;
}

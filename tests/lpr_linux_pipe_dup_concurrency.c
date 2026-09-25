#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

enum { THREADS = 8, ROUNDS = 400 };
static pthread_barrier_t gate;
static atomic_uint failures;
static void fail(const char *op)
{
    if (atomic_fetch_add(&failures, 1) < 12)
        fprintf(stderr, "PIPE_DUP_FAIL op=%s errno=%d\n", op, errno);
}
static void *worker(void *argument)
{
    const unsigned id = (unsigned)(uintptr_t)argument;
    pthread_barrier_wait(&gate);
    for (unsigned i = 0; i < ROUNDS; ++i) {
        int pair[2];
        if (pipe2(pair, O_CLOEXEC | O_NONBLOCK)) { fail("pipe2"); continue; }
        int copy = fcntl(pair[0], F_DUPFD_CLOEXEC, 3);
        if (copy < 0) fail("dup");
        else {
            unsigned value = id * ROUNDS + i, actual = ~value;
            if (!(fcntl(copy, F_GETFD) & FD_CLOEXEC)) fail("cloexec");
            if (write(pair[1], &value, sizeof(value)) != sizeof(value)) fail("write");
            if (read(copy, &actual, sizeof(actual)) != sizeof(actual) || actual != value)
                fail("read-content");
            if (close(copy)) fail("close-dup");
        }
        if (close(pair[0])) fail("close-read");
        if (close(pair[1])) fail("close-write");
    }
    return 0;
}
int main(void)
{
    pthread_t threads[THREADS];
    pthread_barrier_init(&gate, 0, THREADS);
    for (unsigned i = 0; i < THREADS; ++i)
        if (pthread_create(&threads[i], 0, worker, (void *)(uintptr_t)i)) return 2;
    for (unsigned i = 0; i < THREADS; ++i) pthread_join(threads[i], 0);
    printf("PIPE_DUP_CONCURRENCY failures=%u\n", atomic_load(&failures));
    return atomic_load(&failures) ? 1 : 0;
}

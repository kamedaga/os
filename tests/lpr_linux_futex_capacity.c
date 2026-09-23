#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { THREADS = 320, ROUNDS = 2, FUTEX_WAIT_PRIVATE = 128, FUTEX_WAKE_PRIVATE = 129 };
static atomic_uint words[ROUNDS], arrived[ROUNDS], failures;

static void *worker(void *unused)
{
    (void)unused;
    for (unsigned r = 0; r < ROUNDS; ++r) {
        atomic_fetch_add(&arrived[r], 1);
        while (!atomic_load(&words[r])) {
            struct timespec timeout = { .tv_sec = 15 };
            long rc = syscall(SYS_futex, &words[r], FUTEX_WAIT_PRIVATE, 0,
                &timeout, NULL, 0);
            if (rc < 0 && errno != EAGAIN && errno != EINTR) {
                atomic_fetch_add(&failures, 1);
                break;
            }
        }
    }
    return NULL;
}

int main(void)
{
    pthread_t threads[THREADS];
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) || pthread_attr_setstacksize(&attr, 128 * 1024)) return 2;
    unsigned created = 0;
    for (; created < THREADS; ++created) {
        int error = pthread_create(&threads[created], &attr, worker, NULL);
        if (error) {
            fprintf(stderr, "FUTEX_CAPACITY create=%u error=%d\n", created, error);
            atomic_fetch_add(&failures, 1);
            break;
        }
    }
    pthread_attr_destroy(&attr);
    for (unsigned r = 0; r < ROUNDS; ++r) {
        unsigned tries = 0;
        while (atomic_load(&arrived[r]) != created && ++tries < 30000) usleep(1000);
        if (atomic_load(&arrived[r]) != created) atomic_fetch_add(&failures, 1);
        usleep(300000);
        atomic_store(&words[r], 1);
        long woke = syscall(SYS_futex, &words[r], FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
        printf("FUTEX_CAPACITY round=%u arrived=%u woke=%ld\n", r, atomic_load(&arrived[r]), woke);
        if (woke < 0) atomic_fetch_add(&failures, 1);
    }
    for (unsigned i = 0; i < created; ++i) pthread_join(threads[i], NULL);
    printf("FUTEX_CAPACITY threads=%u rounds=%u failures=%u\n", created, ROUNDS, atomic_load(&failures));
    return atomic_load(&failures) ? 1 : 0;
}

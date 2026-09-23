#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

enum { THREADS = 24, ROUNDS = 3 };
static int pairs[THREADS][2];
static atomic_uint failures, waiting;
static pthread_barrier_t gate;

static void *worker(void *argument)
{
    unsigned index = (unsigned)(size_t)argument;
    pthread_barrier_wait(&gate);
    for (unsigned round = 0; round < ROUNDS; ++round) {
        struct pollfd item = { .fd = pairs[index][0], .events = POLLIN };
        atomic_fetch_add(&waiting, 1);
        int result = poll(&item, 1, 5000);
        if (result != 1 || !(item.revents & POLLIN)) {
            fprintf(stderr, "UNIX_WAIT thread=%u round=%u poll=%d events=%x errno=%d\n",
                index, round, result, item.revents, errno);
            atomic_fetch_add(&failures, 1);
        }
        char byte;
        if (recv(pairs[index][0], &byte, 1, 0) != 1 || byte != 'x')
            atomic_fetch_add(&failures, 1);
        pthread_barrier_wait(&gate);
    }
    return NULL;
}

int main(void)
{
    pthread_t threads[THREADS];
    if (pthread_barrier_init(&gate, NULL, THREADS + 1)) return 2;
    for (unsigned i = 0; i < THREADS; ++i) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[i])) return 2;
        if (pthread_create(&threads[i], NULL, worker, (void *)(size_t)i)) return 2;
    }
    pthread_barrier_wait(&gate);
    for (unsigned round = 0; round < ROUNDS; ++round) {
        while (atomic_load(&waiting) < THREADS * (round + 1)) usleep(1000);
        /* Let every thread arm its broker-death and socket wait before waking. */
        usleep(300000);
        for (unsigned i = 0; i < THREADS; ++i)
            if (send(pairs[i][1], "x", 1, 0) != 1) return 2;
        pthread_barrier_wait(&gate);
    }
    for (unsigned i = 0; i < THREADS; ++i) {
        pthread_join(threads[i], NULL);
        close(pairs[i][0]); close(pairs[i][1]);
    }
    printf("UNIX_WAIT_CONCURRENCY threads=%u rounds=%u failures=%u\n",
        THREADS, ROUNDS, atomic_load(&failures));
    return atomic_load(&failures) ? 1 : 0;
}

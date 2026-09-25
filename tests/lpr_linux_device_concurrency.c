#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <signal.h>
#include <unistd.h>

enum { THREADS = 8, ROUNDS = 2000 };
static atomic_uint failures;
static pthread_barrier_t gate;
static void fail(const char *op)
{
    if (atomic_fetch_add(&failures, 1) < 12)
        fprintf(stderr, "DEVICE_CONCURRENCY op=%s errno=%d\n", op, errno);
}
static void *worker(void *arg)
{
    (void)arg;
    pthread_barrier_wait(&gate);
    for (unsigned i = 0; i < ROUNDS; ++i) {
        int fd = open(i & 1 ? "/dev/null" : "/dev/zero", O_RDONLY | O_CLOEXEC);
        if (fd < 0) { fail("open"); continue; }
        char byte = 42;
        ssize_t n = read(fd, &byte, 1);
        if (n != (i & 1 ? 0 : 1) || (!(i & 1) && byte != 0)) fail("read");
        if (fcntl(fd, F_GETFD) != FD_CLOEXEC) fail("cloexec");
        int ev = eventfd(0, EFD_CLOEXEC);
        if (ev < 0) fail("eventfd");
        else if (close(ev)) fail("close-eventfd");
        if ((i % 20) == 0) {
            sigset_t mask;
            sigemptyset(&mask);
            int extra[] = {
                epoll_create1(EPOLL_CLOEXEC),
                timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC),
                inotify_init1(IN_CLOEXEC),
                signalfd(-1, &mask, SFD_CLOEXEC),
                socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0),
            };
            for (unsigned k = 0; k < sizeof(extra) / sizeof(extra[0]); ++k) {
                if (extra[k] < 0) fail("create-extra");
                else {
                    if (fcntl(extra[k], F_GETFD) != FD_CLOEXEC) fail("cloexec-extra");
                    if (close(extra[k])) fail("close-extra");
                }
            }
        }
        if (close(fd)) fail("close");
    }
    return NULL;
}
int main(void)
{
    pthread_t threads[THREADS];
    if (pthread_barrier_init(&gate, NULL, THREADS)) return 2;
    for (unsigned i = 0; i < THREADS; ++i)
        if (pthread_create(&threads[i], NULL, worker, NULL)) return 2;
    for (unsigned i = 0; i < THREADS; ++i) pthread_join(threads[i], NULL);
    printf("DEVICE_CONCURRENCY failures=%u\n", atomic_load(&failures));
    return atomic_load(&failures) ? 1 : 0;
}

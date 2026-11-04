#define _GNU_SOURCE
#include "../userland/gpud/ready.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
struct host_thread {
    pthread_t id;
    struct gpud_ready_worker *w;
    void (*run)(struct gpud_ready_worker *);
    int done;
};
static _Atomic unsigned long drain_calls;
static _Thread_local int ep = -1;
static _Thread_local struct {
    int fd;
    uint64_t events;
} old[256];
static _Thread_local size_t oldn;
static int lift(int fd) {
    int n = fcntl(fd, F_DUPFD_CLOEXEC, 16);
    close(fd);
    return n;
}
int gpud_ready_os_event(pacha_fd_t *fd) {
    *fd = lift(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    return *fd < 16 ? -1 : 0;
}
int gpud_ready_os_signal(pacha_fd_t fd) {
    uint64_t v = 1;
    ssize_t n = write(fd, &v, 8);
    return n == 8 || errno == EAGAIN ? 0 : -1;
}
int gpud_ready_os_drain(pacha_fd_t fd) {
    atomic_fetch_add(&drain_calls, 1);
    uint64_t v;
    ssize_t n = read(fd, &v, 8);
    return n == 8 || errno == EAGAIN ? 0 : -1;
}
int gpud_ready_os_wait(struct pacha_fd_wait_retained *set, struct pacha_pollfd *p, size_t n,
                       uint64_t timeout) {
    (void)set;
    assert(n <= 256);
    if (ep < 0) {
        ep = epoll_create1(EPOLL_CLOEXEC);
        assert(ep >= 0);
    }
    int change = n != oldn;
    for (size_t i = 0; i < n && !change; i++)
        change = p[i].fd != (uint64_t)old[i].fd || p[i].events != old[i].events;
    if (change) {
        for (size_t i = 0; i < oldn; i++)
            assert(!epoll_ctl(ep, EPOLL_CTL_DEL, old[i].fd, NULL));
        for (size_t i = 0; i < n; i++) {
            struct epoll_event e = {
                .events = EPOLLRDHUP | ((p[i].events & PACHA_FD_EVENT_READABLE) ? EPOLLIN : 0),
                .data.u32 = i};
            assert(!epoll_ctl(ep, EPOLL_CTL_ADD, p[i].fd, &e));
            old[i].fd = p[i].fd;
            old[i].events = p[i].events;
        }
        oldn = n;
    }
    for (size_t i = 0; i < n; i++)
        p[i].revents = 0;
    struct epoll_event out[256];
    int r;
    do
        r = epoll_wait(ep, out, 256, timeout == UINT64_MAX ? -1 : 0);
    while (r < 0 && errno == EINTR);
    for (int j = 0; j < r; j++) {
        unsigned i = out[j].data.u32;
        assert(i < n);
        p[i].revents = ((out[j].events & EPOLLIN) ? PACHA_FD_EVENT_READABLE : 0) |
                       ((out[j].events & (EPOLLHUP | EPOLLRDHUP)) ? PACHA_FD_EVENT_HANGUP : 0);
    }
    return r;
}
static void *entry(void *arg) {
    struct host_thread *t = arg;
    t->run(t->w);
    if (ep >= 0)
        close(ep);
    ep = -1;
    oldn = 0;
    assert(!gpud_ready_os_signal(t->done));
    return NULL;
}
int gpud_ready_os_start(struct gpud_ready_worker *w, void (*run)(struct gpud_ready_worker *),
                        void **out) {
    struct host_thread *t = calloc(1, sizeof(*t));
    assert(t);
    t->w = w;
    t->run = run;
    t->done = lift(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    int rc = pthread_create(&t->id, NULL, entry, t);
    if (rc) {
        close(t->done);
        free(t);
        return -1;
    }
    *out = t;
    return 0;
}
int gpud_ready_os_ack(void *arg, pacha_fd_t ack) {
    struct host_thread *t = arg;
    struct pollfd p[2] = {{ack, POLLIN, 0}, {t->done, POLLIN, 0}};
    int n;
    do
        n = poll(p, 2, 3000);
    while (n < 0 && errno == EINTR);
    return n > 0 && p[0].revents ? 0 : -1;
}
int gpud_ready_os_join(void *arg) {
    struct host_thread *t = arg;
    int rc = pthread_join(t->id, NULL);
    close(t->done);
    free(t);
    return rc ? -1 : 0;
}
int gpud_ready_os_close(pacha_fd_t fd) { return close(fd); }
static uint64_t ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000ULL + t.tv_nsec;
}
static struct gpud_ready_event await(struct gpud_ready *r) {
    struct gpud_ready_event e;
    for (int i = 0; i < 100; i++) {
        int n = gpud_ready_next(r, &e);
        assert(n >= 0);
        if (n)
            return e;
        struct pollfd p = {gpud_ready_fd(r), POLLIN, 0};
        assert(poll(&p, 1, 3000) > 0);
    }
    abort();
}
static void semantics(void) {
    struct gpud_ready *r = gpud_ready_create();
    assert(r);
    pacha_fd_t fd;
    assert(!gpud_ready_os_event(&fd));
    struct gpud_ready_source *s = NULL;
    /* Ready before registration, level stays ready, bounded one-shot delivery. */
    assert(!gpud_ready_os_signal(fd));
    assert(!gpud_ready_add(r, fd, PACHA_FD_EVENT_READABLE, 1, (void *)123, &s));
    struct gpud_ready_event e = await(r);
    assert(e.source == s && e.context == (void *)123 && e.events & PACHA_FD_EVENT_READABLE);
    uint64_t first = e.generation;
    for (int i = 0; i < 1000; i++) {
        assert(!gpud_ready_os_signal(fd));
        assert(!gpud_ready_next(r, &e));
    }
    assert(!gpud_ready_rearm(s));
    e = await(r);
    assert(e.generation == first);
    assert(!gpud_ready_os_drain(fd));
    assert(!gpud_ready_rearm(s));
    assert(!gpud_ready_os_signal(fd));
    e = await(r);
    assert(e.source == s);
    assert(!gpud_ready_remove(&s));
    close(fd);
    int p[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, p));
    int a = lift(p[0]), b = lift(p[1]);
    assert(!gpud_ready_add(r, a, PACHA_FD_EVENT_HANGUP, 2, NULL, &s));
    close(b);
    e = await(r);
    assert(e.events & PACHA_FD_EVENT_HANGUP);
    assert(e.generation > first);
    assert(!gpud_ready_remove(&s));
    close(a);
    /* Reuse the numeric FD after a queued HUP, before owner dequeues it. */
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, p));
    a = lift(p[0]);
    b = lift(p[1]);
    assert(!gpud_ready_add(r, a, PACHA_FD_EVENT_HANGUP, 3, NULL, &s));
    close(b);
    struct pollfd poller = {gpud_ready_fd(r), POLLIN, 0};
    assert(poll(&poller, 1, 3000) > 0);
    assert(!gpud_ready_remove(&s));
    int reused = a;
    close(a);
    int temp = eventfd(0, EFD_NONBLOCK);
    assert(temp >= 0);
    assert(dup2(temp, reused) == reused);
    if (temp != reused)
        close(temp);
    assert(!gpud_ready_add(r, reused, PACHA_FD_EVENT_READABLE, 4, NULL, &s));
    assert(!gpud_ready_next(r, &e));
    assert(!gpud_ready_os_signal(reused));
    e = await(r);
    assert(e.kind == 4 && e.source == s);
    assert(!gpud_ready_remove(&s));
    close(reused);
    assert(!gpud_ready_destroy(r));
    puts("ready semantic/arm/recheck/one-shot/HUP/queued-removal/FD-reuse PASS");
}
struct writer_state {
    int fd;
    unsigned count;
};
static void *writer(void *arg) {
    struct writer_state *w = arg;
    for (unsigned i = 0; i < w->count; i++) {
        assert(!gpud_ready_os_signal(w->fd));
        if (!(i % 7))
            sched_yield();
    }
    return NULL;
}
static void concurrent_delivery(void) {
    struct gpud_ready *r = gpud_ready_create();
    assert(r);
    pacha_fd_t fd;
    assert(!gpud_ready_os_event(&fd));
    struct gpud_ready_source *s = NULL;
    assert(!gpud_ready_add(r, fd, PACHA_FD_EVENT_READABLE, 1, NULL, &s));
    struct writer_state w = {fd, 50000};
    pthread_t t;
    assert(!pthread_create(&t, NULL, writer, &w));
    uint64_t total = 0;
    while (total < w.count) {
        struct gpud_ready_event e = await(r);
        assert(e.source == s);
        uint64_t value;
        assert(read(fd, &value, 8) == 8);
        total += value;
        if (total < w.count)
            assert(!gpud_ready_rearm(s));
    }
    assert(!pthread_join(t, NULL));
    assert(total == w.count);
    assert(!gpud_ready_remove(&s));
    close(fd);
    assert(!gpud_ready_destroy(r));
    r = gpud_ready_create();
    assert(r);
    assert(!gpud_ready_os_event(&fd));
    assert(!gpud_ready_add(r, fd, PACHA_FD_EVENT_READABLE, 1, NULL, &s));
    uint64_t full = UINT64_MAX - 1;
    assert(write(gpud_ready_fd(r), &full, 8) == 8);
    assert(!gpud_ready_os_signal(fd));
    /* The injected counter is not a collector publication. Complete a worker
     * arm/recheck before awaiting its real event; its signal sees saturation. */
    assert(!gpud_ready_rearm(s));
    struct gpud_ready_event e = await(r);
    assert(e.source == s);
    assert(!gpud_ready_remove(&s));
    close(fd);
    assert(!gpud_ready_destroy(r));
    /* Retirement with queued, disarmed sources: stack/queue reclamation follows
     * worker join, not just the control acknowledgement. */
    r = gpud_ready_create();
    assert(r);
    struct gpud_ready_source *pending[512] = {0};
    int f[512];
    for (unsigned i = 0; i < 512; i++) {
        pacha_fd_t v;
        assert(!gpud_ready_os_event(&v));
        f[i] = v;
        assert(!gpud_ready_os_signal(v));
        assert(!gpud_ready_add(r, v, PACHA_FD_EVENT_READABLE, 1, NULL, &pending[i]));
    }
    assert(!gpud_ready_destroy(r));
    for (unsigned i = 0; i < 512; i++)
        close(f[i]);
    puts("concurrent 50000 events/coalescing/saturated notification/queued retirement PASS");
}

static void scale(unsigned n) {
    struct gpud_ready *r = gpud_ready_create();
    assert(r);
    struct gpud_ready_source **s = calloc(n ? n : 1, sizeof(*s));
    int *fds = calloc(n ? n : 1, sizeof(*fds));
    assert(s && fds);
    for (unsigned i = 0; i < n; i++) {
        pacha_fd_t fd;
        assert(!gpud_ready_os_event(&fd));
        fds[i] = fd;
        assert(!gpud_ready_add(r, fd, PACHA_FD_EVENT_READABLE, 0, (void *)(uintptr_t)i, &s[i]));
    }
    struct gpud_ready_event e;
    for (unsigned i = 0; i < 100; i++)
        assert(!gpud_ready_next(r, &e));
    unsigned long before_drains = atomic_load(&drain_calls);
    uint64_t begin = ns();
    for (unsigned i = 0; i < 100000; i++)
        assert(!gpud_ready_next(r, &e));
    uint64_t end = ns();
    assert(atomic_load(&drain_calls) == before_drains);
    printf("N=%u idle_owner_ns_per_request=%.2f empty_drains=0\n", n, (end - begin) / 100000.0);
    fflush(stdout);
    /* One change at the end of a large set must not depend on preceding idle FDs. */
    if (n) {
        assert(!gpud_ready_os_signal(fds[n - 1]));
        e = await(r);
        assert(e.source == s[n - 1]);
    }
    for (unsigned i = 0; i < n; i++) {
        assert(!gpud_ready_remove(&s[i]));
        close(fds[i]);
    }
    assert(!gpud_ready_destroy(r));
    free(fds);
    free(s);
}

#ifndef GPUD_READY_HOST_NO_MAIN
int main(void) {
    semantics();
    concurrent_delivery();
    unsigned n[] = {0, 256, 1024, 4096, 16384};
    for (unsigned i = 0; i < 5; i++)
        scale(n[i]);
    puts("GPUD_READY_PASS");
}

#endif

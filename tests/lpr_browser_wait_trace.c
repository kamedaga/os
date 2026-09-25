#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Opt-in diagnostic interposer, never part of the default browser launch.
 * Record metadata only. All cross-thread fields are atomic: a sample can be
 * skipped while changing, but must not dereference another thread's arguments.
 * A long condition/poll wait is often normal, not itself evidence of a bug. */
enum { SLOT_COUNT = 512 };
struct slot {
    _Atomic unsigned seq, kind;
    _Atomic long tid, fd0, fd1, timeout;
    _Atomic uintptr_t caller, object;
    _Atomic long started;
};
static struct slot slots[SLOT_COUNT];
static _Atomic unsigned next_slot;
static _Thread_local struct slot *own;
static _Thread_local int attempted;

static long seconds(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0 ? now.tv_sec : 0;
}

static void enter(unsigned kind, long fd0, long fd1, long timeout,
                  uintptr_t object, void *caller)
{
    int saved = errno;
    if (!attempted) {
        attempted = 1;
        unsigned index = atomic_fetch_add(&next_slot, 1);
        if (index < SLOT_COUNT) {
            own = &slots[index];
            own->tid = syscall(SYS_gettid);
        }
    }
    if (own) {
        atomic_fetch_add(&own->seq, 1);
        own->kind = kind;
        own->fd0 = fd0;
        own->fd1 = fd1;
        own->timeout = timeout;
        own->object = object;
        own->caller = (uintptr_t)caller;
        own->started = seconds();
        atomic_fetch_add(&own->seq, 1);
    }
    errno = saved;
}

static void leave(void)
{
    if (own) own->kind = 0;
}

static void *monitor(void *unused)
{
    (void)unused;
    for (;;) {
        struct timespec delay = { .tv_sec = 15 };
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
        long now = seconds();
        unsigned count = atomic_load(&next_slot);
        if (count > SLOT_COUNT) count = SLOT_COUNT;
        for (unsigned i = 0; i < count; ++i) {
            struct slot *s = &slots[i];
            unsigned seq = s->seq, kind = s->kind;
            long tid = s->tid, fd0 = s->fd0, fd1 = s->fd1;
            long timeout = s->timeout, started = s->started;
            uintptr_t caller = s->caller, object = s->object;
            if ((seq & 1) || seq != s->seq || !kind || now - started < 10)
                continue;
            Dl_info info = {0};
            dladdr((void *)caller, &info);
            char line[768];
            int n = snprintf(line, sizeof(line),
                "BROWSER_WAIT pid=%ld tid=%ld kind=%u age=%ld fd=%ld,%ld timeout=%ld object=%#lx caller=%s+%#lx\n",
                (long)getpid(), tid, kind, now - started, fd0, fd1,
                timeout, (unsigned long)object,
                info.dli_fname ? info.dli_fname : "?",
                (unsigned long)(caller - (uintptr_t)info.dli_fbase));
            if (n > 0) (void)write(2, line,
                (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
        }
    }
    return NULL;
}

__attribute__((constructor)) static void start_monitor(void)
{
    int saved = errno;
    pthread_t thread;
    if (pthread_create(&thread, NULL, monitor, NULL) == 0)
        pthread_detach(thread);
    errno = saved;
}

int poll(struct pollfd *fds, nfds_t count, int timeout)
{
    int (*next)(struct pollfd *, nfds_t, int) = dlsym(RTLD_NEXT, "poll");
    enter(1, count ? fds[0].fd : -1, count > 1 ? fds[1].fd : -1,
          timeout, count, __builtin_return_address(0));
    int result = next(fds, count, timeout);
    leave();
    return result;
}

int ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout,
          const sigset_t *mask)
{
    int (*next)(struct pollfd *, nfds_t, const struct timespec *,
                const sigset_t *) = dlsym(RTLD_NEXT, "ppoll");
    enter(2, count ? fds[0].fd : -1, count > 1 ? fds[1].fd : -1,
          timeout ? timeout->tv_sec : -1, count, __builtin_return_address(0));
    int result = next(fds, count, timeout, mask);
    leave();
    return result;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    int (*next)(pthread_cond_t *, pthread_mutex_t *) =
        dlsym(RTLD_NEXT, "pthread_cond_wait");
    enter(3, -1, -1, -1, (uintptr_t)cond, __builtin_return_address(0));
    int result = next(cond, mutex);
    leave();
    return result;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                          const struct timespec *deadline)
{
    int (*next)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *) =
        dlsym(RTLD_NEXT, "pthread_cond_timedwait");
    enter(5, -1, -1, deadline->tv_sec, (uintptr_t)cond,
          __builtin_return_address(0));
    int result = next(cond, mutex, deadline);
    leave();
    return result;
}

int epoll_wait(int fd, struct epoll_event *events, int count, int timeout)
{
    int (*next)(int, struct epoll_event *, int, int) =
        dlsym(RTLD_NEXT, "epoll_wait");
    enter(6, fd, -1, timeout, count, __builtin_return_address(0));
    int result = next(fd, events, count, timeout);
    leave();
    return result;
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags)
{
    int incoming_errno = errno;
    ssize_t (*next)(int, struct msghdr *, int) = dlsym(RTLD_NEXT, "recvmsg");
    errno = incoming_errno;
    size_t capacity = msg ? msg->msg_controllen : 0;
    enter(4, fd, -1, flags, 0, __builtin_return_address(0));
    ssize_t result = next(fd, msg, flags);
    int saved = errno;
    leave();
    if ((result < 0 && saved != EAGAIN && saved != EINTR) ||
        (result >= 0 && msg && (msg->msg_flags & MSG_CTRUNC))) {
        char line[512];
        int n = snprintf(line, sizeof(line),
            "BROWSER_RECV pid=%ld tid=%ld fd=%d result=%ld errno=%d input=%#x output=%#x control=%zu/%zu\n",
            (long)getpid(), (long)syscall(SYS_gettid), fd, (long)result,
            saved, flags, msg ? msg->msg_flags : 0,
            msg ? (size_t)msg->msg_controllen : 0, capacity);
        if (n > 0) (void)write(2, line,
            (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    }
    errno = saved;
    return result;
}

/* SPDX-License-Identifier: MIT */
/* Optional LD_PRELOAD observation: delegate unchanged to libc. Only record
 * outermost calls on the GTK thread, either in paint or in the whole window. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <glib.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

static _Thread_local unsigned painting, depth;
static _Thread_local int whole_thread;
static _Thread_local gint64 window_end;
struct io_row {
    const char *name;
    int fd;
    gint64 start, end;
    int64_t result;
    unsigned events, revents;
    int64_t timeout; /* poll: milliseconds; ppoll: nanoseconds. */
    nfds_t nfds;
};
static struct io_row rows[32768];
static size_t used, dropped;

void frame_io_phase(int enabled) { painting = enabled; }
void frame_io_window(gint64 end, int all) { window_end = end; whole_thread = all; }

static gint64 observe_start(void) {
    if (depth || !window_end || (!painting && !whole_thread)) return 0;
    gint64 now = g_get_monotonic_time();
    return now < window_end ? now : 0;
}

void frame_io_dump(FILE *file) {
    fprintf(file, "# io_count=%zu io_dropped=%zu\n", used, dropped);
    for (size_t i = 0; i < used; ++i) {
        const struct io_row *r = &rows[i];
        fprintf(file, "# io,%s,%d,%" PRId64 ",%" PRId64 ",%" PRId64 ",%u,%u,%" PRId64 ",%zu\n",
                r->name, r->fd, r->start, r->end, r->result, r->events, r->revents,
                r->timeout, (size_t)r->nfds);
    }
}

#define OBSERVE(ret, function, params, args, descriptor)                    \
ret function params {                                                     \
    static _Thread_local __typeof__(&function) next;                        \
    if (!next) {                                                          \
        next = (__typeof__(&function))dlsym(RTLD_NEXT, #function);           \
        if (!next) abort();                                               \
    }                                                                     \
    gint64 start = observe_start();                                       \
    if (!start) return next args;                                         \
    ++depth;                                                              \
    ret result = next args;                                               \
    int saved_errno = errno;                                              \
    gint64 end = g_get_monotonic_time();                                   \
    if (used < G_N_ELEMENTS(rows))                                         \
        rows[used++] = (struct io_row){.name = #function, .fd = descriptor,  \
            .start = start, .end = end, .result = result};                  \
    else ++dropped;                                                       \
    --depth;                                                              \
    errno = saved_errno;                                                  \
    return result;                                                        \
}

static void record_poll(const char *name, struct pollfd *fds, nfds_t n,
                        int64_t timeout, gint64 start, gint64 end, int result) {
    /* Only inspect pollfd after success, preserving EFAULT behavior. */
    struct io_row row = {.name = name, .fd = -1, .start = start, .end = end,
        .result = result, .timeout = timeout, .nfds = n};
    if (result >= 0) {
        for (nfds_t i = 0; i < n; ++i) {
            row.events |= fds[i].events;
            row.revents |= fds[i].revents;
            /* For multiple descriptors identify the sole ready FD, not
             * an arbitrary first FD. Otherwise keep the aggregate at -1. */
            if (n == 1 || (result == 1 && fds[i].revents)) row.fd = fds[i].fd;
        }
    }
    if (used < G_N_ELEMENTS(rows))
        rows[used++] = row;
    else ++dropped;
}

int poll(struct pollfd *fds, nfds_t n, int timeout) {
    static _Thread_local __typeof__(&poll) next;
    if (!next && !(next = dlsym(RTLD_NEXT, "poll"))) abort();
    gint64 start = observe_start();
    if (!start) return next(fds, n, timeout);
    ++depth;
    int result = next(fds, n, timeout), saved_errno = errno;
    record_poll("poll", fds, n, timeout, start, g_get_monotonic_time(), result);
    --depth;
    errno = saved_errno;
    return result;
}

int ppoll(struct pollfd *fds, nfds_t n, const struct timespec *timeout,
          const sigset_t *mask) {
    static _Thread_local __typeof__(&ppoll) next;
    if (!next && !(next = dlsym(RTLD_NEXT, "ppoll"))) abort();
    gint64 start = observe_start();
    if (!start) return next(fds, n, timeout, mask);
    ++depth;
    int result = next(fds, n, timeout, mask), saved_errno = errno;
    gint64 end = g_get_monotonic_time();
    int64_t ns = -1;
    /* Dereference only a successfully validated timeout. The libc wrapper
     * preserves the caller's timespec, unlike the raw Linux syscall. */
    if (result >= 0 && timeout)
        ns = timeout->tv_sec > (INT64_MAX-999999999)/1000000000 ? INT64_MAX :
            (int64_t)timeout->tv_sec*1000000000 + timeout->tv_nsec;
    record_poll("ppoll", fds, n, ns, start, end, result);
    --depth;
    errno = saved_errno;
    return result;
}
OBSERVE(ssize_t, writev, (int fd, const struct iovec *iov, int n), (fd, iov, n), fd)
OBSERVE(ssize_t, write, (int fd, const void *data, size_t n), (fd, data, n), fd)
OBSERVE(ssize_t, read, (int fd, void *data, size_t n), (fd, data, n), fd)
OBSERVE(ssize_t, recv, (int fd, void *data, size_t n, int flags), (fd, data, n, flags), fd)
OBSERVE(ssize_t, recvmsg, (int fd, struct msghdr *msg, int flags), (fd, msg, flags), fd)
OBSERVE(ssize_t, sendmsg, (int fd, const struct msghdr *msg, int flags), (fd, msg, flags), fd)
OBSERVE(int, pthread_cond_wait, (pthread_cond_t *cond, pthread_mutex_t *mutex),
        (cond, mutex), -1)

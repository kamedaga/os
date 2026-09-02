#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

typedef void *(*loader_new_fn)(void *file);
typedef void (*loader_set_sandbox_fn)(void *loader, int selector);
typedef void *(*loader_load_fn)(void *loader, void **error);
typedef void *(*image_get_frame_fn)(void *image, void *request, void **error);
typedef int (*posix_spawnp_fn)(pid_t *, const char *,
                              const posix_spawn_file_actions_t *,
                              const posix_spawnattr_t *, char *const[],
                              char *const[]);
typedef pid_t (*fork_fn)(void);
typedef int (*socketpair_fn)(int, int, int, int[2]);
typedef int (*poll_fn)(struct pollfd *, nfds_t, int);
typedef int (*ppoll_fn)(struct pollfd *, nfds_t, const struct timespec *,
                        const sigset_t *);
typedef int (*epoll_wait_fn)(int, struct epoll_event *, int, int);
typedef int (*epoll_pwait_fn)(int, struct epoll_event *, int, int,
                              const sigset_t *);
typedef ssize_t (*send_fn)(int, const void *, size_t, int);
typedef ssize_t (*recv_fn)(int, void *, size_t, int);
typedef ssize_t (*sendmsg_fn)(int, const struct msghdr *, int);
typedef ssize_t (*recvmsg_fn)(int, struct msghdr *, int);
typedef ssize_t (*read_fn)(int, void *, size_t);
typedef ssize_t (*write_fn)(int, const void *, size_t);
typedef ssize_t (*writev_fn)(int, const struct iovec *, int);

static loader_new_fn real_loader_new;
static loader_set_sandbox_fn real_loader_set_sandbox;
static loader_load_fn real_loader_load;
static image_get_frame_fn real_image_get_frame;
static posix_spawnp_fn real_posix_spawnp;
static fork_fn real_fork;
static socketpair_fn real_socketpair;
static poll_fn real_poll;
static ppoll_fn real_ppoll;
static epoll_wait_fn real_epoll_wait;
static epoll_pwait_fn real_epoll_pwait;
static send_fn real_send;
static recv_fn real_recv;
static sendmsg_fn real_sendmsg;
static recvmsg_fn real_recvmsg;
static read_fn real_read;
static write_fn real_write;
static writev_fn real_writev;
static uint64_t trace_origin_ns;
static unsigned trace_sequence;

static uint64_t monotonic_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

static void emit(const char *operation, const char *phase, const void *object,
                 const void *result, int selector, uint64_t begin_ns)
{
    const uint64_t now = monotonic_ns();
    const unsigned sequence =
        __atomic_fetch_add(&trace_sequence, 1u, __ATOMIC_RELAXED);
    char line[320];
    const int length = snprintf(
        line, sizeof(line),
        "GLYCIN_TIMING n=%u pid=%ld tid=%ld rel_ms=%.3f op=%s phase=%s "
        "object=%p result=%p selector=%d duration_ms=%.3f errno=%d\n",
        sequence, (long)syscall(SYS_getpid), (long)syscall(SYS_gettid),
        (double)(now - trace_origin_ns) / 1000000.0,
        operation, phase, object, result, selector,
        begin_ns == 0 ? 0.0 : (double)(now - begin_ns) / 1000000.0,
        errno);
    if (length <= 0) return;
    const size_t count = (size_t)length < sizeof(line) ?
        (size_t)length : sizeof(line) - 1u;
    /* Bypass the interposed write() below so tracing never traces itself. */
    (void)syscall(SYS_write, STDERR_FILENO, line, count);
}

static void resolve_symbols(void)
{
    if (real_loader_new == NULL) {
        real_loader_new = (loader_new_fn)dlsym(RTLD_NEXT, "gly_loader_new");
    }
    if (real_loader_set_sandbox == NULL) {
        real_loader_set_sandbox = (loader_set_sandbox_fn)dlsym(
            RTLD_NEXT, "gly_loader_set_sandbox_selector");
    }
    if (real_loader_load == NULL) {
        real_loader_load = (loader_load_fn)dlsym(RTLD_NEXT, "gly_loader_load");
    }
    if (real_image_get_frame == NULL) {
        real_image_get_frame = (image_get_frame_fn)dlsym(
            RTLD_NEXT, "gly_image_get_specific_frame");
    }
    if (real_posix_spawnp == NULL) {
        real_posix_spawnp = (posix_spawnp_fn)dlsym(RTLD_NEXT, "posix_spawnp");
    }
    if (real_fork == NULL) {
        real_fork = (fork_fn)dlsym(RTLD_NEXT, "fork");
    }
    if (real_socketpair == NULL) {
        real_socketpair = (socketpair_fn)dlsym(RTLD_NEXT, "socketpair");
    }
    if (real_poll == NULL) {
        real_poll = (poll_fn)dlsym(RTLD_NEXT, "poll");
    }
    if (real_ppoll == NULL) {
        real_ppoll = (ppoll_fn)dlsym(RTLD_NEXT, "ppoll");
    }
    if (real_epoll_wait == NULL) {
        real_epoll_wait = (epoll_wait_fn)dlsym(RTLD_NEXT, "epoll_wait");
    }
    if (real_epoll_pwait == NULL) {
        real_epoll_pwait = (epoll_pwait_fn)dlsym(RTLD_NEXT, "epoll_pwait");
    }
    if (real_send == NULL) {
        real_send = (send_fn)dlsym(RTLD_NEXT, "send");
    }
    if (real_recv == NULL) {
        real_recv = (recv_fn)dlsym(RTLD_NEXT, "recv");
    }
    if (real_sendmsg == NULL) {
        real_sendmsg = (sendmsg_fn)dlsym(RTLD_NEXT, "sendmsg");
    }
    if (real_recvmsg == NULL) {
        real_recvmsg = (recvmsg_fn)dlsym(RTLD_NEXT, "recvmsg");
    }
    if (real_read == NULL) {
        real_read = (read_fn)dlsym(RTLD_NEXT, "read");
    }
    if (real_write == NULL) {
        real_write = (write_fn)dlsym(RTLD_NEXT, "write");
    }
    if (real_writev == NULL) {
        real_writev = (writev_fn)dlsym(RTLD_NEXT, "writev");
    }
}

static void emit_wait(const char *operation, int fd, int result,
                      uint64_t begin_ns)
{
    const uint64_t elapsed = monotonic_ns() - begin_ns;
    if (elapsed < 500000ull) return;
    emit(operation, "end", (const void *)(uintptr_t)(uint32_t)fd,
         (const void *)(intptr_t)result, -1, begin_ns);
}

__attribute__((constructor)) static void timing_trace_init(void)
{
    trace_origin_ns = monotonic_ns();
    resolve_symbols();
    emit("trace", "loaded", NULL, NULL, -1, 0);
}

void *gly_loader_new(void *file)
{
    resolve_symbols();
    if (real_loader_new == NULL) return NULL;
    const uint64_t begin = monotonic_ns();
    emit("loader_new", "begin", file, NULL, -1, 0);
    void *result = real_loader_new(file);
    const int saved_errno = errno;
    emit("loader_new", "end", file, result, -1, begin);
    errno = saved_errno;
    return result;
}

void gly_loader_set_sandbox_selector(void *loader, int selector)
{
    resolve_symbols();
    const uint64_t begin = monotonic_ns();
    emit("set_sandbox", "begin", loader, NULL, selector, 0);
    if (real_loader_set_sandbox != NULL) {
        real_loader_set_sandbox(loader, selector);
    }
    const int saved_errno = errno;
    emit("set_sandbox", "end", loader, NULL, selector, begin);
    errno = saved_errno;
}

void *gly_loader_load(void *loader, void **error)
{
    resolve_symbols();
    if (real_loader_load == NULL) return NULL;
    const uint64_t begin = monotonic_ns();
    emit("loader_load", "begin", loader, NULL, -1, 0);
    void *result = real_loader_load(loader, error);
    const int saved_errno = errno;
    emit("loader_load", "end", loader, result, -1, begin);
    errno = saved_errno;
    return result;
}

void *gly_image_get_specific_frame(void *image, void *request, void **error)
{
    resolve_symbols();
    if (real_image_get_frame == NULL) return NULL;
    const uint64_t begin = monotonic_ns();
    emit("get_frame", "begin", image, NULL, -1, 0);
    void *result = real_image_get_frame(image, request, error);
    const int saved_errno = errno;
    emit("get_frame", "end", image, result, -1, begin);
    errno = saved_errno;
    return result;
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attributes, char *const argv[],
                 char *const envp[])
{
    resolve_symbols();
    if (real_posix_spawnp == NULL) return ENOSYS;
    const uint64_t begin = monotonic_ns();
    const int result = real_posix_spawnp(
        pid, file, actions, attributes, argv, envp);
    const int saved_errno = errno;
    emit("posix_spawnp", "end", file,
         pid != NULL ? (const void *)(uintptr_t)(uint32_t)*pid : NULL,
         result, begin);
    errno = saved_errno;
    return result;
}

pid_t fork(void)
{
    resolve_symbols();
    if (real_fork == NULL) {
        errno = ENOSYS;
        return -1;
    }
    const uint64_t begin = monotonic_ns();
    const pid_t result = real_fork();
    const int saved_errno = errno;
    emit("fork", "end", NULL,
         (const void *)(intptr_t)result, -1, begin);
    errno = saved_errno;
    return result;
}

int socketpair(int domain, int type, int protocol, int pair[2])
{
    resolve_symbols();
    if (real_socketpair == NULL) {
        errno = ENOSYS;
        return -1;
    }
    const uint64_t begin = monotonic_ns();
    const int result = real_socketpair(domain, type, protocol, pair);
    const int saved_errno = errno;
    emit_wait("socketpair", result == 0 ? pair[0] : -1, result, begin);
    errno = saved_errno;
    return result;
}

int poll(struct pollfd *fds, nfds_t count, int timeout)
{
    resolve_symbols();
    if (real_poll == NULL) {
        errno = ENOSYS;
        return -1;
    }
    const uint64_t begin = monotonic_ns();
    const int result = real_poll(fds, count, timeout);
    const int saved_errno = errno;
    emit_wait("poll", count != 0 ? fds[0].fd : -1, result, begin);
    errno = saved_errno;
    return result;
}

int ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout,
          const sigset_t *mask)
{
    resolve_symbols();
    if (real_ppoll == NULL) {
        errno = ENOSYS;
        return -1;
    }
    const uint64_t begin = monotonic_ns();
    const int result = real_ppoll(fds, count, timeout, mask);
    const int saved_errno = errno;
    emit_wait("ppoll", count != 0 ? fds[0].fd : -1, result, begin);
    errno = saved_errno;
    return result;
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
    resolve_symbols();
    if (real_epoll_wait == NULL) {
        errno = ENOSYS;
        return -1;
    }
    const uint64_t begin = monotonic_ns();
    const int result = real_epoll_wait(epfd, events, maxevents, timeout);
    const int saved_errno = errno;
    emit_wait("epoll_wait", epfd, result, begin);
    errno = saved_errno;
    return result;
}

int epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
                int timeout, const sigset_t *mask)
{
    resolve_symbols();
    if (real_epoll_pwait == NULL) {
        errno = ENOSYS;
        return -1;
    }
    const uint64_t begin = monotonic_ns();
    const int result = real_epoll_pwait(
        epfd, events, maxevents, timeout, mask);
    const int saved_errno = errno;
    emit_wait("epoll_pwait", epfd, result, begin);
    errno = saved_errno;
    return result;
}

ssize_t send(int fd, const void *buffer, size_t length, int flags)
{
    resolve_symbols();
    if (real_send == NULL) {
        errno = ENOSYS;
        return -1;
    }
    const uint64_t begin = monotonic_ns();
    const ssize_t result = real_send(fd, buffer, length, flags);
    const int saved_errno = errno;
    emit_wait("send", fd, (int)result, begin);
    errno = saved_errno;
    return result;
}

ssize_t recv(int fd, void *buffer, size_t length, int flags)
{
    resolve_symbols();
    if (real_recv == NULL) {
        errno = ENOSYS;
        return -1;
    }
    const uint64_t begin = monotonic_ns();
    const ssize_t result = real_recv(fd, buffer, length, flags);
    const int saved_errno = errno;
    emit_wait("recv", fd, (int)result, begin);
    errno = saved_errno;
    return result;
}

ssize_t sendmsg(int fd, const struct msghdr *message, int flags)
{
    resolve_symbols();
    if (real_sendmsg == NULL) {
        errno = ENOSYS;
        return -1;
    }
    const uint64_t begin = monotonic_ns();
    const ssize_t result = real_sendmsg(fd, message, flags);
    const int saved_errno = errno;
    emit_wait("sendmsg", fd, (int)result, begin);
    errno = saved_errno;
    return result;
}

ssize_t recvmsg(int fd, struct msghdr *message, int flags)
{
    resolve_symbols();
    if (real_recvmsg == NULL) {
        errno = ENOSYS;
        return -1;
    }
    const uint64_t begin = monotonic_ns();
    const ssize_t result = real_recvmsg(fd, message, flags);
    const int saved_errno = errno;
    emit_wait("recvmsg", fd, (int)result, begin);
    errno = saved_errno;
    return result;
}

ssize_t read(int fd, void *buffer, size_t length)
{
    resolve_symbols();
    if (real_read == NULL) {
        errno = ENOSYS;
        return -1;
    }
    const uint64_t begin = monotonic_ns();
    const ssize_t result = real_read(fd, buffer, length);
    const int saved_errno = errno;
    emit_wait("read", fd, (int)result, begin);
    errno = saved_errno;
    return result;
}

ssize_t write(int fd, const void *buffer, size_t length)
{
    resolve_symbols();
    if (real_write == NULL) {
        errno = ENOSYS;
        return -1;
    }
    const uint64_t begin = monotonic_ns();
    const ssize_t result = real_write(fd, buffer, length);
    const int saved_errno = errno;
    emit_wait("write", fd, (int)result, begin);
    errno = saved_errno;
    return result;
}

ssize_t writev(int fd, const struct iovec *vectors, int count)
{
    resolve_symbols();
    if (real_writev == NULL) {
        errno = ENOSYS;
        return -1;
    }
    const uint64_t begin = monotonic_ns();
    const ssize_t result = real_writev(fd, vectors, count);
    const int saved_errno = errno;
    emit_wait("writev", fd, (int)result, begin);
    errno = saved_errno;
    return result;
}

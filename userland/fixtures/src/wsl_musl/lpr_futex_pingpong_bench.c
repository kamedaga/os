#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define LPR_FUTEX_WAIT_PRIVATE (0 | 128)
#define LPR_FUTEX_WAKE_PRIVATE (1 | 128)

enum {
    DEFAULT_ITERATIONS = 256,
    DEFAULT_TRIALS = 5,
};

static _Atomic uint32_t turn;
static _Atomic uint32_t worker_ready;
static unsigned iterations;
static int socket_pair[2];

typedef struct socket_profile {
    _Atomic uint64_t send_ns;
    _Atomic uint64_t recv_ns;
    _Atomic uint64_t poll_ns;
    _Atomic uint64_t send_calls;
    _Atomic uint64_t recv_calls;
    _Atomic uint64_t recv_eagain;
    _Atomic uint64_t poll_calls;
} socket_profile_t;

static socket_profile_t socket_profile;

static int futex_wait(_Atomic uint32_t *word, uint32_t expected)
{
    for (;;) {
        errno = 0;
        const long result = syscall(
            SYS_futex,
            word,
            LPR_FUTEX_WAIT_PRIVATE,
            expected,
            0,
            0,
            0);
        if (result == 0 || errno == EAGAIN || errno == EINTR) return 0;
        return -1;
    }
}

static int futex_wake(_Atomic uint32_t *word)
{
    errno = 0;
    const long result = syscall(
        SYS_futex,
        word,
        LPR_FUTEX_WAKE_PRIVATE,
        1,
        0,
        0,
        0);
    return result >= 0 ? 0 : -1;
}

static uint64_t monotonic_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000000000ull +
        (uint64_t)value.tv_nsec;
}

static void *worker_main(void *unused)
{
    (void)unused;
    atomic_store_explicit(&worker_ready, 1, memory_order_release);
    for (unsigned i = 0; i < iterations; ++i) {
        while (atomic_load_explicit(&turn, memory_order_acquire) != 1) {
            if (futex_wait(&turn, 0) != 0) return (void *)(intptr_t)1;
        }
        atomic_store_explicit(&turn, 0, memory_order_release);
        if (futex_wake(&turn) != 0) return (void *)(intptr_t)2;
    }
    return 0;
}

static int run_futex_trial(unsigned trial)
{
    pthread_t worker;
    atomic_store_explicit(&turn, 0, memory_order_relaxed);
    atomic_store_explicit(&worker_ready, 0, memory_order_relaxed);
    if (pthread_create(&worker, 0, worker_main, 0) != 0) return 10;
    while (atomic_load_explicit(&worker_ready, memory_order_acquire) == 0) {
        sched_yield();
    }

    const uint64_t begin = monotonic_ns();
    for (unsigned i = 0; i < iterations; ++i) {
        atomic_store_explicit(&turn, 1, memory_order_release);
        if (futex_wake(&turn) != 0) return 11;
        while (atomic_load_explicit(&turn, memory_order_acquire) != 0) {
            if (futex_wait(&turn, 1) != 0) return 12;
        }
    }
    const uint64_t end = monotonic_ns();

    void *worker_result = 0;
    if (pthread_join(worker, &worker_result) != 0 || worker_result != 0) {
        return 13;
    }
    const uint64_t elapsed_ns = end - begin;
    printf(
        "LPR_FUTEX_PINGPONG trial=%u iterations=%u elapsed_ns=%llu "
        "ns_roundtrip=%llu\n",
        trial,
        iterations,
        (unsigned long long)elapsed_ns,
        (unsigned long long)(elapsed_ns / iterations));
    return 0;
}

static int message_send(int fd, const unsigned char *message, size_t length)
{
    size_t done = 0;
    while (done < length) {
        struct iovec vector = {
            .iov_base = (void *)(message + done),
            .iov_len = length - done,
        };
        const struct msghdr header = {
            .msg_iov = &vector,
            .msg_iovlen = 1,
        };
        const uint64_t begin = monotonic_ns();
        const ssize_t result = sendmsg(fd, &header, 0);
        const uint64_t end = monotonic_ns();
        atomic_fetch_add_explicit(
            &socket_profile.send_ns, end - begin, memory_order_relaxed);
        atomic_fetch_add_explicit(
            &socket_profile.send_calls, 1, memory_order_relaxed);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd wait = { .fd = fd, .events = POLLOUT };
            const uint64_t poll_begin = monotonic_ns();
            const int poll_result = poll(&wait, 1, -1);
            const uint64_t poll_end = monotonic_ns();
            atomic_fetch_add_explicit(
                &socket_profile.poll_ns,
                poll_end - poll_begin,
                memory_order_relaxed);
            atomic_fetch_add_explicit(
                &socket_profile.poll_calls, 1, memory_order_relaxed);
            if (poll_result > 0) continue;
        }
        if (result <= 0) return -1;
        done += (size_t)result;
    }
    return 0;
}

static int message_recv(int fd, unsigned char *message, size_t length)
{
    size_t done = 0;
    while (done < length) {
        struct iovec vector = {
            .iov_base = message + done,
            .iov_len = length - done,
        };
        struct msghdr header = {
            .msg_iov = &vector,
            .msg_iovlen = 1,
        };
        const uint64_t begin = monotonic_ns();
        const ssize_t result = recvmsg(fd, &header, 0);
        const uint64_t end = monotonic_ns();
        atomic_fetch_add_explicit(
            &socket_profile.recv_ns, end - begin, memory_order_relaxed);
        atomic_fetch_add_explicit(
            &socket_profile.recv_calls, 1, memory_order_relaxed);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            atomic_fetch_add_explicit(
                &socket_profile.recv_eagain, 1, memory_order_relaxed);
            struct pollfd wait = { .fd = fd, .events = POLLIN };
            const uint64_t poll_begin = monotonic_ns();
            const int poll_result = poll(&wait, 1, -1);
            const uint64_t poll_end = monotonic_ns();
            atomic_fetch_add_explicit(
                &socket_profile.poll_ns,
                poll_end - poll_begin,
                memory_order_relaxed);
            atomic_fetch_add_explicit(
                &socket_profile.poll_calls, 1, memory_order_relaxed);
            if (poll_result > 0) continue;
        }
        if (result <= 0) return -1;
        done += (size_t)result;
    }
    return 0;
}

static void *socket_worker_main(void *unused)
{
    (void)unused;
    unsigned char request[64];
    unsigned char response[64];
    memset(response, 0x5a, sizeof(response));
    atomic_store_explicit(&worker_ready, 1, memory_order_release);
    for (unsigned i = 0; i < iterations; ++i) {
        if (message_recv(socket_pair[1], request, sizeof(request)) != 0) {
            return (void *)(intptr_t)1;
        }
        if (message_send(socket_pair[1], response, sizeof(response)) != 0) {
            return (void *)(intptr_t)2;
        }
    }
    return 0;
}

static int run_socket_trial(unsigned trial)
{
    pthread_t worker;
    unsigned char request[64];
    unsigned char response[64];
    memset(request, 0xa5, sizeof(request));
    memset(&socket_profile, 0, sizeof(socket_profile));
    atomic_store_explicit(&worker_ready, 0, memory_order_relaxed);
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, socket_pair) != 0) {
        return 20;
    }
    if (pthread_create(&worker, 0, socket_worker_main, 0) != 0) return 21;
    while (atomic_load_explicit(&worker_ready, memory_order_acquire) == 0) {
        sched_yield();
    }

    const uint64_t begin = monotonic_ns();
    for (unsigned i = 0; i < iterations; ++i) {
        if (message_send(socket_pair[0], request, sizeof(request)) != 0) return 22;
        if (message_recv(socket_pair[0], response, sizeof(response)) != 0) return 23;
    }
    const uint64_t end = monotonic_ns();

    void *worker_result = 0;
    if (pthread_join(worker, &worker_result) != 0 || worker_result != 0) {
        return 24;
    }
    (void)close(socket_pair[0]);
    (void)close(socket_pair[1]);
    const uint64_t elapsed_ns = end - begin;
    printf(
        "LPR_SOCKET_PINGPONG trial=%u iterations=%u elapsed_ns=%llu "
        "ns_roundtrip=%llu\n",
        trial,
        iterations,
        (unsigned long long)elapsed_ns,
        (unsigned long long)(elapsed_ns / iterations));
    const uint64_t send_calls = atomic_load_explicit(
        &socket_profile.send_calls, memory_order_relaxed);
    const uint64_t recv_calls = atomic_load_explicit(
        &socket_profile.recv_calls, memory_order_relaxed);
    const uint64_t poll_calls = atomic_load_explicit(
        &socket_profile.poll_calls, memory_order_relaxed);
    const uint64_t send_ns = atomic_load_explicit(
        &socket_profile.send_ns, memory_order_relaxed);
    const uint64_t recv_ns = atomic_load_explicit(
        &socket_profile.recv_ns, memory_order_relaxed);
    const uint64_t poll_ns = atomic_load_explicit(
        &socket_profile.poll_ns, memory_order_relaxed);
    printf(
        "LPR_SOCKET_PHASES trial=%u send_calls=%llu send_ns=%llu "
        "send_avg_ns=%llu recv_calls=%llu recv_eagain=%llu recv_ns=%llu "
        "recv_avg_ns=%llu poll_calls=%llu poll_ns=%llu poll_avg_ns=%llu\n",
        trial,
        (unsigned long long)send_calls,
        (unsigned long long)send_ns,
        (unsigned long long)(send_calls != 0 ? send_ns / send_calls : 0),
        (unsigned long long)recv_calls,
        (unsigned long long)atomic_load_explicit(
            &socket_profile.recv_eagain, memory_order_relaxed),
        (unsigned long long)recv_ns,
        (unsigned long long)(recv_calls != 0 ? recv_ns / recv_calls : 0),
        (unsigned long long)poll_calls,
        (unsigned long long)poll_ns,
        (unsigned long long)(poll_calls != 0 ? poll_ns / poll_calls : 0));
    return 0;
}

int main(int argc, char **argv)
{
    unsigned trials = DEFAULT_TRIALS;
    iterations = DEFAULT_ITERATIONS;
    if (argc > 1) iterations = (unsigned)strtoul(argv[1], 0, 10);
    if (argc > 2) trials = (unsigned)strtoul(argv[2], 0, 10);
    if (iterations == 0 || iterations > 100000 || trials == 0 || trials > 100) {
        fprintf(stderr, "usage: %s [iterations 1..100000] [trials 1..100]\n", argv[0]);
        return 2;
    }
    for (unsigned trial = 1; trial <= trials; ++trial) {
        const int status = run_futex_trial(trial);
        if (status != 0) {
            fprintf(stderr, "LPR_FUTEX_PINGPONG_FAILED trial=%u status=%d errno=%d\n",
                    trial, status, errno);
            return status;
        }
    }
    for (unsigned trial = 1; trial <= trials; ++trial) {
        const int status = run_socket_trial(trial);
        if (status != 0) {
            fprintf(stderr, "LPR_SOCKET_PINGPONG_FAILED trial=%u status=%d errno=%d\n",
                    trial, status, errno);
            return status;
        }
    }
    return 0;
}

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
#include <sys/un.h>
#include <sys/wait.h>
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
static char named_socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

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

static int named_socket_child_main(int listener)
{
    unsigned char request[64];
    unsigned char response[64];
    struct sockaddr_un address;
    (void)close(listener);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, named_socket_path, strlen(named_socket_path) + 1);

    const int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) return 1;
    if (connect(socket_fd, (const struct sockaddr *)&address,
                sizeof(address)) != 0) {
        return 2;
    }

    memset(response, 0x5a, sizeof(response));
    for (unsigned i = 0; i < iterations; ++i) {
        if (message_recv(socket_fd, request, sizeof(request)) != 0) return 3;
        if (message_send(socket_fd, response, sizeof(response)) != 0) return 4;
    }
    (void)close(socket_fd);
    return 0;
}

static int run_named_socket_trial(unsigned trial)
{
    unsigned char request[64];
    unsigned char response[64];
    struct sockaddr_un address;
    memset(request, 0xa5, sizeof(request));

    const int path_length = snprintf(named_socket_path, sizeof(named_socket_path),
                                     "/tmp/lpr-futex-pingpong-%ld.sock",
                                     (long)getpid());
    if (path_length < 0 || (size_t)path_length >= sizeof(named_socket_path)) {
        return 30;
    }
    (void)unlink(named_socket_path);

    const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) return 31;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, named_socket_path, strlen(named_socket_path) + 1);
    if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        (void)close(listener);
        return 32;
    }
    if (listen(listener, 1) != 0) {
        (void)close(listener);
        (void)unlink(named_socket_path);
        return 33;
    }

    const pid_t child = fork();
    if (child < 0) {
        (void)close(listener);
        (void)unlink(named_socket_path);
        return 34;
    }
    if (child == 0) _exit(named_socket_child_main(listener));

    for (;;) {
        socket_pair[0] = accept(listener, 0, 0);
        if (socket_pair[0] >= 0) break;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd wait = { .fd = listener, .events = POLLIN };
            if (poll(&wait, 1, -1) > 0) continue;
        }
        return 35;
    }
    (void)close(listener);

    const uint64_t begin = monotonic_ns();
    for (unsigned i = 0; i < iterations; ++i) {
        if (message_send(socket_pair[0], request, sizeof(request)) != 0) return 36;
        if (message_recv(socket_pair[0], response, sizeof(response)) != 0) return 37;
    }
    const uint64_t end = monotonic_ns();

    (void)close(socket_pair[0]);
    (void)unlink(named_socket_path);
    int child_status = 0;
    pid_t wait_result;
    do {
        wait_result = waitpid(child, &child_status, 0);
    } while (wait_result < 0 && errno == EINTR);
    if (wait_result != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        return 38;
    }

    const uint64_t elapsed_ns = end - begin;
    printf(
        "LPR_NAMED_SOCKET_PINGPONG trial=%u iterations=%u elapsed_ns=%llu "
        "ns_roundtrip=%llu\n",
        trial,
        iterations,
        (unsigned long long)elapsed_ns,
        (unsigned long long)(elapsed_ns / iterations));
    return 0;
}

struct bulk_transfer {
    int sender, receiver, type, error;
    uint64_t bytes;
    size_t chunk;
    unsigned char *buffer;
    const unsigned char *pattern;
};

static void *bulk_receiver(void *opaque)
{
    struct bulk_transfer *transfer = opaque;
    unsigned char *buffer = transfer->buffer;
    const unsigned char *expected = transfer->pattern;
    uint64_t total = 0;
    while (total < transfer->bytes) {
        ssize_t count = recv(transfer->receiver, buffer, 65536, 0);
        if (count < 0 && errno == EINTR) continue;
        size_t packet = transfer->bytes - total < transfer->chunk ?
            (size_t)(transfer->bytes - total) : transfer->chunk;
        if (count <= 0 || (uint64_t)count > transfer->bytes - total ||
            (transfer->type != SOCK_STREAM && (size_t)count != packet) ||
            memcmp(buffer, expected, (size_t)count)) {
            transfer->error = count < 0 ? errno : EPROTO;
            /* Keep the descriptors open until join: no recycled FD can be
             * shutdown accidentally, and failed DGRAM receives need an
             * explicit shutdown of the sender's ACK receive direction. */
            shutdown(transfer->sender, SHUT_RDWR);
            return NULL;
        }
        total += (uint64_t)count;
    }
    if (send(transfer->receiver, expected, 1, MSG_NOSIGNAL) != 1) {
        transfer->error = errno ? errno : EIO;
        shutdown(transfer->sender, SHUT_RDWR);
    }
    return NULL;
}

static int bulk_trial(unsigned trial, int type, uint64_t bytes, size_t chunk)
{
    int fds[2];
    if (socketpair(AF_UNIX, type | SOCK_CLOEXEC, 0, fds)) return 1;
    unsigned char *storage = malloc(2u * 65536u);
    if (!storage) { close(fds[0]); close(fds[1]); return 1; }
    struct bulk_transfer transfer = { .sender = fds[0], .receiver = fds[1],
        .type = type, .bytes = bytes, .chunk = chunk,
        .pattern = storage, .buffer = storage + 65536 };
    unsigned char *payload = storage, ack = 0;
    memset(payload, 0xa5, 65536);
    pthread_t thread;
    int status = pthread_create(&thread, NULL, bulk_receiver, &transfer);
    if (status) { close(fds[0]); close(fds[1]); free(storage); return 1; }
    /* Creation is outside the timer. No per-send timer/profiling, no stop-
     * and-wait ACKs: the reader drains concurrently until all bytes verify.
     * Its last ACK is included; close/join and output are outside the timer. */
    uint64_t started = monotonic_ns(), remaining = bytes;
    while (remaining) {
        size_t size = remaining < chunk ? (size_t)remaining : chunk;
        ssize_t count = send(fds[0], payload, size, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || (size_t)count > size || (type != SOCK_STREAM && (size_t)count != size)) {
            status = 1; break;
        }
        remaining -= (uint64_t)count;
    }
    if (!status) {
        ssize_t count;
        do { count = recv(fds[0], &ack, 1, 0); } while (count < 0 && errno == EINTR);
        if (count != 1 || ack != 0xa5) status = 1;
    }
    uint64_t ended = monotonic_ns();
    if (status) { shutdown(fds[0], SHUT_RDWR); shutdown(fds[1], SHUT_RDWR); }
    if (pthread_join(thread, NULL) || transfer.error) status = 1;
    close(fds[0]); close(fds[1]);
    free(storage);
    if (status || !started || ended <= started) {
        fprintf(stderr, "UNIXD_BULK_FAILED type=%d trial=%u receiver_error=%d errno=%d\n",
            type, trial, transfer.error, errno);
        return 1;
    }
    printf("UNIXD_BULK type=%d trial=%u bytes=%llu chunk=%zu elapsed_ns=%llu MiB_s=%.3f verified=1\n",
        type, trial, (unsigned long long)bytes, chunk, (unsigned long long)(ended - started),
        (double)bytes * 1000000000.0 / (double)(ended - started) / 1048576.0);
    return 0;
}

static int bulk_main(int argc, char **argv)
{
    uint64_t options[4] = {64u * 1024u * 1024u, 16384, 3, 0};
    if (argc > 6) return 2;
    for (int i = 2; i < argc; i++) {
        char *end;
        errno = 0;
        options[i - 2] = strtoull(argv[i], &end, 10);
        if (errno || end == argv[i] || *end || argv[i][0] == '-') return 2;
    }
    if (!options[0] || options[0] > 1024u * 1024u * 1024u ||
        !options[1] || options[1] > 65504 || !options[2] || options[2] > 100 ||
        (options[3] && options[3] != SOCK_STREAM && options[3] != SOCK_SEQPACKET && options[3] != SOCK_DGRAM)) {
        fprintf(stderr, "usage: --bulk [bytes 1..1073741824] [chunk 1..65504] [trials 1..100] [type 0=all/1/2/5]\n");
        return 2;
    }
    setbuf(stdout, NULL);
    const int types[] = {SOCK_STREAM, SOCK_SEQPACKET, SOCK_DGRAM};
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (options[3] && options[3] != (uint64_t)types[i]) continue;
        for (unsigned trial = 1; trial <= options[2]; trial++)
            if (bulk_trial(trial, types[i], options[0], (size_t)options[1])) return 1;
    }
    puts("UNIXD_BULK_DONE");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--bulk")) return bulk_main(argc, argv);
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
    for (unsigned trial = 1; trial <= trials; ++trial) {
        const int status = run_named_socket_trial(trial);
        if (status != 0) {
            fprintf(stderr,
                    "LPR_NAMED_SOCKET_PINGPONG_FAILED trial=%u status=%d errno=%d\n",
                    trial, status, errno);
            return status;
        }
    }
    return 0;
}

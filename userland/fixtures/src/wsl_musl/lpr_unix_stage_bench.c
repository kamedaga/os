#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

static uint64_t ticks(void)
{
    unsigned lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock"); exit(1); }
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void require(int ok, const char *what)
{
    if (!ok) { perror(what); exit(1); }
}
enum { SEND, RECEIVE, POLL, CREATE, BIND, LISTEN, CONNECT, ACCEPT, CLOSE, PHASES };
static const char *names[] = { "send", "recv", "poll", "create", "bind", "listen", "connect", "accept", "close" };
struct metric { uint64_t count, ticks; };
static struct metric metrics[PHASES];
static void record(unsigned phase, uint64_t start)
{
    metrics[phase].ticks += ticks() - start;
    metrics[phase].count++;
}
static void transfer(int fd, unsigned char *data, size_t bytes, int writing, int right, int measured)
{
    size_t done = 0;
    while (done < bytes) {
        union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control = {0};
        struct iovec vector = { data + done, bytes - done };
        struct msghdr message = { .msg_iov = &vector, .msg_iovlen = 1 };
        if (right >= 0 && !done) {
            message.msg_control = control.bytes;
            message.msg_controllen = sizeof(control.bytes);
            if (writing) {
                struct cmsghdr *c = CMSG_FIRSTHDR(&message);
                c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
                c->cmsg_len = CMSG_LEN(sizeof(int));
                memcpy(CMSG_DATA(c), &right, sizeof(right));
            }
        }
        const uint64_t start = ticks();
        const ssize_t result = writing ? sendmsg(fd, &message, MSG_NOSIGNAL) : recvmsg(fd, &message, 0);
        if (measured) record(writing ? SEND : RECEIVE, start);
        require(result > 0, writing ? "sendmsg" : "recvmsg");
        if (!writing && right >= 0 && !done) {
            struct cmsghdr *c = CMSG_FIRSTHDR(&message);
            require(c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
                c->cmsg_len == CMSG_LEN(sizeof(int)) && !(message.msg_flags & MSG_CTRUNC), "rights header");
            int received;
            memcpy(&received, CMSG_DATA(c), sizeof(received));
            unsigned char a, b;
            require(pread(received, &a, 1, 0) == 1 && pread(right, &b, 1, 0) == 1 && a == b, "rights contents");
            require(close(received) == 0, "rights close");
        }
        done += result;
    }
}
struct worker { int fd; size_t bytes; unsigned iterations; };
static void *echo_worker(void *opaque)
{
    struct worker *w = opaque;
    unsigned char *buffer = malloc(w->bytes);
    require(buffer != NULL, "worker malloc");
    for (unsigned i = 0; i < w->iterations; i++) {
        transfer(w->fd, buffer, w->bytes, 0, -1, 0);
        transfer(w->fd, buffer, w->bytes, 1, -1, 0);
    }
    free(buffer);
    return NULL;
}
enum { EVICTION_THREADS = 20 };
static pthread_barrier_t eviction_barrier;
static void *eviction_worker(void *opaque)
{
    uint64_t value = (uintptr_t)opaque + 1, received = 0;
    int pair[2];
    int started = pthread_barrier_wait(&eviction_barrier);
    require(started == 0 || started == PTHREAD_BARRIER_SERIAL_THREAD, "pressure start barrier");
    require(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0, "evict pair");
    /* Thread-local clients share one process session / request queue.
     * Exercise concurrent RPC backpressure, not independent cache owners. */
    for (unsigned i = 0; i < 12; i++) {
        transfer(pair[0], (void *)&value, sizeof(value), 1, -1, 0);
        transfer(pair[1], (void *)&received, sizeof(received), 0, -1, 0);
        require(value == received, "evict payload");
        int result = pthread_barrier_wait(&eviction_barrier);
        require(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD, "evict barrier");
        value += EVICTION_THREADS;
    }
    require(close(pair[0]) == 0 && close(pair[1]) == 0, "evict close");
    return NULL;
}
static int thread_pressure_test(void)
{
    pthread_t threads[EVICTION_THREADS];
    require(pthread_barrier_init(&eviction_barrier, NULL, EVICTION_THREADS) == 0, "barrier init");
    for (unsigned i = 0; i < EVICTION_THREADS; i++)
        require(pthread_create(&threads[i], NULL, eviction_worker, (void *)(uintptr_t)i) == 0, "evict thread");
    for (unsigned i = 0; i < EVICTION_THREADS; i++)
        require(pthread_join(threads[i], NULL) == 0, "evict join");
    require(pthread_barrier_destroy(&eviction_barrier) == 0, "barrier destroy");
    puts("UNIX_STAGE_EVICTION=OK threads=20 rounds=12");
    return 0;
}
static ssize_t pipe_byte(int fd, unsigned char *byte, int writing)
{
    ssize_t result;
    do { result = writing ? write(fd, byte, 1) : read(fd, byte, 1); }
    while (result < 0 && errno == EINTR);
    return result;
}
static int eviction_test(void)
{
    /* Separate processes retain independent LPR page caches. Run one RPC
     * sequence at a time: exercise server eviction, not concurrent client
     * allocation pressure or the per-process native FD ceiling. */
    enum { CLIENTS = 20, ROUNDS = 12 };
    int gates[CLIENTS][2], ack[2];
    pid_t children[CLIENTS];
    require(pipe(ack) == 0, "evict ack pipe");
    for (unsigned i = 0; i < CLIENTS; i++) require(pipe(gates[i]) == 0, "evict gate");
    for (unsigned i = 0; i < CLIENTS; i++) {
        children[i] = fork(); require(children[i] >= 0, "evict fork");
        if (!children[i]) {
            close(ack[0]);
            for (unsigned j = 0; j < CLIENTS; j++) {
                close(gates[j][1]);
                if (j != i) close(gates[j][0]);
            }
            int pair[2];
            require(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0, "evict process pair");
            unsigned char byte = i;
            require(pipe_byte(ack[1], &byte, 1) == 1, "evict ready");
            for (unsigned round = 0; round < ROUNDS; round++) {
                require(pipe_byte(gates[i][0], &byte, 0) == 1, "evict go");
                uint64_t value = round * CLIENTS + i, got = UINT64_MAX;
                transfer(pair[0], (void *)&value, sizeof(value), 1, -1, 0);
                transfer(pair[1], (void *)&got, sizeof(got), 0, -1, 0);
                require(value == got, "evict process payload");
                byte = i;
                require(pipe_byte(ack[1], &byte, 1) == 1, "evict done");
            }
            close(pair[0]); close(pair[1]); close(gates[i][0]); close(ack[1]);
            _exit(0);
        }
        unsigned char ready;
        require(pipe_byte(ack[0], &ready, 0) == 1 && ready == i, "evict startup ack");
    }
    close(ack[1]);
    for (unsigned i = 0; i < CLIENTS; i++) close(gates[i][0]);
    for (unsigned round = 0; round < ROUNDS; round++)
        for (unsigned i = 0; i < CLIENTS; i++) {
            unsigned char byte = i;
            require(pipe_byte(gates[i][1], &byte, 1) == 1, "evict dispatch");
            require(pipe_byte(ack[0], &byte, 0) == 1 && byte == i, "evict response");
        }
    for (unsigned i = 0; i < CLIENTS; i++) {
        int status;
        close(gates[i][1]);
        pid_t waited;
        do { waited = waitpid(children[i], &status, 0); } while (waited < 0 && errno == EINTR);
        require(waited == children[i] && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "evict child exit");
    }
    close(ack[0]);
    puts("UNIX_STAGE_EVICTION=OK processes=20 rounds=12");
    return 0;
}
static void timed_close(int fd)
{
    uint64_t start = ticks();
    require(close(fd) == 0, "close");
    record(CLOSE, start);
}
static void connect_pair(int type, unsigned id, int abstract, int out[2])
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof(address.sun_path), "/tmp/unix-stage-%d-%u", getpid(), id);
    socklen_t length = offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) + 1;
    if (abstract) address.sun_path[0] = 0;
    else unlink(address.sun_path);
    uint64_t start = ticks();
    int listener = socket(AF_UNIX, type, 0);
    out[0] = socket(AF_UNIX, type, 0);
    require(listener >= 0 && out[0] >= 0, "socket"); record(CREATE, start);
    start = ticks();
    require(bind(listener, (void *)&address, length) == 0, "bind"); record(BIND, start);
    if (type != SOCK_DGRAM) {
        start = ticks(); require(listen(listener, 4) == 0, "listen"); record(LISTEN, start);
    }
    start = ticks(); require(connect(out[0], (void *)&address, length) == 0, "connect"); record(CONNECT, start);
    if (type == SOCK_DGRAM) out[1] = listener;
    else {
        start = ticks(); out[1] = accept(listener, NULL, NULL);
        require(out[1] >= 0, "accept"); record(ACCEPT, start);
        timed_close(listener);
    }
    if (!abstract) require(unlink(address.sun_path) == 0, "unlink");
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--eviction-test")) return eviction_test();
    if ((argc == 2 || argc == 3) && !strcmp(argv[1], "--thread-pressure-test")) {
        char *end = NULL;
        unsigned long cycles = argc == 3 ? strtoul(argv[2], &end, 10) : 1;
        if (!cycles || cycles > 100 || (end && *end)) return 2;
        for (unsigned long i = 0; i < cycles; i++) thread_pressure_test();
        printf("UNIX_THREAD_PRESSURE=OK threads=20 rounds=12 cycles=%lu\n", cycles);
        return 0;
    }
    if (argc != 6) {
        fprintf(stderr, "usage: %s ready|rtt|poll|epoll|rights|named|abstract type bytes iterations trials\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    int type = atoi(argv[2]);
    size_t bytes = strtoul(argv[3], NULL, 0);
    unsigned iterations = strtoul(argv[4], NULL, 0), trials = strtoul(argv[5], NULL, 0);
    int named = !strcmp(mode, "named"), abstract = !strcmp(mode, "abstract");
    int rtt = !strcmp(mode, "rtt"), polling = !strcmp(mode, "poll"), epolling = !strcmp(mode, "epoll");
    int rights = !strcmp(mode, "rights");
    require((type == SOCK_STREAM || type == SOCK_SEQPACKET || type == SOCK_DGRAM) &&
        bytes >= 8 && bytes <= 16384 && iterations && trials &&
        (named || abstract || rtt || polling || epolling || rights || !strcmp(mode, "ready")), "arguments");
    unsigned char *send_buffer = malloc(bytes), *recv_buffer = malloc(bytes);
    require(send_buffer && recv_buffer, "malloc");
    memset(send_buffer, 0x5a, bytes); memset(recv_buffer, 0, bytes);
    uint64_t start_ns = ns(), start_ticks = ticks();
    struct timespec delay = { .tv_nsec = 200000000 };
    require(nanosleep(&delay, NULL) == 0, "calibration sleep");
    uint64_t end_ticks = ticks(), end_ns = ns();
    printf("UNIX_STAGE_CLOCK pid=%d ticks=%llu ns=%llu\n", getpid(),
        (unsigned long long)(end_ticks - start_ticks), (unsigned long long)(end_ns - start_ns));
    int right = rights ? open("/etc/passwd", O_RDONLY) : -1;
    require(!rights || right >= 0, "rights file");
    for (unsigned trial = 0; trial < trials; trial++) {
        memset(metrics, 0, sizeof(metrics));
        int pair[2] = {-1, -1}, ep = -1;
        if (!named && !abstract) {
            uint64_t start = ticks();
            require(socketpair(AF_UNIX, type, 0, pair) == 0, "socketpair"); record(CREATE, start);
            if (epolling) {
                ep = epoll_create1(0); require(ep >= 0, "epoll create");
                struct epoll_event ev = { .events = EPOLLIN, .data.fd = pair[1] };
                require(epoll_ctl(ep, EPOLL_CTL_ADD, pair[1], &ev) == 0, "epoll add");
            }
        }
        pthread_t thread;
        struct worker worker = { .fd = pair[1], .bytes = bytes, .iterations = iterations };
        if (rtt) require(pthread_create(&thread, NULL, echo_worker, &worker) == 0, "pthread create");
        uint64_t begin = ticks(), begin_ns = ns();
        for (unsigned i = 0; i < iterations; i++) {
            if (named || abstract) connect_pair(type, trial * iterations + i, abstract, pair);
            uint64_t sequence = (uint64_t)trial * iterations + i;
            memcpy(send_buffer, &sequence, sizeof(sequence));
            transfer(pair[0], send_buffer, bytes, 1, right, 1);
            if (polling || epolling) {
                uint64_t start = ticks();
                if (epolling) {
                    struct epoll_event ev;
                    require(epoll_wait(ep, &ev, 1, 0) == 1 && (ev.events & EPOLLIN), "epoll ready");
                } else {
                    struct pollfd p = { .fd = pair[1], .events = POLLIN };
                    require(poll(&p, 1, 0) == 1 && (p.revents & POLLIN), "poll ready");
                }
                record(POLL, start);
            }
            transfer(pair[rtt ? 0 : 1], recv_buffer, bytes, 0, right, 1);
            require(memcmp(send_buffer, recv_buffer, bytes) == 0, "payload sequence");
            if (named || abstract) { timed_close(pair[0]); timed_close(pair[1]); }
        }
        uint64_t elapsed = ticks() - begin, elapsed_ns = ns() - begin_ns;
        if (rtt) require(pthread_join(thread, NULL) == 0, "join");
        if (!named && !abstract) { timed_close(pair[0]); timed_close(pair[1]); }
        if (ep >= 0) timed_close(ep);
        printf("UNIX_STAGE_RESULT pid=%d mode=%s type=%d bytes=%zu iterations=%u trial=%u ticks=%llu ns=%llu\n",
            getpid(), mode, type, bytes, iterations, trial, (unsigned long long)elapsed, (unsigned long long)elapsed_ns);
        for (unsigned p = 0; p < PHASES; p++) if (metrics[p].count)
            printf("UNIX_STAGE_PHASE pid=%d trial=%u phase=%s count=%llu ticks=%llu\n", getpid(), trial,
                names[p], (unsigned long long)metrics[p].count, (unsigned long long)metrics[p].ticks);
    }
    if (right >= 0) require(close(right) == 0, "file close");
    free(send_buffer); free(recv_buffer);
    printf("UNIX_STAGE_DONE pid=%d status=0\n", getpid());
    return 0;
}

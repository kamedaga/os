#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum { THREADS = 8, ROUNDS = 64, BYTES = 8192 };
static int pairs[THREADS][2], receiver, socket_mode;
static atomic_uint failures;

static void *worker(void *opaque)
{
    unsigned index = (unsigned)(uintptr_t)opaque;
    int channel = pairs[index][receiver];
    for (unsigned round = 0; round < ROUNDS; ++round) {
        unsigned char control[CMSG_SPACE(sizeof(int))] = {0};
        char byte = 'x';
        struct iovec vector = { &byte, 1 };
        struct msghdr msg = { .msg_iov = &vector, .msg_iovlen = 1,
            .msg_control = control, .msg_controllen = sizeof(control) };
        int fd = -1, failed = 0;
        uint64_t expected = ((uint64_t)index << 32) | round;
        if (!receiver) {
            if (socket_mode) {
                int data_pair[2];
                if (socketpair(AF_UNIX, SOCK_STREAM, 0, data_pair)) failed = 1;
                else {
                    fd = data_pair[0];
                    if (send(data_pair[1], &expected, sizeof(expected), MSG_NOSIGNAL) != sizeof(expected))
                        failed = 1;
                    close(data_pair[1]);
                }
            } else {
                fd = memfd_create("rights-concurrency", MFD_CLOEXEC);
                if (fd < 0 || ftruncate(fd, BYTES)) failed = 1;
                uint64_t *data = failed ? MAP_FAILED :
                    mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (data == MAP_FAILED) failed = 1;
                else {
                    for (unsigned i = 0; i < BYTES / sizeof(*data); ++i)
                        data[i] = expected ^ i;
                    munmap(data, BYTES);
                }
            }
            struct cmsghdr *header = CMSG_FIRSTHDR(&msg);
            header->cmsg_level = SOL_SOCKET;
            header->cmsg_type = SCM_RIGHTS;
            header->cmsg_len = CMSG_LEN(sizeof(fd));
            memcpy(CMSG_DATA(header), &fd, sizeof(fd));
            ssize_t result = -1;
            if (!failed) do { result = sendmsg(channel, &msg, MSG_NOSIGNAL); }
                while (result < 0 && errno == EINTR);
            if (result != 1) failed = 1;
        } else {
            ssize_t result;
            do { result = recvmsg(channel, &msg, MSG_CMSG_CLOEXEC); }
                while (result < 0 && errno == EINTR);
            struct cmsghdr *header = CMSG_FIRSTHDR(&msg);
            if (result != 1 || byte != 'x' || (msg.msg_flags & MSG_CTRUNC) ||
                !header || header->cmsg_level != SOL_SOCKET ||
                header->cmsg_type != SCM_RIGHTS ||
                header->cmsg_len != CMSG_LEN(sizeof(fd))) failed = 1;
            else memcpy(&fd, CMSG_DATA(header), sizeof(fd));
            if (!failed && fcntl(fd, F_GETFD) != FD_CLOEXEC) failed = 1;
            struct stat st;
            uint64_t first = UINT64_MAX;
            if (socket_mode) {
                if (!failed && (fstat(fd, &st) || !S_ISSOCK(st.st_mode))) failed = 1;
                size_t got = 0;
                while (!failed && got < sizeof(first)) {
                    ssize_t n = read(fd, (char *)&first + got, sizeof(first) - got);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) { failed = 1; break; }
                    got += (size_t)n;
                }
                if (!failed && (first != expected || read(fd, &byte, 1) != 0)) failed = 1;
            } else {
                if (!failed && (fstat(fd, &st) || st.st_size != BYTES ||
                    pread(fd, &first, sizeof(first), 0) != sizeof(first) ||
                    first != expected)) failed = 1;
                const uint64_t *data = failed ? MAP_FAILED :
                    mmap(NULL, BYTES, PROT_READ, MAP_SHARED, fd, 0);
                if (data == MAP_FAILED) failed = 1;
                else {
                    for (unsigned i = 0; i < BYTES / sizeof(*data); ++i)
                        if (data[i] != (expected ^ i)) failed = 1;
                    munmap((void *)data, BYTES);
                }
            }
        }
        if (fd >= 0) close(fd);
        if (failed) {
            fprintf(stderr, "RIGHTS_CONCURRENCY side=%d thread=%u round=%u errno=%d flags=%#x control=%zu\n",
                receiver, index, round, errno, msg.msg_flags, (size_t)msg.msg_controllen);
            atomic_fetch_add(&failures, 1);
            break;
        }
    }
    close(channel);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "socket"))) return 2;
    socket_mode = argc == 2;
    signal(SIGPIPE, SIG_IGN);
    for (unsigned i = 0; i < THREADS; ++i)
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[i])) return 2;
    pid_t child = fork();
    if (child < 0) return 2;
    receiver = child == 0;
    pthread_t threads[THREADS];
    for (unsigned i = 0; i < THREADS; ++i) {
        close(pairs[i][!receiver]);
        if (pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i))
            _exit(2);
    }
    for (unsigned i = 0; i < THREADS; ++i) pthread_join(threads[i], NULL);
    if (receiver) _exit(atomic_load(&failures) ? 1 : 0);
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
        atomic_fetch_add(&failures, 1);
    printf("RIGHTS_CONCURRENCY mode=%s threads=%u rounds=%u failures=%u\n",
        socket_mode ? "socket" : "memfd", THREADS, ROUNDS, atomic_load(&failures));
    return atomic_load(&failures) ? 1 : 0;
}

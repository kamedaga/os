#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef SYS_pipe
#define SYS_pipe 22
#endif
#ifndef SYS_pipe2
#define SYS_pipe2 293
#endif

enum { PIPE_PROBE_CHUNK = 512, PIPE_PROBE_BUF = 8192 };

static unsigned parse_u32_env(const char *name, unsigned fallback) {
    const char *text = getenv(name);
    if (text == NULL || *text == 0) return fallback;
    unsigned value = 0;
    while (*text >= '0' && *text <= '9') {
        value = value * 10u + (unsigned)(*text - '0');
        text++;
    }
    return *text == 0 && value != 0 ? value : fallback;
}

static int close_checked(int fd) {
    if (fd < 0) return 0;
    return close(fd) == 0 ? 0 : 1;
}

static int make_pipe_pair(int fds[2], unsigned index, int extra_flags) {
    fds[0] = -1;
    fds[1] = -1;
    long pipe_status = (index & 1u) == 0 ?
        syscall(SYS_pipe2, fds, O_CLOEXEC | extra_flags) :
        syscall(SYS_pipe, fds);
    if (pipe_status != 0) {
        perror("pipe");
        return 10;
    }
    if ((index & 1u) != 0 && extra_flags != 0) {
        if (fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | extra_flags) != 0 ||
            fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | extra_flags) != 0) {
            perror("fcntl nonblock");
            close_checked(fds[0]);
            close_checked(fds[1]);
            return 11;
        }
    }
    return 0;
}

static int pipe_probe_roundtrip(unsigned index) {
    int fds[2] = { -1, -1 };
    int make_status = make_pipe_pair(fds, index, 0);
    if (make_status != 0) return make_status;

    int read_dup = dup(fds[0]);
    int write_dup = dup(fds[1]);
    if (read_dup < 0 || write_dup < 0) {
        perror("dup");
        close_checked(read_dup);
        close_checked(write_dup);
        close_checked(fds[0]);
        close_checked(fds[1]);
        return 11;
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        perror("epoll_create1");
        close_checked(read_dup);
        close_checked(write_dup);
        close_checked(fds[0]);
        close_checked(fds[1]);
        return 12;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = 0x5049504500000000ULL | (unsigned long long)index;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev) != 0) {
        perror("epoll_ctl");
        close_checked(epfd);
        close_checked(read_dup);
        close_checked(write_dup);
        close_checked(fds[0]);
        close_checked(fds[1]);
        return 13;
    }
    struct epoll_event out_ev;
    if (epoll_wait(epfd, &out_ev, 1, 0) != 0) {
        fprintf(stderr, "pipe_probe: empty pipe was unexpectedly ready\n");
        close_checked(epfd);
        close_checked(read_dup);
        close_checked(write_dup);
        close_checked(fds[0]);
        close_checked(fds[1]);
        return 14;
    }

    char payload[64];
    int payload_len = snprintf(payload, sizeof(payload), "pipe-probe-%u", index);
    if (payload_len <= 0 || payload_len >= (int)sizeof(payload)) return 15;

    if ((index & 1u) == 0) {
        ssize_t n = write(write_dup, payload, (size_t)payload_len);
        if (n != payload_len) {
            perror("write");
            close_checked(epfd);
            close_checked(read_dup);
            close_checked(write_dup);
            close_checked(fds[0]);
            close_checked(fds[1]);
            return 16;
        }
    } else {
        struct iovec iov[2];
        iov[0].iov_base = payload;
        iov[0].iov_len = (size_t)(payload_len / 2);
        iov[1].iov_base = payload + iov[0].iov_len;
        iov[1].iov_len = (size_t)payload_len - iov[0].iov_len;
        ssize_t n = writev(write_dup, iov, 2);
        if (n != payload_len) {
            perror("writev");
            close_checked(epfd);
            close_checked(read_dup);
            close_checked(write_dup);
            close_checked(fds[0]);
            close_checked(fds[1]);
            return 17;
        }
    }

    if (epoll_wait(epfd, &out_ev, 1, 0) != 1) {
        fprintf(stderr, "pipe_probe: readable pipe was not ready\n");
        close_checked(epfd);
        close_checked(read_dup);
        close_checked(write_dup);
        close_checked(fds[0]);
        close_checked(fds[1]);
        return 18;
    }

    char got[64];
    memset(got, 0, sizeof(got));
    ssize_t read_len = 0;
    if ((index & 1u) == 0) {
        read_len = read(read_dup, got, sizeof(got) - 1);
    } else {
        char a[32];
        char b[32];
        memset(a, 0, sizeof(a));
        memset(b, 0, sizeof(b));
        struct iovec iov[2];
        iov[0].iov_base = a;
        iov[0].iov_len = (size_t)(payload_len / 2);
        iov[1].iov_base = b;
        iov[1].iov_len = sizeof(b) - 1;
        read_len = readv(read_dup, iov, 2);
        snprintf(got, sizeof(got), "%s%s", a, b);
    }
    if (read_len != payload_len || memcmp(got, payload, (size_t)payload_len) != 0) {
        fprintf(stderr, "pipe_probe: read mismatch index=%u want=%s got=%s read=%zd\n", index, payload, got, read_len);
        close_checked(epfd);
        close_checked(read_dup);
        close_checked(write_dup);
        close_checked(fds[0]);
        close_checked(fds[1]);
        return 19;
    }

    int close_errors = 0;
    close_errors += close_checked(epfd);
    close_errors += close_checked(read_dup);
    close_errors += close_checked(write_dup);
    close_errors += close_checked(fds[0]);
    close_errors += close_checked(fds[1]);
    return close_errors == 0 ? 0 : 20;
}

static int pipe_probe_fill_drain(unsigned index) {
    int fds[2] = { -1, -1 };
    int make_status = make_pipe_pair(fds, index, O_NONBLOCK);
    if (make_status != 0) return 30;

    char write_buf[PIPE_PROBE_CHUNK];
    char read_buf[PIPE_PROBE_CHUNK];
    for (unsigned i = 0; i < sizeof(write_buf); i++) write_buf[i] = (char)('a' + ((index + i) % 23u));

    unsigned writes = 0;
    unsigned total = 0;
    for (;;) {
        ssize_t n = write(fds[1], write_buf, sizeof(write_buf));
        if (n < 0) {
            if (errno == EAGAIN) break;
            perror("fill write");
            close_checked(fds[0]);
            close_checked(fds[1]);
            return 31;
        }
        if (n == 0) {
            fprintf(stderr, "pipe_probe: zero write during fill\n");
            close_checked(fds[0]);
            close_checked(fds[1]);
            return 32;
        }
        total += (unsigned)n;
        writes++;
        if (total > PIPE_PROBE_BUF) {
            fprintf(stderr, "pipe_probe: fill exceeded expected buffer total=%u\n", total);
            close_checked(fds[0]);
            close_checked(fds[1]);
            return 33;
        }
    }
    if (writes == 0 || total == 0) {
        fprintf(stderr, "pipe_probe: fill made no progress\n");
        close_checked(fds[0]);
        close_checked(fds[1]);
        return 34;
    }

    unsigned drained = 0;
    while (drained < total) {
        ssize_t n = read(fds[0], read_buf, sizeof(read_buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("drain read");
            close_checked(fds[0]);
            close_checked(fds[1]);
            return 35;
        }
        if (n == 0) {
            fprintf(stderr, "pipe_probe: EOF during drain\n");
            close_checked(fds[0]);
            close_checked(fds[1]);
            return 36;
        }
        drained += (unsigned)n;
    }

    int close_errors = 0;
    close_errors += close_checked(fds[0]);
    close_errors += close_checked(fds[1]);
    return close_errors == 0 ? 0 : 37;
}

static int pipe_probe_eof(unsigned index) {
    int fds[2] = { -1, -1 };
    int make_status = make_pipe_pair(fds, index, 0);
    if (make_status != 0) return 40;
    if (close_checked(fds[1]) != 0) {
        close_checked(fds[0]);
        return 41;
    }
    char buf[8];
    ssize_t n = read(fds[0], buf, sizeof(buf));
    if (n != 0) {
        fprintf(stderr, "pipe_probe: EOF read returned %zd errno=%d\n", n, errno);
        close_checked(fds[0]);
        return 42;
    }
    return close_checked(fds[0]) == 0 ? 0 : 43;
}

static int pipe_probe_broken_write(unsigned index) {
    int fds[2] = { -1, -1 };
    int make_status = make_pipe_pair(fds, index, 0);
    if (make_status != 0) return 50;
    if (close_checked(fds[0]) != 0) {
        close_checked(fds[1]);
        return 51;
    }
    errno = 0;
    ssize_t n = write(fds[1], "x", 1);
    if (n >= 0 || errno != EPIPE) {
        fprintf(stderr, "pipe_probe: broken write returned %zd errno=%d\n", n, errno);
        close_checked(fds[1]);
        return 52;
    }
    return close_checked(fds[1]) == 0 ? 0 : 53;
}

static int pipe_probe_fd_pressure(unsigned window) {
    if (window == 0) window = 1;
    if (window > 16) window = 16;
    int fds[16][2];
    for (unsigned i = 0; i < 16; i++) {
        fds[i][0] = -1;
        fds[i][1] = -1;
    }
    unsigned created = 0;
    for (; created < window; created++) {
        if (make_pipe_pair(fds[created], created, 0) != 0) break;
    }
    int extra[2] = { -1, -1 };
    int extra_status = make_pipe_pair(extra, 99, 0);
    if (extra_status == 0) {
        close_checked(extra[0]);
        close_checked(extra[1]);
    }
    for (unsigned i = 0; i < created; i++) {
        close_checked(fds[i][0]);
        close_checked(fds[i][1]);
    }
    if (created == 0) {
        fprintf(stderr, "pipe_probe: fd pressure created no pipes\n");
        return 60;
    }
    return 0;
}

static int pipe_probe_once(unsigned index, unsigned window) {
    switch (index % 5u) {
    case 0: return pipe_probe_roundtrip(index);
    case 1: return pipe_probe_fill_drain(index);
    case 2: return pipe_probe_eof(index);
    case 3: return pipe_probe_broken_write(index);
    default: return pipe_probe_fd_pressure(window);
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    unsigned loops = parse_u32_env("PIPE_PROBE_LOOPS", 16);
    unsigned window = parse_u32_env("PIPE_PROBE_WINDOW", 8);
    unsigned completed = 0;
    for (unsigned i = 0; i < loops; i++) {
        int status = pipe_probe_once(i, window);
        if (status != 0) {
            fprintf(stderr, "pipe_probe: failed index=%u status=%d\n", i, status);
            return status;
        }
        completed++;
    }
    printf("pipe_probe: ok loops=%u\n", completed);
    return 0;
}

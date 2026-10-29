/* SPDX-License-Identifier: MIT */
/* Run under LPR to cover the legacy syscall used by Java 8's NIO selector. */
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>

int main(void)
{
    const int invalid_sizes[] = {0, -1, (-2147483647 - 1)};
    for (unsigned i = 0; i < sizeof(invalid_sizes) / sizeof(invalid_sizes[0]); ++i) {
        errno = 0;
        const long result = syscall(SYS_epoll_create, invalid_sizes[i]);
        if (result != -1 || errno != EINVAL) {
            fprintf(stderr, "invalid size %d: result=%ld errno=%d\n",
                    invalid_sizes[i], result, errno);
            return 1;
        }
    }

    int epfd = syscall(SYS_epoll_create, 1);
    if (epfd < 0) {
        perror("legacy epoll_create");
        return 2;
    }
    const int flags = fcntl(epfd, F_GETFD);
    if (flags < 0 || (flags & FD_CLOEXEC)) {
        fputs("legacy epoll descriptor must not have CLOEXEC\n", stderr);
        return 3;
    }
    int pipefd[2];
    if (pipe(pipefd)) return 4;
    struct epoll_event event = {
        .events = EPOLLIN, .data.u64 = UINT64_C(0x123456789abcdef0),
    };
    struct epoll_event received = {0};
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pipefd[0], &event)) return 5;
    if (write(pipefd[1], "x", 1) != 1) return 6;
    const int ready = epoll_wait(epfd, &received, 1, 1000);
    if (ready != 1 || !(received.events & EPOLLIN) ||
        received.data.u64 != event.data.u64) {
        fprintf(stderr, "pipe readiness: result=%d errno=%d\n", ready, errno);
        return 7;
    }
    close(pipefd[0]);
    close(pipefd[1]);
    close(epfd);

    /* Linux consumes an int argument, ignoring the upper register bits. */
    epfd = syscall(SYS_epoll_create, UINT64_C(0x100000001));
    if (epfd < 0) return 8;
    close(epfd);
    puts("LEGACY_EPOLL: invalid sizes, int argument, fd flags, pipe readiness and event data PASS");
    return 0;
}

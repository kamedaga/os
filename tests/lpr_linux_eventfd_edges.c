/* Linux parity: eventfd writes generate ET notifications without a read. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

static int edge(int epfd, uint64_t token)
{
    struct epoll_event event;
    return epoll_wait(epfd, &event, 1, 1000) == 1 &&
        event.events == EPOLLIN && event.data.u64 == token &&
        epoll_wait(epfd, &event, 1, 0) == 0;
}

int main(void)
{
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    struct epoll_event event = {.events = EPOLLIN | EPOLLET, .data.u64 = 42};
    if (epfd < 0 || fd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event)) return 1;
    uint64_t one = 1;
    for (unsigned i = 0; i < 3; ++i)
        if (write(fd, &one, sizeof(one)) != sizeof(one) || !edge(epfd, 42)) return 2;
    int alias = dup(fd);
    if (alias < 0 || write(alias, &one, sizeof(one)) != sizeof(one) || !edge(epfd, 42)) return 3;

    /* The producer has no epoll descriptor: progress must live in the shared
     * eventfd object, rather than a producer's local list of epoll interests. */
    pid_t child = fork();
    if (child < 0) return 4;
    if (!child) {
        close(epfd);
        usleep(30000); /* Let the parent arm a blocking epoll wait. */
        _exit(write(alias, &one, sizeof(one)) == sizeof(one) ? 0 : 1);
    }
    int received = edge(epfd, 42), status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) || !received) return 5;
    uint64_t total = 0;
    if (read(fd, &total, sizeof(total)) != sizeof(total) || total != 5) return 6;
    close(alias);
    close(fd);
    close(epfd);
    puts("EVENTFD_REPEATED_EDGE_DUP_FORK_PASS");
    return 0;
}

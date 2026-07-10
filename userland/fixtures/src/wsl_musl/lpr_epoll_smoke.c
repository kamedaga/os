#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void emit(const char *message)
{
    (void)write(1, message, strlen(message));
}

static int fail(const char *message)
{
    emit(message);
    return 0;
}

static int add_interest(int epfd, int fd, uint32_t events, uint64_t data)
{
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.u64 = data;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event);
}

static int mod_interest(int epfd, int fd, uint32_t events, uint64_t data)
{
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.u64 = data;
    return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &event);
}

static int has_event(
    const struct epoll_event *events,
    int count,
    uint64_t data,
    uint32_t mask)
{
    for (int i = 0; i < count; ++i) {
        if (events[i].data.u64 == data && (events[i].events & mask) == mask) {
            return 1;
        }
    }
    return 0;
}

static int mixed_level_ctl_smoke(void)
{
    enum {
        PIPE_DATA = 0x11,
        EVENT_DATA = 0x22,
        SOCKET_DATA = 0x33,
        FILE_DATA = 0x44,
    };
    int pipefd[2] = {-1, -1};
    const int epfd = epoll_create1(EPOLL_CLOEXEC);
    const int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    const int sockfd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    const int filefd = open("/etc/os-release", O_RDONLY | O_CLOEXEC);
    if (epfd < 0 || efd < 0 || sockfd < 0 || filefd < 0 || pipe(pipefd) != 0) {
        return fail("LPR_EPOLL_MIXED=BAD:create\n");
    }
    if ((fcntl(epfd, F_GETFD) & FD_CLOEXEC) == 0) {
        return fail("LPR_EPOLL_CLOEXEC=BAD\n");
    }
    if (add_interest(epfd, pipefd[0], EPOLLIN, PIPE_DATA) != 0 ||
        add_interest(epfd, efd, EPOLLIN, EVENT_DATA) != 0 ||
        add_interest(epfd, sockfd, EPOLLIN, SOCKET_DATA) != 0 ||
        add_interest(epfd, filefd, EPOLLIN, FILE_DATA) != 0)
    {
        return fail("LPR_EPOLL_MIXED=BAD:add\n");
    }

    struct epoll_event events[8];
    memset(events, 0, sizeof(events));
    int count = epoll_wait(epfd, events, 8, 0);
    if (count < 1 || !has_event(events, count, FILE_DATA, EPOLLIN)) {
        return fail("LPR_EPOLL_FILED=BAD\n");
    }
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, filefd, 0) != 0) {
        return fail("LPR_EPOLL_MIXED=BAD:file-del\n");
    }

    const char byte = 'p';
    if (write(pipefd[1], &byte, 1) != 1) {
        return fail("LPR_EPOLL_MIXED=BAD:pipe-write\n");
    }
    count = epoll_wait(epfd, events, 8, 200);
    if (count < 1 || !has_event(events, count, PIPE_DATA, EPOLLIN)) {
        return fail("LPR_EPOLL_MIXED=BAD:pipe-event\n");
    }
    count = epoll_wait(epfd, events, 8, 0);
    if (count < 1 || !has_event(events, count, PIPE_DATA, EPOLLIN)) {
        return fail("LPR_EPOLL_LT=BAD\n");
    }
    char read_byte = 0;
    if (read(pipefd[0], &read_byte, 1) != 1 || read_byte != byte) {
        return fail("LPR_EPOLL_MIXED=BAD:pipe-read\n");
    }

    const uint64_t one = 1;
    uint64_t counter = 0;
    if (write(efd, &one, sizeof(one)) != (ssize_t)sizeof(one)) {
        return fail("LPR_EPOLL_MIXED=BAD:eventfd-write\n");
    }
    count = epoll_wait(epfd, events, 8, 200);
    if (count < 1 || !has_event(events, count, EVENT_DATA, EPOLLIN)) {
        return fail("LPR_EPOLL_MIXED=BAD:eventfd-event\n");
    }
    if (read(efd, &counter, sizeof(counter)) != (ssize_t)sizeof(counter) || counter != one) {
        return fail("LPR_EPOLL_MIXED=BAD:eventfd-read\n");
    }

    if (mod_interest(epfd, efd, EPOLLOUT, EVENT_DATA) != 0) {
        return fail("LPR_EPOLL_MIXED=BAD:eventfd-mod-out\n");
    }
    count = epoll_wait(epfd, events, 8, 0);
    if (count < 1 || !has_event(events, count, EVENT_DATA, EPOLLOUT)) {
        return fail("LPR_EPOLL_MIXED=BAD:eventfd-out\n");
    }
    if (mod_interest(epfd, efd, EPOLLIN, EVENT_DATA) != 0 ||
        mod_interest(epfd, sockfd, EPOLLIN, SOCKET_DATA) != 0)
    {
        return fail("LPR_EPOLL_MIXED=BAD:socket-mod\n");
    }
    struct sockaddr_in unreachable;
    memset(&unreachable, 0, sizeof(unreachable));
    unreachable.sin_family = AF_INET;
    unreachable.sin_port = 1;
    ((unsigned char *)&unreachable.sin_addr.s_addr)[0] = 10;
    ((unsigned char *)&unreachable.sin_addr.s_addr)[1] = 3;
    ((unsigned char *)&unreachable.sin_addr.s_addr)[2] = 0;
    ((unsigned char *)&unreachable.sin_addr.s_addr)[3] = 1;
    errno = 0;
    if (connect(sockfd, (const struct sockaddr *)&unreachable, sizeof(unreachable)) == 0 ||
        errno != ENETUNREACH)
    {
        return fail("LPR_EPOLL_MIXED=BAD:socket-trigger\n");
    }
    count = epoll_wait(epfd, events, 8, 200);
    if (count < 1 || !has_event(events, count, SOCKET_DATA, EPOLLERR)) {
        return fail("LPR_EPOLL_MIXED=BAD:socket-event\n");
    }
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, sockfd, 0) != 0) {
        return fail("LPR_EPOLL_MIXED=BAD:socket-del\n");
    }

    if (epoll_ctl(epfd, EPOLL_CTL_DEL, pipefd[0], 0) != 0 ||
        write(pipefd[1], &byte, 1) != 1 ||
        epoll_wait(epfd, events, 8, 0) != 0)
    {
        return fail("LPR_EPOLL_DEL=BAD\n");
    }

    int edge_pipe[2] = {-1, -1};
    if (pipe(edge_pipe) != 0) {
        return fail("LPR_EPOLL_ET_REJECT=BAD:create\n");
    }
    errno = 0;
    if (add_interest(epfd, edge_pipe[0], EPOLLIN | EPOLLET, 0x55) == 0 || errno != EINVAL) {
        return fail("LPR_EPOLL_ET_REJECT=BAD\n");
    }

    close(edge_pipe[0]);
    close(edge_pipe[1]);
    close(pipefd[0]);
    close(pipefd[1]);
    close(efd);
    close(sockfd);
    close(filefd);
    close(epfd);
    emit("LPR_EPOLL_MIXED=OK\n");
    emit("LPR_EPOLL_LT=OK\n");
    emit("LPR_EPOLL_DEL=OK\n");
    emit("LPR_EPOLL_CLOEXEC=OK\n");
    emit("LPR_EPOLL_ET_REJECT=OK\n");
    return 1;
}

static int timeout_dup_close_smoke(void)
{
    struct epoll_event event;
    const int epfd = epoll_create1(0);
    if (epfd < 0 || epoll_wait(epfd, &event, 1, 0) != 0) {
        return fail("LPR_EPOLL_TIMEOUT=BAD:zero\n");
    }
    if (epoll_pwait(epfd, &event, 1, 25, 0) != 0) {
        return fail("LPR_EPOLL_TIMEOUT=BAD:positive\n");
    }

    const int dup_epfd = dup(epfd);
    int dup_pipe[2] = {-1, -1};
    if (dup_epfd < 0 || pipe(dup_pipe) != 0 ||
        add_interest(dup_epfd, dup_pipe[0], EPOLLIN, 0x66) != 0)
    {
        return fail("LPR_EPOLL_DUP=BAD:add\n");
    }
    const char byte = 'd';
    if (write(dup_pipe[1], &byte, 1) != 1 ||
        epoll_wait(epfd, &event, 1, 100) != 1 ||
        event.data.u64 != 0x66 || (event.events & EPOLLIN) == 0)
    {
        return fail("LPR_EPOLL_DUP=BAD:shared\n");
    }
    char got = 0;
    if (read(dup_pipe[0], &got, 1) != 1) {
        return fail("LPR_EPOLL_DUP=BAD:read\n");
    }

    const int target_alias = dup(dup_pipe[0]);
    if (target_alias < 0) {
        return fail("LPR_EPOLL_CLOSE_AUTO=BAD:dup\n");
    }
    close(dup_pipe[0]);
    if (write(dup_pipe[1], &byte, 1) != 1 ||
        epoll_wait(epfd, &event, 1, 100) != 1 ||
        event.data.u64 != 0x66)
    {
        return fail("LPR_EPOLL_CLOSE_AUTO=BAD:alias\n");
    }
    close(target_alias);
    if (epoll_wait(epfd, &event, 1, 0) != 0) {
        return fail("LPR_EPOLL_CLOSE_AUTO=BAD:last-close\n");
    }

    close(dup_pipe[1]);
    close(dup_epfd);
    close(epfd);
    emit("LPR_EPOLL_TIMEOUT=OK\n");
    emit("LPR_EPOLL_DUP=OK\n");
    emit("LPR_EPOLL_CLOSE_AUTO=OK\n");
    return 1;
}

static int hup_smoke(void)
{
    int pipefd[2] = {-1, -1};
    const int epfd = epoll_create1(0);
    if (epfd < 0 || pipe(pipefd) != 0 ||
        add_interest(epfd, pipefd[0], EPOLLIN, 0x77) != 0)
    {
        return fail("LPR_EPOLL_HUP=BAD:create\n");
    }
    close(pipefd[1]);
    struct epoll_event event;
    const int count = epoll_wait(epfd, &event, 1, 200);
    if (count != 1 || event.data.u64 != 0x77 || (event.events & EPOLLHUP) == 0) {
        return fail("LPR_EPOLL_HUP=BAD:event\n");
    }
    close(pipefd[0]);
    close(epfd);
    emit("LPR_EPOLL_HUP=OK\n");
    return 1;
}

static int fork_infinite_smoke(void)
{
    int pipefd[2] = {-1, -1};
    const int epfd = epoll_create1(0);
    if (epfd < 0 || pipe(pipefd) != 0 ||
        add_interest(epfd, pipefd[0], EPOLLIN, 0x88) != 0)
    {
        return fail("LPR_EPOLL_FORK_INFINITE=BAD:create\n");
    }
    const pid_t child = fork();
    if (child < 0) {
        return fail("LPR_EPOLL_FORK_INFINITE=BAD:fork\n");
    }
    if (child == 0) {
        close(pipefd[1]);
        struct epoll_event event;
        const int count = epoll_wait(epfd, &event, 1, -1);
        char byte = 0;
        const int ok = count == 1 && event.data.u64 == 0x88 &&
            (event.events & EPOLLIN) != 0 && read(pipefd[0], &byte, 1) == 1 && byte == 'f';
        _exit(ok ? 0 : 1);
    }
    close(pipefd[0]);
    (void)sleep(1);
    const char byte = 'f';
    int status = 0;
    if (write(pipefd[1], &byte, 1) != 1 ||
        waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        return fail("LPR_EPOLL_FORK_INFINITE=BAD:wait\n");
    }
    close(pipefd[1]);
    close(epfd);
    emit("LPR_EPOLL_FORK_INFINITE=OK\n");
    return 1;
}

static int cloexec_exec_smoke(const char *self)
{
    const int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        return fail("LPR_EPOLL_CLOEXEC_EXEC=BAD:create\n");
    }
    const pid_t child = fork();
    if (child < 0) {
        close(epfd);
        return fail("LPR_EPOLL_CLOEXEC_EXEC=BAD:fork\n");
    }
    if (child == 0) {
        char fd_text[16];
        (void)snprintf(fd_text, sizeof(fd_text), "%d", epfd);
        char *const child_argv[] = {
            (char *)self,
            (char *)"--check-cloexec",
            fd_text,
            NULL,
        };
        execv(self, child_argv);
        _exit(2);
    }
    int status = 0;
    const int ok = waitpid(child, &status, 0) == child &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0;
    close(epfd);
    if (!ok) {
        return fail("LPR_EPOLL_CLOEXEC_EXEC=BAD:wait\n");
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--check-cloexec") == 0) {
        char *end = NULL;
        const long fd = strtol(argv[2], &end, 10);
        errno = 0;
        if (end != argv[2] && *end == '\0' && fd >= 0 &&
            fcntl((int)fd, F_GETFD) == -1 && errno == EBADF)
        {
            emit("LPR_EPOLL_CLOEXEC_EXEC=OK\n");
            return 0;
        }
        fail("LPR_EPOLL_CLOEXEC_EXEC=BAD:open\n");
        return 1;
    }
    emit("LPR_EPOLL_START\n");
    if (!mixed_level_ctl_smoke() ||
        !timeout_dup_close_smoke() ||
        !hup_smoke() ||
        !fork_infinite_smoke() ||
        !cloexec_exec_smoke(argv[0]))
    {
        return 1;
    }
    emit("LPR_EPOLL_DONE\n");
    return 0;
}

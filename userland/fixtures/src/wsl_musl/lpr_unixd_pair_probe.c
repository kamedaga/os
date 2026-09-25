#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

static int pair[2];
static int listener;
static struct sockaddr_un server_name, client_name;
static socklen_t name_bytes;
static void check(int ok, const char *step)
{
    if (!ok) { printf("UNIXD_PAIR_FAIL step=%s errno=%d\n", step, errno); fflush(stdout); exit(1); }
}
static void *sender(void *unused)
{
    (void)unused;
    usleep(50000);
    check(write(pair[1], "wake", 4) == 4, "thread-write");
    return NULL;
}

static void *named_sender(void *unused)
{
    (void)unused;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(fd >= 0 && bind(fd, (void *)&client_name, name_bytes) == 0, "client-bind");
    usleep(50000);
    check(connect(fd, (void *)&server_name, name_bytes) == 0, "named-connect");
    struct sockaddr_un name;
    socklen_t bytes = sizeof(name);
    check(getpeername(fd, (void *)&name, &bytes) == 0 && bytes == name_bytes &&
        !memcmp(&name, &server_name, name_bytes), "getpeername");
    check(write(fd, "named", 5) == 5 && close(fd) == 0, "named-write-close");
    return NULL;
}

static void *free_backlog(void *close_listener)
{
    usleep(50000);
    if (close_listener) check(close(listener) == 0, "listener-close");
    else {
        int fd = accept(listener, NULL, NULL);
        check(fd >= 0 && close(fd) == 0, "backlog-accept");
    }
    return NULL;
}

static void named_connections(void)
{
    server_name.sun_family = client_name.sun_family = AF_UNIX;
    memcpy(server_name.sun_path + 1, "unixd\0server", 12);
    memcpy(client_name.sun_path + 1, "unixd\0client", 12);
    name_bytes = offsetof(struct sockaddr_un, sun_path) + 13;
    listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(listener >= 0 && bind(listener, (void *)&server_name, name_bytes) == 0 &&
        listen(listener, 1) == 0, "server-bind-listen");
    struct sockaddr_un name = {0};
    socklen_t bytes = 7;
    check(getsockname(listener, (void *)&name, &bytes) == 0 && bytes == name_bytes &&
        !memcmp(&name, &server_name, 7), "truncated-getsockname");
    pthread_t thread;
    check(pthread_create(&thread, NULL, named_sender, NULL) == 0, "named-thread");
    struct pollfd pending = { .fd = listener, .events = POLLIN };
    check(poll(&pending, 1, 1000) == 1 && pending.revents == POLLIN, "listener-poll");
    bytes = sizeof(name);
    int fd = accept4(listener, (void *)&name, &bytes, SOCK_CLOEXEC);
    check(fd >= 0 && bytes == name_bytes && !memcmp(&name, &client_name, bytes), "blocking-accept-peer-name");
    char data[16];
    check(read(fd, data, sizeof(data)) == 5 && !memcmp(data, "named", 5), "named-read");
    check(close(fd) == 0 && pthread_join(thread, NULL) == 0, "named-cleanup");
    puts("UNIXD_NAMED_CONNECT=OK");
    for (int closing = 0; closing < 2; closing++) {
        int first = socket(AF_UNIX, SOCK_STREAM, 0);
        check(first >= 0 && connect(first, (void *)&server_name, name_bytes) == 0, "fill-backlog");
        int nonblock = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        check(nonblock >= 0 && connect(nonblock, (void *)&server_name, name_bytes) == -1 &&
            errno == EAGAIN && close(nonblock) == 0, "full-backlog-nonblock");
        int next = socket(AF_UNIX, SOCK_STREAM, 0);
        check(next >= 0, "backlog-next");
        check(pthread_create(&thread, NULL, free_backlog, closing ? &thread : NULL) == 0, "backlog-thread");
        int result = connect(next, (void *)&server_name, name_bytes);
        check(closing ? result == -1 && errno == ECONNREFUSED : result == 0, "blocked-connect-wake");
        check(pthread_join(thread, NULL) == 0, "backlog-join");
        if (!closing) {
            fd = accept(listener, NULL, NULL);
            check(fd >= 0 && close(fd) == 0, "drain-backlog");
        }
        check(close(first) == 0 && close(next) == 0, "backlog-clients-close");
    }
    puts("UNIXD_BACKLOG_WAKE=OK");
}

static void pathname_connections(void)
{
    const char *path = "/tmp/unixd-path-probe.sock";
    const char *alias = "/tmp/unixd-path-probe-link.sock";
    (void)unlink(path); (void)unlink(alias);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    strcpy(address.sun_path, path);
    socklen_t bytes = offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1;
    int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(server >= 0 && bind(server, (void *)&address, bytes) == 0 && listen(server, 2) == 0, "path-bind-listen");
    struct stat st;
    check(lstat(path, &st) == 0 && S_ISSOCK(st.st_mode), "path-socket-inode");
    check(link(path, alias) == 0, "path-hardlink");
    struct sockaddr_un linked = { .sun_family = AF_UNIX };
    strcpy(linked.sun_path, alias);
    int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(client >= 0 && connect(client, (void *)&linked,
        offsetof(struct sockaddr_un, sun_path) + strlen(alias) + 1) == 0, "path-hardlink-connect");
    int accepted = accept4(server, NULL, NULL, SOCK_CLOEXEC);
    check(accepted >= 0, "path-accept");
    check(unlink(path) == 0 && unlink(alias) == 0, "path-unlink");
    int replacement = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(replacement >= 0 && bind(replacement, (void *)&address, bytes) == 0 &&
        listen(replacement, 1) == 0, "path-rebind-new-inode");
    check(write(client, "old", 3) == 3, "unlinked-write");
    char data[8];
    check(read(accepted, data, sizeof(data)) == 3 && !memcmp(data, "old", 3), "unlinked-old-connection");
    int next = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(next >= 0 && connect(next, (void *)&address, bytes) == 0, "path-replacement-connect");
    int new_accepted = accept4(replacement, NULL, NULL, SOCK_CLOEXEC);
    check(new_accepted >= 0, "path-replacement-accept");
    check(close(new_accepted) == 0 && close(next) == 0 && close(replacement) == 0, "path-replacement-close");
    next = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(next >= 0 && connect(next, (void *)&address, bytes) == -1 && errno == ECONNREFUSED,
        "path-stale-refused");
    check(bind(next, (void *)&address, bytes) == -1 && errno == EADDRINUSE, "path-node-still-exists");
    check(close(next) == 0 && close(client) == 0 && close(accepted) == 0 && close(server) == 0,
        "path-close");
    check(unlink(path) == 0 && chdir("/tmp") == 0, "path-relative-cwd");
    strcpy(address.sun_path, "unixd-path-probe.sock");
    bytes = offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) + 1;
    server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(server >= 0 && client >= 0 && bind(server, (void *)&address, bytes) == 0 &&
        listen(server, 1) == 0 && connect(client, (void *)&address, bytes) == 0, "path-relative-connect");
    accepted = accept4(server, NULL, NULL, SOCK_CLOEXEC);
    check(accepted >= 0 && close(accepted) == 0 && close(client) == 0 && close(server) == 0 &&
        unlink(path) == 0 && chdir("/") == 0, "path-relative-cleanup");
    puts("UNIXD_PATHNAME=OK");
}

static void message_io(void)
{
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "iov-pair");
    struct iovec out[3] = {{"ab", 2}, {NULL, 0}, {"cdef", 4}};
    check(writev(pair[1], out, 3) == 6, "writev");
    char first[2], rest[8];
    struct iovec in[2] = {{first, sizeof(first)}, {rest, sizeof(rest)}};
    check(readv(pair[0], in, 2) == 6 && !memcmp(first, "ab", 2) && !memcmp(rest, "cdef", 4), "readv");
    check(write(pair[1], "one", 3) == 3 && write(pair[1], "two!", 4) == 4, "fionread-fill");
    int bytes = -1;
    check(ioctl(pair[0], FIONREAD, &bytes) == 0 && bytes == 7, "fionread-all-records");
    check(read(pair[0], rest, sizeof(rest)) == 3 && read(pair[0], rest, sizeof(rest)) == 4, "fionread-drain");
    int on = 1;
    check(ioctl(pair[0], FIONBIO, &on) == 0, "fionbio-set");
    int file_flags = fcntl(pair[0], F_GETFL);
    check(file_flags >= 0 && (file_flags & O_NONBLOCK), "fcntl-nonblock-set");
    check(read(pair[0], rest, 1) == -1 && errno == EAGAIN, "fionbio-effective");
    on = 0;
    check(ioctl(pair[0], FIONBIO, &on) == 0, "fionbio-clear");
    file_flags = fcntl(pair[0], F_GETFL);
    check(file_flags >= 0 && !(file_flags & O_NONBLOCK), "fcntl-nonblock-cleared");
    struct ucred peer;
    socklen_t length = sizeof(peer);
    check(getsockopt(pair[0], SOL_SOCKET, SO_PEERCRED, &peer, &length) == 0 &&
        length == sizeof(peer) && peer.pid == getpid() && peer.uid == geteuid() && peer.gid == getegid(), "peer-credentials");
    struct timeval tv = { .tv_usec = 20000 }, copied;
    check(setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0, "receive-timeout-set");
    length = sizeof(copied);
    check(getsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &copied, &length) == 0 && copied.tv_usec == 20000,
        "receive-timeout-get");
    check(read(pair[0], rest, 1) == -1 && errno == EAGAIN, "receive-timeout-wake");
    check(close(pair[0]) == 0 && close(pair[1]) == 0, "iov-close");
    check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair) == 0, "msg-pair");
    struct msghdr send_header = { .msg_iov = out, .msg_iovlen = 3 };
    check(sendmsg(pair[1], &send_header, MSG_NOSIGNAL) == 6, "sendmsg-one-packet");
    in[1].iov_len = 1;
    struct msghdr receive_header = { .msg_iov = in, .msg_iovlen = 2 };
    check(recvmsg(pair[0], &receive_header, MSG_PEEK | MSG_TRUNC) == 6 &&
        (receive_header.msg_flags & MSG_TRUNC) && !memcmp(first, "ab", 2) && rest[0] == 'c', "recvmsg-peek-trunc");
    in[1].iov_len = sizeof(rest);
    check(recvmsg(pair[0], &receive_header, 0) == 6 && receive_header.msg_flags == 0 &&
        !memcmp(first, "ab", 2) && !memcmp(rest, "cdef", 4), "recvmsg-untruncated");
    check(recv(pair[0], rest, 1, 0) == -1 && errno == EAGAIN, "msg-single-record");
    struct iovec invalid[2] = {{"first", 5}, {(void *)1, 1}};
    send_header.msg_iov = invalid; send_header.msg_iovlen = 2;
    check(sendmsg(pair[1], &send_header, 0) == -1 && errno == EFAULT, "iov-invalid-before-publish");
    check(recv(pair[0], rest, 1, 0) == -1 && errno == EAGAIN, "iov-failure-no-partial-packet");
    send_header.msg_iov = NULL; send_header.msg_iovlen = 0;
    check(sendmsg(pair[1], &send_header, 0) == 0 && recvmsg(pair[0], &receive_header, 0) == 0,
        "msg-empty-packet");
    check(close(pair[0]) == 0 && close(pair[1]) == 0, "msg-close");
    puts("UNIXD_MESSAGE_IO=OK");
}

static void readiness_waits(void)
{
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "poll-pair");
    struct pollfd fds[2] = {{ .fd = pair[0], .events = POLLIN }, { .fd = -1 }};
    check(poll(fds, 2, 20) == 0, "poll-timeout");
    pthread_t thread;
    check(pthread_create(&thread, NULL, sender, NULL) == 0, "poll-thread");
    check(poll(fds, 2, 1000) == 1 && fds[0].revents == POLLIN, "poll-blocking-wake");
    char data[8];
    check(read(pair[0], data, sizeof(data)) == 4 && pthread_join(thread, NULL) == 0, "poll-drain");
    check(pthread_create(&thread, NULL, sender, NULL) == 0, "select-thread");
    fd_set readfds;
    FD_ZERO(&readfds); FD_SET(pair[0], &readfds);
    struct timeval timeout = { .tv_sec = 1 };
    check(select(pair[0] + 1, &readfds, NULL, NULL, &timeout) == 1 &&
        FD_ISSET(pair[0], &readfds), "select-wake");
    check(read(pair[0], data, sizeof(data)) == 4 && pthread_join(thread, NULL) == 0, "select-drain");
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event event = { .events = EPOLLIN | EPOLLRDHUP | EPOLLET, .data.u64 = 123 };
    check(ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, pair[0], &event) == 0, "epoll-add");
    check(pthread_create(&thread, NULL, sender, NULL) == 0, "epoll-thread");
    check(epoll_wait(ep, &event, 1, 1000) == 1 && event.events == EPOLLIN &&
        event.data.u64 == 123, "epoll-blocking-wake");
    check(epoll_wait(ep, &event, 1, 20) == 0, "epoll-edge-no-repeat");
    check(read(pair[0], data, sizeof(data)) == 4 && pthread_join(thread, NULL) == 0, "epoll-drain");
    check(pthread_create(&thread, NULL, sender, NULL) == 0, "epoll-refill-thread");
    check(epoll_wait(ep, &event, 1, 1000) == 1 && event.events == EPOLLIN, "epoll-refill-wake");
    check(read(pair[0], data, sizeof(data)) == 4 && pthread_join(thread, NULL) == 0, "epoll-refill-drain");
    check(shutdown(pair[1], SHUT_WR) == 0, "poll-shutdown");
    check(epoll_wait(ep, &event, 1, 1000) == 1 && (event.events & EPOLLRDHUP), "epoll-rdhup");
    check(close(pair[1]) == 0, "poll-peer-close");
    fds[0].events = 0;
    check(poll(fds, 1, 1000) == 1 && (fds[0].revents & POLLHUP), "poll-unconditional-hup");
    check(close(ep) == 0 && close(pair[0]) == 0, "poll-cleanup");
    puts("UNIXD_POLL_EPOLL=OK");
}
static void cross_process_epoll_edges(void)
{
    const int types[] = { SOCK_STREAM, SOCK_SEQPACKET, SOCK_DGRAM };
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        int sockets[2], start[2], finished[2];
        check(socketpair(AF_UNIX, types[i] | SOCK_NONBLOCK, 0, sockets) == 0 &&
            pipe(start) == 0 && pipe(finished) == 0, "remote-edge-setup");
        /* Create epoll only after fork: the reader must not inherit the
         * observer or update it through its own local descriptor table. */
        pid_t child = fork();
        check(child >= 0, "remote-edge-fork");
        if (!child) {
            close(start[1]); close(finished[0]);
            char byte;
            int ok = read(start[0], &byte, 1) == 1 &&
                recv(sockets[0], &byte, 1, 0) == 1 && byte == 'A' &&
                recv(sockets[0], &byte, 1, 0) == -1 && errno == EAGAIN;
            byte = ok ? 'Y' : 'N';
            if (write(finished[1], &byte, 1) != 1) _exit(2);
            _exit(ok ? 0 : 1);
        }
        close(start[0]); close(finished[1]);
        int ep = epoll_create1(EPOLL_CLOEXEC);
        struct epoll_event event = { .events = EPOLLIN | EPOLLET, .data.u64 = 456 };
        check(ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, sockets[0], &event) == 0,
            "remote-edge-add");
        check(send(sockets[1], "A", 1, MSG_NOSIGNAL) == 1 &&
            epoll_wait(ep, &event, 1, 1000) == 1 && event.events == EPOLLIN,
            "remote-edge-first");
        check(epoll_wait(ep, &event, 1, 0) == 0, "remote-edge-no-repeat");
        char byte;
        check(write(start[1], "D", 1) == 1 &&
            read(finished[0], &byte, 1) == 1 && byte == 'Y', "remote-edge-drained");
        int status;
        check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0, "remote-edge-child");
        /* No intervening epoll/poll call may observe the empty state. */
        check(send(sockets[1], "B", 1, MSG_NOSIGNAL) == 1, "remote-edge-refill");
        int count = epoll_wait(ep, &event, 1, 1000);
        printf("UNIXD_REMOTE_EDGE type=%d count=%d events=%u\n",
            types[i], count, count > 0 ? event.events : 0);
        check(count == 1 && event.events == EPOLLIN && event.data.u64 == 456,
            "remote-edge-refill-event");
        check(epoll_wait(ep, &event, 1, 0) == 0, "remote-edge-refill-once");
        check(recv(sockets[0], &byte, 1, 0) == 1 && byte == 'B', "remote-edge-data");
        close(ep); close(sockets[0]); close(sockets[1]);
        close(start[1]); close(finished[0]);
    }
    puts("UNIXD_REMOTE_EDGE=OK");
}

static void cross_process_epoll_write_edges(void)
{
    const int types[] = { SOCK_STREAM, SOCK_SEQPACKET, SOCK_DGRAM };
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        int sockets[2], start[2], finished[2];
        check(socketpair(AF_UNIX, types[i] | SOCK_NONBLOCK, 0, sockets) == 0 &&
            pipe(start) == 0 && pipe(finished) == 0, "remote-write-setup");
        pid_t child = fork();
        check(child >= 0, "remote-write-fork");
        if (!child) {
            close(start[1]); close(finished[0]);
            char byte, payload[512] = {0};
            if (read(start[0], &byte, 1) != 1) _exit(1);
            unsigned count = 0;
            ssize_t sent;
            while ((sent = send(sockets[1], payload, sizeof(payload), MSG_NOSIGNAL)) > 0)
                if (++count > 4096) _exit(2);
            if (!count || sent != -1 || errno != EAGAIN ||
                write(finished[1], "F", 1) != 1 || read(start[0], &byte, 1) != 1)
                _exit(3);
            unsigned drained = 0;
            ssize_t received;
            while ((received = recv(sockets[0], payload, sizeof(payload), 0)) > 0)
                if (++drained > 8192) _exit(4);
            if (!drained || received != -1 || errno != EAGAIN ||
                write(finished[1], "D", 1) != 1) _exit(5);
            _exit(0);
        }
        close(start[0]); close(finished[1]);
        int ep = epoll_create1(EPOLL_CLOEXEC);
        struct epoll_event event = { .events = EPOLLOUT | EPOLLET, .data.u64 = 789 };
        check(ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, sockets[1], &event) == 0 &&
            epoll_wait(ep, &event, 1, 1000) == 1 && event.events == EPOLLOUT,
            "remote-write-initial");
        check(epoll_wait(ep, &event, 1, 0) == 0, "remote-write-no-repeat");
        char byte;
        check(write(start[1], "F", 1) == 1 && read(finished[0], &byte, 1) == 1 &&
            byte == 'F', "remote-write-full");
        check(write(start[1], "D", 1) == 1 && read(finished[0], &byte, 1) == 1 &&
            byte == 'D', "remote-write-drained");
        int status;
        check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0, "remote-write-child");
        int count = epoll_wait(ep, &event, 1, 1000);
        printf("UNIXD_REMOTE_WRITE_EDGE type=%d count=%d events=%u\n",
            types[i], count, count > 0 ? event.events : 0);
        check(count == 1 && event.events == EPOLLOUT && event.data.u64 == 789,
            "remote-write-event");
        check(epoll_wait(ep, &event, 1, 0) == 0, "remote-write-once");
        close(ep); close(sockets[0]); close(sockets[1]);
        close(start[1]); close(finished[0]);
    }
    puts("UNIXD_REMOTE_WRITE_EDGE=OK");
}

static ssize_t send_rights(int socket, const int *fds, unsigned count)
{
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(253 * sizeof(int))]; } control = {0};
    char data = 'R';
    struct iovec vector = { &data, 1 };
    struct msghdr message = { .msg_iov = &vector, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = CMSG_SPACE(count * sizeof(int)) };
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(count * sizeof(int));
    memcpy(CMSG_DATA(header), fds, count * sizeof(int));
    return sendmsg(socket, &message, MSG_NOSIGNAL);
}

static unsigned receive_rights(int socket, int *fds, unsigned capacity, int flags, int truncated)
{
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(253 * sizeof(int))]; } control = {0};
    char data = 0;
    struct iovec vector = { &data, 1 };
    struct msghdr message = { .msg_iov = &vector, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = capacity ? CMSG_LEN(capacity * sizeof(int)) : 0 };
    check(recvmsg(socket, &message, flags) == 1 && data == 'R', "rights-recvmsg");
    check(!!(message.msg_flags & MSG_CTRUNC) == truncated, "rights-ctrunc");
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    if (!header) { check(capacity == 0, "rights-header-missing"); return 0; }
    check(header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS &&
        header->cmsg_len >= CMSG_LEN(0), "rights-header");
    unsigned count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    check(count <= capacity, "rights-count");
    memcpy(fds, CMSG_DATA(header), count * sizeof(int));
    return count;
}

static void pty_rights_io(void)
{
    int sockets[2];
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0, "pty-rights-pair");
    int ends[2];
    ends[0] = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    check(ends[0] >= 0 && grantpt(ends[0]) == 0 && unlockpt(ends[0]) == 0, "pty-master-open");
    char *name = ptsname(ends[0]);
    check(name != NULL, "pty-slave-name");
    ends[1] = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    check(ends[1] >= 0, "pty-slave-open");
    struct termios settings;
    check(tcgetattr(ends[1], &settings) == 0, "pty-getattr");
    cfmakeraw(&settings);
    check(tcsetattr(ends[1], TCSANOW, &settings) == 0, "pty-raw");
    struct stat before[2];
    int number = -1;
    check(ioctl(ends[0], TIOCGPTN, &number) == 0, "pty-number");
    check(fstat(ends[0], &before[0]) == 0 && fstat(ends[1], &before[1]) == 0, "pty-stat-original");
    /* Forward imported descriptors again, closing every sending copy. */
    for (unsigned pass = 0; pass < 2; pass++) {
        check(send_rights(sockets[0], ends, 2) == 1, "pty-rights-send");
        check(close(ends[0]) == 0 && close(ends[1]) == 0, "pty-sender-close");
        check(receive_rights(sockets[1], ends, 2, MSG_CMSG_CLOEXEC, 0) == 2, "pty-rights-receive");
        for (unsigned i = 0; i < 2; i++) {
            struct stat after;
            check(fstat(ends[i], &after) == 0 && S_ISCHR(after.st_mode) &&
                after.st_rdev == before[i].st_rdev, "pty-role-stat");
            check(fcntl(ends[i], F_GETFD) == FD_CLOEXEC, "pty-receive-cloexec");
        }
        int received_number = -1;
        check(ioctl(ends[0], TIOCGPTN, &received_number) == 0 && received_number == number,
            "pty-master-role");
        char bytes[4];
        check(write(ends[0], "in", 2) == 2 && read(ends[1], bytes, sizeof(bytes)) == 2 &&
            !memcmp(bytes, "in", 2), "pty-master-to-slave");
        check(write(ends[1], "out", 3) == 3 && read(ends[0], bytes, sizeof(bytes)) == 3 &&
            !memcmp(bytes, "out", 3), "pty-slave-to-master");
    }
    check(close(ends[0]) == 0 && close(ends[1]) == 0 &&
        close(sockets[0]) == 0 && close(sockets[1]) == 0, "pty-rights-close");
    puts("UNIXD_PTY_RIGHTS=OK");
}

static void rights_io(void)
{
    for (unsigned mode = 0; mode < 2; mode++) {
        int sockets[2];
        check(socketpair(AF_UNIX, (mode ? SOCK_SEQPACKET : SOCK_STREAM) |
            SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0, "rights-pair");
        int source = open("/tmp/unixd-rights-file", O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
        check(source >= 0 && write(source, "escrow", 6) == 6 &&
            lseek(source, 0, SEEK_SET) == 0 && unlink("/tmp/unixd-rights-file") == 0, "rights-file");
        check(send_rights(sockets[0], &source, 1) == 1 && close(source) == 0, "rights-send-close");
        int first[20], second[20];
        check(receive_rights(sockets[1], first, 1, MSG_PEEK | MSG_CMSG_CLOEXEC, 0) == 1,
            "rights-peek");
        check(fcntl(first[0], F_GETFD) == FD_CLOEXEC, "rights-cloexec");
        check(receive_rights(sockets[1], second, 1, 0, 0) == 1 && first[0] != second[0], "rights-peek-independent");
        check(close(first[0]) == 0, "rights-peek-close");
        char data[8] = {0};
        check(read(second[0], data, 6) == 6 && !memcmp(data, "escrow", 6), "rights-after-sender-close");
        source = second[0];
        for (unsigned i = 0; i < 20; i++) first[i] = source;
        check(send_rights(sockets[0], first, 20) == 1, "rights-segmented-send");
        check(receive_rights(sockets[1], second, 20, MSG_CMSG_CLOEXEC, 0) == 20, "rights-segmented-recv");
        for (unsigned i = 0; i < 20; i++) check(close(second[i]) == 0, "rights-segmented-close");
        check(send_rights(sockets[0], first, 3) == 1 &&
            receive_rights(sockets[1], second, 1, 0, 1) == 1 && close(second[0]) == 0, "rights-prefix");
        check(send_rights(sockets[0], first, 3) == 1 &&
            receive_rights(sockets[1], second, 0, 0, 1) == 0, "rights-no-control");
        check(send_rights(sockets[0], first, 3) == 1 && read(sockets[1], data, 1) == 1, "rights-plain-read-discards");
        first[1] = -1;
        check(send_rights(sockets[0], first, 2) == -1 && errno == EBADF, "rights-invalid-fd");
        check(read(sockets[1], data, 1) == -1 && errno == EAGAIN, "rights-invalid-no-payload");
        for (unsigned i = 0; i < 40; i++) {
            check(send_rights(sockets[0], &source, 1) == 1 &&
                receive_rights(sockets[1], second, 1, 0, 0) == 1 && close(second[0]) == 0, "rights-repeat");
        }
        check(close(source) == 0 && close(sockets[0]) == 0 && close(sockets[1]) == 0, "rights-close");
    }
    puts("UNIXD_RIGHTS_IO=OK");
}

static void socket_rights_io(void)
{
    for (unsigned mode = 0; mode < 2; mode++) {
        int control[2], data[2];
        int kind = mode ? SOCK_SEQPACKET : SOCK_STREAM;
        check(socketpair(AF_UNIX, kind | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, control) == 0 &&
            socketpair(AF_UNIX, kind | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, data) == 0, "socket-rights-pairs");
        check(write(data[0], "held", 4) == 4 && send_rights(control[0], &data[1], 1) == 1 &&
            close(data[1]) == 0, "socket-rights-send-close");
        int peek, received;
        check(receive_rights(control[1], &peek, 1, MSG_PEEK | MSG_CMSG_CLOEXEC, 0) == 1 &&
            receive_rights(control[1], &received, 1, MSG_CMSG_CLOEXEC, 0) == 1 && peek != received,
            "socket-rights-peek");
        char bytes[8] = {0};
        check(read(received, bytes, 4) == 4 && !memcmp(bytes, "held", 4), "socket-rights-buffered-input");
        check(write(peek, "back", 4) == 4 && read(data[0], bytes, 4) == 4 && !memcmp(bytes, "back", 4),
            "socket-rights-bidirectional");
        check(fcntl(peek, F_SETFD, 0) == 0 && fcntl(received, F_GETFD) == FD_CLOEXEC,
            "socket-rights-descriptor-flags-independent");
        check(fcntl(received, F_SETFL, 0) == 0 && fcntl(peek, F_GETFL) == O_RDWR,
            "socket-rights-shared-clear-nonblock");
        check(fcntl(peek, F_SETFL, O_NONBLOCK) == 0 && fcntl(received, F_GETFL) == (O_RDWR | O_NONBLOCK) &&
            read(received, bytes, 1) == -1 && errno == EAGAIN, "socket-rights-shared-set-nonblock");
        int ep = epoll_create1(EPOLL_CLOEXEC);
        struct epoll_event event = { .events = EPOLLIN, .data.u64 = 99 };
        check(ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, received, &event) == 0 &&
            write(data[0], "poll", 4) == 4 && epoll_wait(ep, &event, 1, 1000) == 1 &&
            event.data.u64 == 99 && read(peek, bytes, 4) == 4, "socket-rights-epoll");
        check(close(ep) == 0 && close(peek) == 0, "socket-rights-peek-close");
        check(send_rights(control[0], &received, 1) == 1 && close(received) == 0 &&
            write(data[0], "tail", 4) == 4 && close(data[0]) == 0, "socket-rights-reexport-peer-close");
        check(receive_rights(control[1], &received, 1, 0, 0) == 1 && read(received, bytes, 4) == 4 &&
            !memcmp(bytes, "tail", 4) && read(received, bytes, 1) == 0, "socket-rights-closed-peer-attach");
        check(close(received) == 0 && close(control[0]) == 0 && close(control[1]) == 0, "socket-rights-cleanup");
    }
    int control[2];
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) == 0, "listener-rights-pair");
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    memcpy(address.sun_path + 1, "rights-listener", 15);
    socklen_t length = offsetof(struct sockaddr_un, sun_path) + 16;
    int server = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    check(server >= 0 && bind(server, (void *)&address, length) == 0 && listen(server, 2) == 0,
        "listener-rights-bind");
    int file = open("/tmp/unixd-mixed-rights", O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    check(file >= 0 && unlink("/tmp/unixd-mixed-rights") == 0, "mixed-rights-file");
    int sent[3] = { file, server, file }, received[3];
    check(send_rights(control[0], sent, 3) == 1 && close(file) == 0 && close(server) == 0,
        "mixed-rights-send");
    check(receive_rights(control[1], received, 3, MSG_CMSG_CLOEXEC, 0) == 3, "mixed-rights-recv");
    int listening = 0;
    socklen_t size = sizeof(listening);
    check(getsockopt(received[1], SOL_SOCKET, SO_ACCEPTCONN, &listening, &size) == 0 && listening == 1,
        "listener-rights-state");
    int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(client >= 0 && connect(client, (void *)&address, length) == 0, "listener-rights-connect");
    int accepted = accept4(received[1], NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    check(accepted >= 0 && fcntl(accepted, F_GETFL) == (O_RDWR | O_NONBLOCK) &&
        write(client, "live", 4) == 4, "listener-rights-accept");
    char bytes[4];
    check(read(accepted, bytes, 4) == 4 && !memcmp(bytes, "live", 4), "listener-rights-communication");
    for (unsigned i = 0; i < 3; i++) check(close(received[i]) == 0, "mixed-rights-close");
    check(close(accepted) == 0 && close(client) == 0 && close(control[0]) == 0 && close(control[1]) == 0,
        "listener-rights-cleanup");
    puts("UNIXD_SOCKET_RIGHTS=OK");
}

static void *registration_sender(void *opaque)
{
    const int fd = *(int *)opaque;
    usleep(10000);
    check(write(fd, "w", 1) == 1, "registration-delayed-send");
    return NULL;
}

static void wait_registration_lifetime(void)
{
    int control[2];
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) == 0, "registration-control");
    for (unsigned i = 0; i < 8; i++) {
        int data[2], received;
        check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, data) == 0, "registration-pair");
        struct pollfd pending[2] = { { .fd = data[1], .events = POLLIN },
            { .fd = data[1], .events = POLLIN } };
        check(poll(pending, 2, 2) == 0, "registration-warm-duplicate-timeout");
        check(send_rights(control[0], &data[1], 1) == 1 && close(data[1]) == 0,
            "registration-drop-last-owner");
        check(receive_rights(control[1], &received, 1, MSG_CMSG_CLOEXEC, 0) == 1,
            "registration-reimport");
        pthread_t thread;
        check(pthread_create(&thread, NULL, registration_sender, &data[0]) == 0, "registration-thread");
        pending[0] = (struct pollfd){ .fd = received, .events = POLLIN };
        check(poll(pending, 1, 1000) == 1 && pending[0].revents == POLLIN,
            "registration-reimport-wake");
        char byte;
        check(read(received, &byte, 1) == 1 && byte == 'w' && pthread_join(thread, NULL) == 0,
            "registration-reimport-read");
        check(close(received) == 0 && close(data[0]) == 0, "registration-close");
    }
    /* More distinct waits than retained cache entries, without leaking pins
     * or keeping peer endpoints alive after their final close. */
    for (unsigned i = 0; i < 300; i++) {
        int data[2];
        check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, data) == 0, "registration-churn-pair");
        struct pollfd pending = { .fd = data[0], .events = POLLIN };
        check(poll(&pending, 1, 1) == 0 && close(data[0]) == 0, "registration-churn-drop");
        char byte;
        check(read(data[1], &byte, 1) == 0 && close(data[1]) == 0, "registration-no-idle-owner");
    }
    check(close(control[0]) == 0 && close(control[1]) == 0, "registration-control-close");
    puts("UNIXD_WAIT_REGISTRATION_LIFETIME=OK");
}

static int inherited_image(int argc, char **argv)
{
    check(argc == 5, "exec-arguments");
    int fd = atoi(argv[2]), duplicate = atoi(argv[3]), cloexec = atoi(argv[4]);
    check(fcntl(cloexec, F_GETFD) == -1 && errno == EBADF, "exec-cloexec-closed");
    char bytes[8];
    check(read(fd, bytes, 4) == 4 && !memcmp(bytes, "ping", 4), "exec-read");
    check(write(duplicate, "pong", 4) == 4, "exec-write-duplicate");
    int imported[2];
    check(receive_rights(fd, imported, 2, MSG_CMSG_CLOEXEC, 0) == 2, "exec-cross-process-rights");
    check(read(imported[0], bytes, 4) == 4 && !memcmp(bytes, "file", 4), "exec-cross-process-file");
    check(write(imported[1], "scm", 3) == 3 && close(imported[0]) == 0 && close(imported[1]) == 0,
        "exec-cross-process-socket");
    check(close(fd) == 0 && write(duplicate, "last", 4) == 4 && close(duplicate) == 0,
        "exec-duplicate-lifetime");
    puts("UNIXD_EXEC_CHILD=OK");
    return 0;
}

static void fork_exec_io(void)
{
    for (unsigned mode = 0; mode < 2; mode++) {
        int sockets[2];
        check(socketpair(AF_UNIX, mode ? SOCK_SEQPACKET : SOCK_STREAM, 0, sockets) == 0, "fork-pair");
        pid_t child = fork();
        check(child >= 0, "fork-create");
        if (!child) {
            check(close(sockets[0]) == 0, "fork-child-close-peer");
            /* Exercise the child session before exec, not only fresh-image import. */
            check(write(sockets[1], "fork", 4) == 4, "fork-child-write");
            int duplicate = dup(sockets[1]), cloexec = dup(sockets[1]);
            check(duplicate >= 0 && cloexec >= 0 && fcntl(cloexec, F_SETFD, FD_CLOEXEC) == 0, "fork-dup");
            char first[20], second[20], third[20];
            snprintf(first, sizeof(first), "%d", sockets[1]);
            snprintf(second, sizeof(second), "%d", duplicate);
            snprintf(third, sizeof(third), "%d", cloexec);
            char *args[] = { "/cmd/lpr_unixd_pair_probe.elf", "--unix-inherit", first, second, third, NULL };
            execv(args[0], args);
            check(0, "execv");
        }
        check(close(sockets[1]) == 0, "fork-parent-close-peer");
        char bytes[8];
        check(read(sockets[0], bytes, 4) == 4 && !memcmp(bytes, "fork", 4), "fork-parent-read");
        check(write(sockets[0], "ping", 4) == 4 && read(sockets[0], bytes, 4) == 4 &&
            !memcmp(bytes, "pong", 4), "fork-exec-exchange");
        int file = open("/tmp/unixd-cross-process-file", O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
        int data[2];
        check(file >= 0 && write(file, "file", 4) == 4 && lseek(file, 0, SEEK_SET) == 0 &&
            unlink("/tmp/unixd-cross-process-file") == 0 &&
            socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, data) == 0, "fork-cross-process-objects");
        int sent[] = { file, data[1] };
        check(send_rights(sockets[0], sent, 2) == 1 && close(file) == 0 && close(data[1]) == 0,
            "fork-cross-process-send");
        check(read(data[0], bytes, 3) == 3 && !memcmp(bytes, "scm", 3) && close(data[0]) == 0,
            "fork-cross-process-socket-reply");
        check(read(sockets[0], bytes, 4) == 4 && !memcmp(bytes, "last", 4), "fork-exec-final-read");
        int status;
        check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "fork-exec-wait");
        check(read(sockets[0], bytes, 1) == 0 && close(sockets[0]) == 0, "fork-exec-eof");
    }
    puts("UNIXD_FORK_EXEC=OK");
}

struct dgram_delayed {
    int fd, receive;
    struct sockaddr_un destination;
    socklen_t length;
};

static void *dgram_delayed_io(void *opaque)
{
    struct dgram_delayed *work = opaque;
    usleep(30000);
    char buffer[1024];
    if (work->receive) check(recv(work->fd, buffer, sizeof(buffer), 0) == sizeof(buffer), "dgram-release-budget");
    else check(sendto(work->fd, "wake", 4, 0, (void *)&work->destination, work->length) == 4,
        "dgram-delayed-send");
    return NULL;
}

static volatile sig_atomic_t dgram_sigpipes;
static void count_dgram_sigpipe(int signal_number)
{
    (void)signal_number;
    dgram_sigpipes++;
}

static void *dgram_delayed_shutdown(void *opaque)
{
    int *fd = opaque;
    usleep(30000);
    check(shutdown(*fd, SHUT_RDWR) == 0, "dgram-thread-shutdown");
    return NULL;
}

static void datagram_boundaries(void)
{
    struct sigaction action = { .sa_handler = count_dgram_sigpipe }, previous;
    sigemptyset(&action.sa_mask);
    check(sigaction(SIGPIPE, &action, &previous) == 0, "dgram-sigpipe-handler");
    char bytes[1024];
    for (int paired = 0; paired < 2; paired++) for (int how = 0; how < 3; how++) {
        printf("UNIXD_DGRAM_SHUTDOWN pair=%d how=%d\n", paired, how);
        int s[2] = {-1, -1};
        if (paired) check(socketpair(AF_UNIX, SOCK_DGRAM, 0, s) == 0, "dgram-shutdown-pair");
        else check((s[0] = socket(AF_UNIX, SOCK_DGRAM, 0)) >= 0, "dgram-shutdown-unconnected");
        if (paired) check(send(s[1], "queued", 6, 0) == 6, "dgram-shutdown-queue");
        check(shutdown(s[0], how) == 0, "dgram-shutdown");
        struct pollfd p = { .fd = s[0], .events = POLLIN | POLLOUT | POLLRDHUP };
        short expected = POLLOUT | ((paired || how != SHUT_WR) ? POLLIN : 0) |
            (how != SHUT_WR ? POLLRDHUP : 0) | (how == SHUT_RDWR ? POLLHUP : 0);
        check(poll(&p, 1, 0) == 1 && p.revents == expected, "dgram-shutdown-poll");
        int pending = -1;
        check(ioctl(s[0], FIONREAD, &pending) == 0 && pending == (paired ? 6 : 0), "dgram-fionread");
        if (paired) {
            struct sockaddr_un source;
            memset(&source, 0x7f, sizeof(source));
            socklen_t length = sizeof(source);
            check(recvfrom(s[0], bytes, sizeof(bytes), MSG_DONTWAIT, (void *)&source, &length) == 6 &&
                !memcmp(bytes, "queued", 6) && length == 0 && source.sun_family == 0x7f7f,
                "dgram-unbound-source-and-retained-queue");
        }
        check(ioctl(s[0], FIONREAD, &pending) == 0 && pending == 0 &&
            recv(s[0], bytes, sizeof(bytes), MSG_DONTWAIT) == -1 && errno == EAGAIN,
            "dgram-shutdown-nonblocking-not-eof");
        struct timeval timeout = { .tv_usec = 20000 };
        check(setsockopt(s[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
            "dgram-shutdown-timeout");
        if (how != SHUT_WR) check(recv(s[0], bytes, sizeof(bytes), 0) == 0, "dgram-shutdown-blocking-eof");
        if (how != SHUT_RD)
            check(send(s[0], "out", 3, 0) == -1 && errno == EPIPE && dgram_sigpipes == 0,
                "dgram-shutdown-send-no-sigpipe");
        else if (paired) check(send(s[0], "out", 3, 0) == 3 &&
            recv(s[1], bytes, sizeof(bytes), 0) == 3, "dgram-shutdown-other-direction");
        else check(send(s[0], "out", 3, 0) == -1 && errno == ENOTCONN, "dgram-unconnected-send-error");
        if (paired && how != SHUT_WR) check(send(s[1], "in", 2, 0) == -1 && errno == EPIPE &&
            dgram_sigpipes == 0, "dgram-peer-read-shutdown-no-sigpipe");
        if (paired) {
            p = (struct pollfd){ .fd = s[1], .events = POLLIN | POLLOUT | POLLRDHUP };
            check(poll(&p, 1, 0) == 1 && p.revents == POLLOUT, "dgram-peer-not-eof");
            check(close(s[1]) == 0, "dgram-shutdown-peer-close");
        }
        check(close(s[0]) == 0, "dgram-shutdown-close");
    }
    int s[2];
    puts("UNIXD_DGRAM_SHUTDOWN_READER_START");
    check(socketpair(AF_UNIX, SOCK_DGRAM, 0, s) == 0, "dgram-shutdown-waits-pair");
    pthread_t thread;
    check(pthread_create(&thread, NULL, dgram_delayed_shutdown, &s[0]) == 0 &&
        read(s[0], bytes, sizeof(bytes)) == 0 && pthread_join(thread, NULL) == 0,
        "dgram-shutdown-wakes-reader");
    close(s[0]); close(s[1]);
    puts("UNIXD_DGRAM_SHUTDOWN_WRITER_START");
    check(socketpair(AF_UNIX, SOCK_DGRAM, 0, s) == 0, "dgram-shutdown-writer-pair");
    while (send(s[0], bytes, sizeof(bytes), MSG_DONTWAIT) == sizeof(bytes)) {}
    check(errno == EAGAIN, "dgram-shutdown-writer-full");
    check(pthread_create(&thread, NULL, dgram_delayed_shutdown, &s[0]) == 0 &&
        send(s[0], bytes, sizeof(bytes), 0) == -1 && errno == EPIPE &&
        pthread_join(thread, NULL) == 0 && dgram_sigpipes == 0, "dgram-shutdown-wakes-full-writer");
    close(s[0]); close(s[1]);
    check(sigaction(SIGPIPE, &previous, NULL) == 0, "dgram-restore-sigpipe");
    puts("UNIXD_DGRAM_BOUNDARIES=OK");
}

static void *dgram_delayed_disconnect(void *opaque)
{
    int fd = *(int *)opaque;
    struct sockaddr disconnect = { .sa_family = AF_UNSPEC };
    usleep(30000);
    check(connect(fd, &disconnect, sizeof(sa_family_t)) == 0, "dgram-thread-disconnect");
    return NULL;
}

static void datagram_disconnect(void)
{
    int s[3];
    struct sockaddr_un names[3];
    socklen_t length = offsetof(struct sockaddr_un, sun_path) + 32;
    struct sockaddr disconnect = { .sa_family = AF_UNSPEC };
    char bytes[16];
    for (int i = 0; i < 3; i++) {
        check((s[i] = socket(AF_UNIX, SOCK_DGRAM, 0)) >= 0, "dgram-reconnect-socket");
        memset(&names[i], 0, sizeof(names[i]));
        names[i].sun_family = AF_UNIX;
        snprintf(names[i].sun_path + 1, 31, "disconnect-%d-%d", getpid(), i);
        check(bind(s[i], (void *)&names[i], length) == 0, "dgram-reconnect-bind");
    }
    check(sendto(s[1], "old", 3, 0, (void *)&names[0], length) == 3 &&
        connect(s[0], &disconnect, sizeof(sa_family_t)) == 0 &&
        connect(s[0], (void *)&names[1], length) == 0 &&
        connect(s[0], (void *)&names[1], length) == 0 &&
        recv(s[0], bytes, sizeof(bytes), MSG_DONTWAIT) == 3,
        "dgram-first-and-same-connect-retain");
    check(sendto(s[1], "drop", 4, 0, (void *)&names[0], length) == 4 &&
        connect(s[0], (void *)&names[2], length) == 0 &&
        recv(s[0], bytes, sizeof(bytes), MSG_DONTWAIT) == -1 && errno == EAGAIN,
        "dgram-reconnect-purges");
    check(connect(s[1], (void *)&names[0], length) == -1 && errno == EPERM,
        "dgram-connect-reject-other-peer");
    check(sendto(s[2], "drop", 4, 0, (void *)&names[0], length) == 4 &&
        connect(s[0], &disconnect, sizeof(sa_family_t)) == 0 &&
        recv(s[0], bytes, sizeof(bytes), MSG_DONTWAIT) == -1 && errno == EAGAIN,
        "dgram-disconnect-purges");
    check(send(s[0], "x", 1, 0) == -1 && errno == ENOTCONN, "dgram-disconnect-no-peer");
    check(connect(s[0], (void *)&names[0], length) == 0 && send(s[0], "self", 4, 0) == 4 &&
        connect(s[0], &disconnect, sizeof(sa_family_t)) == 0, "dgram-self-disconnect");
    int self_error = -1;
    socklen_t self_length = sizeof(self_error);
    check(getsockopt(s[0], SOL_SOCKET, SO_ERROR, &self_error, &self_length) == 0 && !self_error,
        "dgram-self-disconnect-no-reset");
    for (int i = 0; i < 3; i++) check(close(s[i]) == 0, "dgram-reconnect-close");
    /* A queued descriptor is the last reference to a STREAM endpoint. Its
     * peer must see EOF when AF_UNSPEC discards the datagram, not only when
     * the receiving socket is eventually closed. Repeat to expose leaks. */
    for (unsigned i = 0; i < 64; i++) {
        int control[2], resource[2];
        check(socketpair(AF_UNIX, SOCK_DGRAM, 0, control) == 0 &&
            socketpair(AF_UNIX, SOCK_STREAM, 0, resource) == 0, "dgram-drop-rights-pairs");
        check(send_rights(control[0], &resource[1], 1) == 1 && close(resource[1]) == 0,
            "dgram-drop-rights-send");
        check(recv(resource[0], bytes, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN,
            "dgram-queued-right-keeps-peer");
        check(connect(control[1], &disconnect, sizeof(sa_family_t)) == 0 &&
            recv(resource[0], bytes, 1, MSG_DONTWAIT) == 0, "dgram-disconnect-releases-rights");
        struct pollfd p = { .fd = control[0], .events = POLLOUT };
        check(poll(&p, 1, 0) == 1 && (p.revents & POLLERR), "dgram-discard-pollerr");
        if (i & 1) {
            int duplicate = dup(control[0]), error = 0;
            socklen_t error_length = sizeof(error);
            check(duplicate >= 0 && getsockopt(duplicate, SOL_SOCKET, SO_ERROR, &error, &error_length) == 0 &&
                error == ECONNRESET && close(duplicate) == 0, "dgram-discard-so-error-shared");
        } else check(send(control[0], "error", 5, 0) == -1 && errno == ECONNRESET,
            "dgram-discard-send-error");
        check(poll(&p, 1, 0) == 1 && !(p.revents & POLLERR), "dgram-discard-error-cleared");
        check(send(control[0], "new", 3, 0) == 3 &&
            recv(control[1], bytes, sizeof(bytes), MSG_DONTWAIT) == 3 && !memcmp(bytes, "new", 3),
            "dgram-purged-route-reuse");
        close(control[0]); close(control[1]); close(resource[0]);
    }
    int full[2];
    char packet[1024] = {0};
    pthread_t thread;
    check(socketpair(AF_UNIX, SOCK_DGRAM, 0, full) == 0, "dgram-disconnect-full-pair");
    while (send(full[0], packet, sizeof(packet), MSG_DONTWAIT) == sizeof(packet)) {}
    check(errno == EAGAIN, "dgram-disconnect-full");
    check(pthread_create(&thread, NULL, dgram_delayed_disconnect, &full[1]) == 0 &&
        send(full[0], packet, sizeof(packet), 0) == -1 && errno == ECONNRESET &&
        pthread_join(thread, NULL) == 0, "dgram-disconnect-wakes-full-writer");
    check(send(full[0], "fresh", 5, 0) == 5 &&
        recv(full[1], bytes, sizeof(bytes), MSG_DONTWAIT) == 5 && !memcmp(bytes, "fresh", 5),
        "dgram-disconnect-full-reclaimed");
    close(full[0]); close(full[1]);
    puts("UNIXD_DGRAM_DISCONNECT=OK");
}

static void check_peercred(int fd, pid_t pid)
{
    struct ucred peer = {0};
    socklen_t length = sizeof(peer);
    check(getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) == 0 && length == sizeof(peer) &&
        peer.pid == pid && peer.uid == (pid ? geteuid() : (uid_t)-1) &&
        peer.gid == (pid ? getegid() : (gid_t)-1), "unix-peercred");
}

static void receive_credentials(int fd, pid_t pid, unsigned capacity, int flags, int rights)
{
    union { struct cmsghdr align; char bytes[128]; } control;
    memset(&control, 0x7f, sizeof(control));
    char byte;
    struct iovec vector = { .iov_base = &byte, .iov_len = 1 };
    struct msghdr message = { .msg_iov = &vector, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = capacity };
    check(recvmsg(fd, &message, flags) == 1 && message.msg_controllen <= capacity,
        "unix-credentials-recv");
    unsigned found = 0, files = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        check(c->cmsg_level == SOL_SOCKET, "unix-credentials-level");
        if (c->cmsg_type == SCM_CREDENTIALS) {
            check(pid >= 0 && !found++, "unix-credentials-count");
            struct ucred expected = { .pid = pid, .uid = pid ? getuid() : 65534,
                .gid = pid ? getgid() : 65534 };
            size_t bytes = c->cmsg_len - CMSG_LEN(0);
            if (bytes == sizeof(expected) && memcmp(CMSG_DATA(c), &expected, bytes)) {
                struct ucred actual;
                memcpy(&actual, CMSG_DATA(c), sizeof(actual));
                printf("UNIXD_CREDENTIAL_MISMATCH expected=%d/%u/%u actual=%d/%u/%u capacity=%u\n",
                    expected.pid, expected.uid, expected.gid, actual.pid, actual.uid, actual.gid, capacity);
            }
            check(bytes <= sizeof(expected) && !memcmp(CMSG_DATA(c), &expected, bytes),
                "unix-credentials-value");
        } else if (c->cmsg_type == SCM_RIGHTS) {
            check(c->cmsg_len == CMSG_LEN(sizeof(int)), "unix-credentials-right-size");
            int imported;
            memcpy(&imported, CMSG_DATA(c), sizeof(imported));
            check(fcntl(imported, F_GETFD) >= 0 && close(imported) == 0, "unix-credentials-right-valid");
            files++;
        } else check(0, "unix-credentials-cmsg-type");
    }
    check(found == (unsigned)(pid >= 0 && capacity >= CMSG_LEN(0)) && files == (unsigned)rights,
        "unix-credentials-cmsg-count");
    check(!!(message.msg_flags & MSG_CTRUNC) == (pid >= 0 && capacity < CMSG_LEN(sizeof(struct ucred))),
        "unix-credentials-truncation");
}

static void unix_credentials(void)
{
    const int types[] = { SOCK_STREAM, SOCK_SEQPACKET, SOCK_DGRAM };
    for (unsigned t = 0; t < 3; t++) {
        int s[2], enabled = 1, disabled = 0;
        printf("UNIXD_CREDENTIALS type=%d\n", types[t]);
        check(socketpair(AF_UNIX, types[t], 0, s) == 0, "unix-credentials-pair");
        check_peercred(s[0], getpid()); check_peercred(s[1], getpid());
        check(write(s[0], "x", 1) == 1 &&
            setsockopt(s[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0,
            "unix-passcred-after-queued");
        receive_credentials(s[1], 0, 128, 0, 0); /* no credentials captured at send */
        for (unsigned capacity = 0; capacity <= 36; capacity++) {
            check(write(s[0], "x", 1) == 1, "unix-passcred-write");
            receive_credentials(s[1], getpid(), capacity, 0, 0);
        }
        check(write(s[0], "p", 1) == 1, "unix-passcred-peek-write");
        receive_credentials(s[1], getpid(), 128, MSG_PEEK, 0);
        receive_credentials(s[1], getpid(), 128, 0, 0);
        int resource = open("/etc/passwd", O_RDONLY);
        check(resource >= 0 && send_rights(s[0], &resource, 1) == 1 && close(resource) == 0,
            "unix-credentials-with-rights");
        receive_credentials(s[1], getpid(), 128, 0, 1);
        if (types[t] == SOCK_STREAM) {
            check(write(s[0], "parts", 5) == 5, "unix-credentials-partial-write");
            for (unsigned i = 0; i < 5; i++) receive_credentials(s[1], getpid(), 128, 0, 0);
        }
        pid_t child = fork();
        check(child >= 0, "unix-credentials-fork");
        if (!child) { check(write(s[0], "c", 1) == 1, "unix-child-credentials-write"); _exit(0); }
        receive_credentials(s[1], child, 128, 0, 0);
        int status;
        check(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status),
            "unix-credentials-child-wait");
        check_peercred(s[1], getpid()); /* creator snapshot, not latest writer */
        int duplicate = dup(s[1]);
        check(duplicate >= 0 && setsockopt(duplicate, SOL_SOCKET, SO_PASSCRED, &disabled, sizeof(disabled)) == 0 &&
            close(duplicate) == 0 && write(s[0], "n", 1) == 1, "unix-passcred-dup-disable");
        receive_credentials(s[1], -1, 128, 0, 0);
        close(s[0]); close(s[1]);
    }
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    check(fd >= 0, "unix-peercred-unconnected-create");
    check_peercred(fd, 0);
    close(fd);
    for (unsigned t = 0; t < 2; t++) {
        const int type = t ? SOCK_DGRAM : SOCK_STREAM;
        int listener = socket(AF_UNIX, type, 0), client = socket(AF_UNIX, type, 0), enabled = 1;
        check(listener >= 0 && client >= 0, "unix-credentials-named-sockets");
        struct sockaddr_un address = { .sun_family = AF_UNIX };
        snprintf(address.sun_path + 1, 32, "credential-%d-%u", getpid(), t);
        socklen_t address_length = offsetof(struct sockaddr_un, sun_path) + 33;
        check(bind(listener, (void *)&address, address_length) == 0 &&
            setsockopt(listener, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0 &&
            setsockopt(client, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0,
            "unix-credentials-named-configure");
        if (!t) { check(listen(listener, 2) == 0, "unix-credentials-listen"); check_peercred(listener, getpid()); }
        check(connect(client, (void *)&address, address_length) == 0 && write(client, "n", 1) == 1,
            "unix-credentials-named-send");
        struct sockaddr_un automatic;
        socklen_t automatic_length = sizeof(automatic);
        check(getsockname(client, (void *)&automatic, &automatic_length) == 0 &&
            automatic_length == offsetof(struct sockaddr_un, sun_path) + 6 && !automatic.sun_path[0],
            "unix-passcred-autobind");
        int receiver = t ? listener : accept(listener, NULL, NULL);
        check(receiver >= 0, "unix-credentials-accept");
        int inherited = 0;
        socklen_t inherited_length = sizeof(inherited);
        check(getsockopt(receiver, SOL_SOCKET, SO_PASSCRED, &inherited, &inherited_length) == 0 && inherited,
            "unix-passcred-inherited");
        receive_credentials(receiver, getpid(), 128, 0, 0);
        check_peercred(client, t ? 0 : getpid());
        if (!t) close(receiver);
        close(listener); close(client);
    }
    puts("UNIXD_CREDENTIALS=OK");
}

static void datagram_io(void)
{
    int sockets[2];
    check(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sockets) == 0, "dgram-pair");
    char bytes[1024] = {0};
    check(recv(sockets[1], bytes, sizeof(bytes), MSG_DONTWAIT) == -1 && errno == EAGAIN, "dgram-empty");
    check(send(sockets[0], "", 0, 0) == 0, "dgram-zero-send");
    struct pollfd ready = { .fd = sockets[1], .events = POLLIN };
    check(poll(&ready, 1, 1000) == 1 && (ready.revents & POLLIN), "dgram-zero-poll");
    check(recv(sockets[1], bytes, sizeof(bytes), MSG_PEEK) == 0 &&
        recv(sockets[1], bytes, sizeof(bytes), 0) == 0 &&
        recv(sockets[1], bytes, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN, "dgram-zero-not-eof");
    struct iovec vectors[2] = {{"abc", 3}, {"def", 3}};
    struct msghdr message = { .msg_iov = vectors, .msg_iovlen = 2 };
    check(sendmsg(sockets[0], &message, 0) == 6, "dgram-sendmsg-iov");
    vectors[0] = (struct iovec){bytes, 2}; vectors[1] = (struct iovec){bytes + 2, 2};
    check(recvmsg(sockets[1], &message, MSG_PEEK | MSG_TRUNC) == 6 &&
        message.msg_flags == MSG_TRUNC && !memcmp(bytes, "abcd", 4), "dgram-peek-trunc");
    check(read(sockets[1], bytes, sizeof(bytes)) == 6 && !memcmp(bytes, "abcdef", 6), "dgram-read");
    int file = open("/tmp/unixd-dgram-rights", O_CREAT | O_TRUNC | O_RDWR, 0600), received = -1;
    check(file >= 0 && write(file, "fd", 2) == 2 && lseek(file, 0, SEEK_SET) == 0,
        "dgram-rights-file");
    check(send_rights(sockets[0], &file, 1) == 1 && close(file) == 0 &&
        receive_rights(sockets[1], &received, 1, MSG_CMSG_CLOEXEC, 0) == 1 &&
        read(received, bytes, sizeof(bytes)) == 2 && !memcmp(bytes, "fd", 2) && close(received) == 0,
        "dgram-rights-roundtrip");
    check(close(sockets[0]) == 0 && close(sockets[1]) == 0, "dgram-pair-close");
    puts("UNIXD_DGRAM_PAIR=OK");

    for (unsigned pathname = 0; pathname < 2; pathname++) {
        int server = socket(AF_UNIX, SOCK_DGRAM, 0);
        int a = socket(AF_UNIX, SOCK_DGRAM, 0), b = socket(AF_UNIX, SOCK_DGRAM, 0);
        struct sockaddr_un names[3] = {{.sun_family = AF_UNIX}, {.sun_family = AF_UNIX}, {.sun_family = AF_UNIX}};
        socklen_t lengths[3];
        int fds[3] = {server, a, b};
        for (unsigned i = 0; i < 3; i++) {
            if (pathname) {
                snprintf(names[i].sun_path, sizeof(names[i].sun_path), "/tmp/unixd-dgram-%u", i);
                unlink(names[i].sun_path);
                lengths[i] = offsetof(struct sockaddr_un, sun_path) + strlen(names[i].sun_path) + 1;
            } else {
                memcpy(names[i].sun_path + 1, "dgram\0name", 10);
                names[i].sun_path[11] = '0' + i;
                lengths[i] = offsetof(struct sockaddr_un, sun_path) + 12;
            }
            check(fds[i] >= 0 && bind(fds[i], (void *)&names[i], lengths[i]) == 0, "dgram-bind");
        }
        check(sendto(a, "first", 5, 0, (void *)&names[0], lengths[0]) == 5 &&
            sendto(b, "second", 6, 0, (void *)&names[0], lengths[0]) == 6, "dgram-two-sources");
        struct sockaddr_un source;
        socklen_t source_length = sizeof(source);
        check(getpeername(a, (void *)&source, &source_length) == -1 && errno == ENOTCONN,
            "dgram-sendto-does-not-connect");
        source_length = sizeof(source);
        check(recvfrom(server, bytes, sizeof(bytes), 0, (void *)&source, &source_length) == 5 &&
            !memcmp(bytes, "first", 5) && source_length == lengths[1] &&
            !memcmp(&source, &names[1], lengths[1]), "dgram-source-first");
        source_length = 5;
        check(recvfrom(server, bytes, sizeof(bytes), 0, (void *)&source, &source_length) == 6 &&
            !memcmp(bytes, "second", 6) && source_length == lengths[2] &&
            !memcmp(&source, &names[2], 5), "dgram-source-truncated");
        struct dgram_delayed delayed = { .fd = a, .destination = names[0], .length = lengths[0] };
        pthread_t thread;
        check(pthread_create(&thread, NULL, dgram_delayed_io, &delayed) == 0, "dgram-poll-thread");
        ready = (struct pollfd){ .fd = server, .events = POLLIN };
        check(poll(&ready, 1, 1000) == 1 && ready.revents == POLLIN &&
            read(server, bytes, sizeof(bytes)) == 4 && pthread_join(thread, NULL) == 0, "dgram-poll-wake");
        check(pthread_create(&thread, NULL, dgram_delayed_io, &delayed) == 0 &&
            read(server, bytes, sizeof(bytes)) == 4 && pthread_join(thread, NULL) == 0,
            "dgram-blocking-read");
        check(connect(a, (void *)&names[0], lengths[0]) == 0 &&
            connect(b, (void *)&names[0], lengths[0]) == 0, "dgram-connect");
        unsigned queued = 0;
        while (send(a, bytes, sizeof(bytes), MSG_DONTWAIT) == sizeof(bytes)) queued++;
        check(queued && queued < 100 && errno == EAGAIN, "dgram-full-budget");
        delayed = (struct dgram_delayed){ .fd = server, .receive = 1 };
        check(pthread_create(&thread, NULL, dgram_delayed_io, &delayed) == 0 &&
            send(b, bytes, sizeof(bytes), 0) == sizeof(bytes) && pthread_join(thread, NULL) == 0,
            "dgram-other-source-budget-wake");
        for (unsigned i = 0; i < queued; i++)
            check(recv(server, bytes, sizeof(bytes), MSG_DONTWAIT) == sizeof(bytes), "dgram-drain-budget");
        check(send(a, "alive", 5, 0) == 5 && close(a) == 0, "dgram-close-sender");
        source_length = sizeof(source);
        check(recvfrom(server, bytes, sizeof(bytes), 0, (void *)&source, &source_length) == 5 &&
            !memcmp(bytes, "alive", 5) && source_length == lengths[1] &&
            !memcmp(&source, &names[1], lengths[1]), "dgram-source-survives-close");
        struct timeval timeout = { .tv_usec = 20000 };
        check(setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
            recv(server, bytes, sizeof(bytes), 0) == -1 && errno == EAGAIN, "dgram-receive-timeout");
        check(close(b) == 0 && close(server) == 0, "dgram-named-close");
        if (pathname) for (unsigned i = 0; i < 3; i++) check(unlink(names[i].sun_path) == 0, "dgram-unlink");
    }
    puts("UNIXD_DGRAM_NAMED=OK");
}

static void killed_process_rights(void)
{
    const int types[] = { SOCK_STREAM, SOCK_SEQPACKET };
    for (unsigned t = 0; t < 2; ++t) for (unsigned iteration = 0; iteration < 8; ++iteration) {
        int channel[2], transferred[2], ready[2];
        check(socketpair(AF_UNIX, types[t], 0, channel) == 0 &&
            socketpair(AF_UNIX, SOCK_STREAM, 0, transferred) == 0 && pipe(ready) == 0,
            "kill-rights-setup");
        pid_t child = fork();
        check(child >= 0, "kill-rights-fork");
        if (!child) {
            close(channel[0]); close(transferred[0]); close(ready[0]);
            check(send_rights(channel[1], &transferred[1], 1) == 1,
                "kill-rights-child-send");
            check(write(ready[1], "R", 1) == 1, "kill-rights-child-ready");
            char byte;
            ssize_t unexpected = read(channel[1], &byte, 1);
            _exit(unexpected >= 0 ? 2 : 3);
        }
        close(channel[1]); close(transferred[1]); close(ready[1]);
        char byte;
        check(read(ready[0], &byte, 1) == 1 && byte == 'R', "kill-rights-ready");
        check(kill(child, SIGKILL) == 0, "kill-rights-signal");
        int status;
        check(waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
            WTERMSIG(status) == SIGKILL, "kill-rights-reaped");
        if (iteration & 1u) {
            pid_t receiver = fork();
            check(receiver >= 0, "kill-receiver-fork");
            if (!receiver) {
                close(transferred[0]); close(ready[0]);
                int received;
                check(receive_rights(channel[0], &received, 1, 0, 0) == 1 &&
                    send(received, "K", 1, MSG_NOSIGNAL) == 1, "kill-receiver-imported");
                /* Keep the imported OFD until SIGKILL. The parent owns no
                 * copy of it, so peer EOF proves dead-process reclamation. */
                for (;;) pause();
            }
            check(read(transferred[0], &byte, 1) == 1 && byte == 'K', "kill-receiver-ready");
            check(kill(receiver, SIGKILL) == 0 && waitpid(receiver, &status, 0) == receiver &&
                WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "kill-receiver-reaped");
        } else {
            int received;
            check(receive_rights(channel[0], &received, 1, 0, 0) == 1,
                "kill-rights-queued-reference-survives");
            check(send(received, "K", 1, MSG_NOSIGNAL) == 1 &&
                read(transferred[0], &byte, 1) == 1 && byte == 'K', "kill-rights-live-reference");
            check(close(received) == 0, "kill-rights-release");
        }
        struct pollfd pollfd = { .fd = transferred[0], .events = POLLIN };
        check(poll(&pollfd, 1, 1000) == 1 && (pollfd.revents & POLLHUP) &&
            read(transferred[0], &byte, 1) == 0, "kill-rights-last-owner-eof");
        pollfd = (struct pollfd){ .fd = channel[0], .events = POLLIN };
        check(poll(&pollfd, 1, 1000) == 1 && (pollfd.revents & POLLHUP) &&
            read(channel[0], &byte, 1) == 0, "kill-rights-dead-peer-eof");
        close(channel[0]); close(transferred[0]); close(ready[0]);
    }
    puts("UNIXD_KILLED_PROCESS_RIGHTS=OK cycles=16");
}

struct cache_fork_reader { int socket, ready; };
static void *cache_fork_receive(void *raw)
{
    struct cache_fork_reader *reader = raw;
    char byte;
    check(recv(reader->socket, &byte, 1, 0) == 1 && byte == 'W', "cache-fork-warm-reader");
    check(write(reader->ready, "R", 1) == 1, "cache-fork-reader-ready");
    check(recv(reader->socket, &byte, 1, 0) == 1 && byte == 'F', "cache-fork-live-parent-mapping");
    return NULL;
}

static void datagram_cache_fork(void)
{
    /* More routes than the retained cache, then fork while another thread
     * is blocked in a warm DGRAM receive. Its mapping must survive in the
     * parent but be removed from the child before any child socket RPC. */
    int sockets[12][2];
    for (unsigned i = 0; i < 12; i++)
        check(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets[i]) == 0, "cache-churn-pair");
    for (unsigned round = 0; round < 3; round++) for (unsigned i = 0; i < 12; i++) {
        unsigned char value = (unsigned char)(round * 12 + i), got = 255;
        check(send(sockets[i][0], &value, 1, 0) == 1 &&
            recv(sockets[i][1], &got, 1, 0) == 1 && got == value, "cache-churn-data");
    }
    for (unsigned i = 0; i < 12; i++) { close(sockets[i][0]); close(sockets[i][1]); }
    for (unsigned i = 0; i < 16; i++) {
        int channel[2], ready_pipe[2];
        check(socketpair(AF_UNIX, SOCK_DGRAM, 0, channel) == 0 && pipe(ready_pipe) == 0, "cache-fork-pair");
        check(send(channel[0], "W", 1, 0) == 1, "cache-fork-warm-send");
        struct cache_fork_reader reader = { channel[1], ready_pipe[1] };
        pthread_t thread;
        check(pthread_create(&thread, NULL, cache_fork_receive, &reader) == 0, "cache-fork-thread");
        char byte;
        check(read(ready_pipe[0], &byte, 1) == 1, "cache-fork-ready");
        usleep(20000);
        pid_t child = fork();
        check(child >= 0, "cache-fork");
        if (!child) {
            int fresh[2];
            check(socketpair(AF_UNIX, SOCK_DGRAM, 0, fresh) == 0 &&
                send(fresh[0], "C", 1, 0) == 1 && recv(fresh[1], &byte, 1, 0) == 1 &&
                byte == 'C', "cache-fork-child-fresh-cache");
            close(fresh[0]); close(fresh[1]);
            check(send(channel[0], "F", 1, 0) == 1, "cache-fork-child-wake-parent");
            _exit(0);
        }
        int status;
        check(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "cache-fork-child-exit");
        check(pthread_join(thread, NULL) == 0, "cache-fork-thread-exit");
        close(channel[0]); close(channel[1]); close(ready_pipe[0]); close(ready_pipe[1]);
    }
    puts("UNIXD_DGRAM_CACHE_FORK=OK cycles=16 routes=12");
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    if (argc == 3 && !strcmp(argv[1], "--cache-exec-loop")) {
        unsigned remaining = (unsigned)strtoul(argv[2], NULL, 10);
        check(remaining <= 40, "cache-exec-count");
        if (!remaining) { puts("UNIXD_CACHE_EXEC=OK cycles=40"); return 0; }
        /* Keep warm cached mapping FDs alive across exec preparation. Their
         * Linux sockets are CLOEXEC, and the invisible native cache caps
         * must be CLOEXEC too; PRIVATE only excludes them from fork. */
        for (unsigned i = 0; i < 2; i++) {
            int fds[2]; char byte;
            check(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, fds) == 0 &&
                send(fds[0], "E", 1, 0) == 1 && recv(fds[1], &byte, 1, 0) == 1 &&
                byte == 'E', "cache-exec-warm");
        }
        char next[16];
        snprintf(next, sizeof(next), "%u", remaining - 1);
        execl(argv[0], argv[0], "--cache-exec-loop", next, (char *)NULL);
        check(0, "cache-exec-image");
    }
    if (argc > 1 && !strcmp(argv[1], "--unix-inherit")) return inherited_image(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "--credentials-only")) { unix_credentials(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--kill-only")) { killed_process_rights(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--cache-fork-only")) { datagram_cache_fork(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--epoll-cross-process-only")) {
        cross_process_epoll_edges(); cross_process_epoll_write_edges(); return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--datagram-only")) { datagram_io(); datagram_boundaries(); datagram_disconnect(); puts("UNIXD_DGRAM_DONE"); return 0; }
    puts("UNIXD_PAIR_START");
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "stream-pair");
    puts("UNIXD_PAIR_CREATED");
    pthread_t thread;
    check(pthread_create(&thread, NULL, sender, NULL) == 0, "pthread-create");
    char buffer[16] = {0};
    check(read(pair[0], buffer, sizeof(buffer)) == 4 && !memcmp(buffer, "wake", 4), "blocking-read");
    check(pthread_join(thread, NULL) == 0, "pthread-join");
    puts("UNIXD_PAIR_BLOCKING=OK");
    int duplicate = dup(pair[1]);
    check(duplicate >= 0 && close(pair[1]) == 0, "dup-close");
    check(send(duplicate, "end", 3, MSG_NOSIGNAL) == 3, "send");
    check(recv(pair[0], buffer, sizeof(buffer), MSG_PEEK) == 3, "peek");
    check(read(pair[0], buffer, sizeof(buffer)) == 3 && !memcmp(buffer, "end", 3), "read-after-peek");
    check(shutdown(duplicate, SHUT_WR) == 0 && read(pair[0], buffer, sizeof(buffer)) == 0, "eof");
    check(close(duplicate) == 0 && close(pair[0]) == 0, "stream-close");
    puts("UNIXD_PAIR_STREAM=OK");
    for (unsigned i = 0; i < 100; i++) {
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair) == 0, "packet-pair");
        check(recv(pair[0], buffer, 1, 0) == -1 && errno == EAGAIN, "empty-nonblock");
        check(send(pair[1], "", 0, MSG_NOSIGNAL) == 0, "empty-packet-send");
        check(recv(pair[0], buffer, sizeof(buffer), 0) == 0, "empty-packet-recv");
        check(recv(pair[0], buffer, 1, 0) == -1 && errno == EAGAIN, "packet-not-eof");
        check(close(pair[0]) == 0 && close(pair[1]) == 0, "packet-close");
    }
    puts("UNIXD_PAIR_SEQPACKET=OK");
    named_connections();
    pathname_connections();
    message_io();
    rights_io();
    pty_rights_io();
    socket_rights_io();
    readiness_waits();
    cross_process_epoll_edges();
    cross_process_epoll_write_edges();
    wait_registration_lifetime();
    fork_exec_io();
    datagram_io();
    datagram_boundaries();
    datagram_disconnect();
    datagram_cache_fork();
    pid_t cache_exec_child = fork();
    check(cache_exec_child >= 0, "cache-exec-fork");
    if (!cache_exec_child) {
        execl(argv[0], argv[0], "--cache-exec-loop", "40", (char *)NULL);
        _exit(1);
    }
    int cache_exec_status;
    check(waitpid(cache_exec_child, &cache_exec_status, 0) == cache_exec_child &&
        WIFEXITED(cache_exec_status) && !WEXITSTATUS(cache_exec_status), "cache-exec-reaped");
    unix_credentials();
    killed_process_rights();
    puts("UNIXD_PAIR_DONE");
    return 0;
}

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int transfer(int socket, int fd)
{
    char byte = 'x', control[CMSG_SPACE(sizeof(fd))] = {0};
    struct iovec io = { &byte, 1 };
    struct msghdr msg = { .msg_iov = &io, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    struct cmsghdr *header = CMSG_FIRSTHDR(&msg);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    return sendmsg(socket, &msg, MSG_NOSIGNAL) == 1 ? 0 : -1;
}

static int receive(int socket)
{
    char byte, control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec io = { &byte, 1 };
    struct msghdr msg = { .msg_iov = &io, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    if (recvmsg(socket, &msg, MSG_CMSG_CLOEXEC) != 1 || (msg.msg_flags & MSG_CTRUNC)) return -1;
    struct cmsghdr *header = CMSG_FIRSTHDR(&msg);
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len != CMSG_LEN(sizeof(int))) return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(header), sizeof(fd));
    return fd;
}

static int child_ok(pid_t child)
{
    int status;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status);
}

static int check(const char *path, const char *self)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) return 1;
    int original = open(path, O_RDWR | O_CLOEXEC);
    if (original < 0) return 2;
    pid_t child = fork();
    if (child < 0) return 2;
    if (!child) _exit(fcntl(original, F_SETFL, O_NONBLOCK) ? 1 : 0);
    if (!child_ok(child) || !(fcntl(original, F_GETFL) & O_NONBLOCK) ||
        transfer(sockets[0], original)) return 2;
    int received = receive(sockets[1]);
    struct stat st;
    if (received < 0 || fstat(received, &st) || !S_ISCHR(st.st_mode) ||
        fcntl(received, F_GETFD) != FD_CLOEXEC) return 3;
    /* SCM_RIGHTS retains the same OFD, not an independently reopened path. */
    if (fcntl(received, F_SETFL, O_NONBLOCK | O_APPEND) ||
        (fcntl(original, F_GETFL) & (O_NONBLOCK | O_APPEND)) != (O_NONBLOCK | O_APPEND)) return 4;
    child = fork();
    if (child < 0) return 5;
    if (!child) _exit(fcntl(received, F_SETFL, O_NONBLOCK) ? 1 : 0);
    if (!child_ok(child) ||
        (fcntl(original, F_GETFL) & O_APPEND)) return 6;
    if (fcntl(received, F_SETFD, 0)) return 6;
    child = fork();
    if (child < 0) return 6;
    if (!child) {
        char number[32];
        snprintf(number, sizeof(number), "%d", received);
        execlp(self, self, "--exec-check", number, (char *)NULL);
        _exit(127);
    }
    if (!child_ok(child) || !(fcntl(original, F_GETFL) & O_APPEND)) return 6;
    if (transfer(sockets[1], received)) return 7;
    close(original);
    close(received);
    int forwarded = receive(sockets[0]);
    char data[8];
    memset(data, 0x55, sizeof(data));
    ssize_t expected = !strcmp(path, "/dev/null") ? 0 : (ssize_t)sizeof(data);
    if (forwarded < 0 || read(forwarded, data, sizeof(data)) != expected ||
        write(forwarded, data, sizeof(data)) != sizeof(data)) return 8;
    if (expected) for (unsigned i = 0; i < sizeof(data); i++) if (data[i]) return 9;
    close(forwarded);
    close(sockets[0]);
    close(sockets[1]);
    printf("DEVICE_RIGHTS path=%s PASS shared-flags fork exec forwarding sender-close cloexec read-write\n", path);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--exec-check")) {
        int fd = atoi(argv[2]);
        struct stat st;
        return fstat(fd, &st) || !S_ISCHR(st.st_mode) ||
            fcntl(fd, F_SETFL, O_NONBLOCK | O_APPEND) ? 1 : 0;
    }
    if (argc != 1) return 2;
    const char *paths[] = { "/dev/null", "/dev/zero" };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int status = check(paths[i], argv[0]);
        if (status) { fprintf(stderr, "DEVICE_RIGHTS path=%s step=%d errno=%d\n", paths[i], status, errno); return status; }
    }
    return 0;
}

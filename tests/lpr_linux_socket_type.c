#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;
static void check(const char *name, long result, int expected)
{
    if (result != -1 || errno != expected) {
        fprintf(stderr, "SOCKET_TYPE %s result=%ld errno=%d expected=%d\n",
            name, result, errno, expected);
        failures++;
    }
}
static void nonsocket(int fd, int expected)
{
    char byte = 0;
    struct iovec iov = { &byte, 1 };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    check("send", send(fd, &byte, 1, MSG_NOSIGNAL), expected);
    check("recv", recv(fd, &byte, 1, MSG_DONTWAIT), expected);
    check("sendmsg", sendmsg(fd, &msg, MSG_NOSIGNAL), expected);
    check("recvmsg", recvmsg(fd, &msg, MSG_DONTWAIT), expected);
}
int main(void)
{
    int pipefd[2], pair[2];
    setvbuf(stdout, NULL, _IONBF, 0);
    if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK)) return 2;
    nonsocket(pipefd[0], ENOTSOCK);
    nonsocket(pipefd[1], ENOTSOCK);
    int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 2;
    nonsocket(fd, ENOTSOCK);
    close(fd);
    nonsocket(fd, EBADF);
    nonsocket(-1, EBADF);
    fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0) return 2;
    nonsocket(fd, ENOTSOCK);
    close(fd);
    /* PulseAudio-style fallback must wake the original pipe, not consume
     * its data or turn a valid non-socket descriptor into EBADF. */
    char byte = 'W', readback = 0;
    ssize_t n = send(pipefd[1], &byte, 1, MSG_NOSIGNAL);
    if (n < 0 && errno == ENOTSOCK) n = write(pipefd[1], &byte, 1);
    if (n != 1 || read(pipefd[0], &readback, 1) != 1 || readback != byte)
        failures++;
    close(pipefd[0]); close(pipefd[1]);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 2;
    if (send(pair[0], &byte, 1, MSG_NOSIGNAL) != 1 ||
        recv(pair[1], &readback, 1, 0) != 1 || readback != byte) failures++;
    close(pair[0]); close(pair[1]);
    printf("SOCKET_TYPE %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}

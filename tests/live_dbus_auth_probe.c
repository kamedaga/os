/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>

static uint64_t now_us(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static int probe(const char *path, int credentials, int external, int blocking) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(addr.sun_path)) { close(fd); return 1; }
    strcpy(addr.sun_path, path);
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr))) {
        perror("connect"); close(fd); return 1;
    }
    struct timeval timeout = {.tv_sec = 3};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))) {
        perror("receive timeout"); close(fd); return 1;
    }
    char zero = 0;
    struct iovec vector = {.iov_base = &zero, .iov_len = 1};
    union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(struct ucred))]; } control = {0};
    struct msghdr msg = {.msg_iov = &vector, .msg_iovlen = 1};
    if (credentials) {
        msg.msg_control = control.bytes; msg.msg_controllen = sizeof(control.bytes);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_CREDENTIALS;
        c->cmsg_len = CMSG_LEN(sizeof(struct ucred));
        struct ucred identity = {.pid = getpid(), .uid = getuid(), .gid = getgid()};
        memcpy(CMSG_DATA(c), &identity, sizeof(identity));
    }
    uint64_t start = now_us();
    if (sendmsg(fd, &msg, MSG_NOSIGNAL) != 1) {
        perror("sendmsg"); close(fd); return 1;
    }
    /* Match the two standard client handshake shapes without linking a
     * second GLib build or changing the running desktop's bus server. */
    const char *auth = external ? "AUTH EXTERNAL 30\r\n" : "AUTH\r\n";
    if (send(fd, auth, strlen(auth), MSG_NOSIGNAL) != (ssize_t)strlen(auth)) {
        perror("send"); close(fd); return 1;
    }
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int ready = blocking ? 0 : poll(&p, 1, 3000);
    char answer[256] = {0};
    ssize_t count = blocking ? read(fd, answer, sizeof(answer) - 1) :
        (ready > 0 ? recv(fd, answer, sizeof(answer) - 1, MSG_DONTWAIT) : -1);
    int error = count < 0 ? errno : 0;
    for (ssize_t i = 0; i < count; ++i)
        if (answer[i] == '\r' || answer[i] == '\n') answer[i] = ' ';
    printf("DBUS_AUTH credentials=%d external=%d blocking=%d ready=%d revents=0x%x count=%zd errno=%d elapsed_us=%llu answer=%s\n",
        credentials, external, blocking, ready, p.revents, count, error,
        (unsigned long long)(now_us() - start), answer);
    close(fd);
    return count > 0 ? 0 : 1;
}
int main(int argc, char **argv) {
    if (argc != 2 && !(argc == 3 && !strcmp(argv[2], "--blocking"))) {
        fprintf(stderr, "usage: %s UNIX_BUS_PATH [--blocking]\n", argv[0]); return 2;
    }
    int status = 0;
    for (int credentials = 0; credentials <= 1; ++credentials)
        for (int external = 0; external <= 1; ++external)
            status |= probe(argv[1], credentials, external, argc == 3);
    return status;
}

/* Run against a reachable DNS server: socket-peek [server IPv4].
 * A short MSG_PEEK and FIONREAD must both preserve the complete datagram. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const unsigned char query[] = {
        0x51, 0x73, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
        7, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        3, 'c', 'o', 'm', 0, 0, 1, 0, 1,
    };
    struct sockaddr_in server = {.sin_family = AF_INET, .sin_port = htons(53)};
    assert(inet_pton(AF_INET, argc > 1 ? argv[1] : "10.0.2.3",
                     &server.sin_addr) == 1);
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    assert(fd >= 0);
    assert(connect(fd, (struct sockaddr *)&server, sizeof(server)) == 0);
    assert(send(fd, query, sizeof(query), 0) == (ssize_t)sizeof(query));
    struct pollfd p = {.fd = fd, .events = POLLIN};
    assert(poll(&p, 1, 5000) == 1 && (p.revents & POLLIN));
    unsigned char first[12], second[12], answer[4096];
    assert(recv(fd, first, sizeof(first), MSG_PEEK | MSG_DONTWAIT) == 12);
    assert(recv(fd, second, sizeof(second), MSG_PEEK | MSG_DONTWAIT) == 12);
    assert(memcmp(first, second, sizeof(first)) == 0);
    assert(first[0] == query[0] && first[1] == query[1] && (first[2] & 0x80));
    int available = 0, again = 0;
    assert(ioctl(fd, FIONREAD, &available) == 0 && available >= 12);
    assert(ioctl(fd, FIONREAD, &again) == 0 && again == available);
    assert(poll(&p, 1, 0) == 1 && (p.revents & POLLIN));
    ssize_t n = recv(fd, answer, sizeof(answer), MSG_DONTWAIT);
    assert(n == available && memcmp(answer, first, sizeof(first)) == 0);
    assert(recv(fd, answer, sizeof(answer), MSG_DONTWAIT) == -1 &&
           (errno == EAGAIN || errno == EWOULDBLOCK));
    close(fd);
    puts("SOCKET_PEEK=PASS repeated-short-peek FIONREAD readiness datagram-preserved");
    return 0;
}

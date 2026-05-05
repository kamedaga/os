#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned parse_ipv4_text(const char *text) {
    unsigned parts[4] = {0, 0, 0, 0};
    unsigned part = 0;
    const char *p = text;
    while (*p && part < 4) {
        if (*p < '0' || *p > '9') return 0;
        unsigned value = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (unsigned)(*p - '0');
            if (value > 255) return 0;
            p++;
        }
        parts[part++] = value;
        if (part == 4) break;
        if (*p != '.') return 0;
        p++;
    }
    if (part != 4 || *p != 0) return 0;
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

int main(int argc, char **argv) {
    const char *ip_text = argc >= 2 ? argv[1] : "1.1.1.1";
    const char *host = argc >= 3 ? argv[2] : "one.one.one.one";
    unsigned ip = parse_ipv4_text(ip_text);
    if (!ip) {
        fprintf(stderr, "tcp_http_get: invalid ip\n");
        return 2;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = htonl(ip);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    char request[256];
    int request_len = snprintf(request, sizeof(request),
        "GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    if (request_len <= 0 || request_len >= (int)sizeof(request)) {
        close(fd);
        return 2;
    }
    if (write(fd, request, (size_t)request_len) != request_len) {
        perror("write");
        close(fd);
        return 1;
    }

    char buf[513];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) {
        perror("read");
        return 1;
    }
    if (n == 0) {
        fprintf(stderr, "tcp_http_get: eof\n");
        return 1;
    }
    buf[n] = 0;
    char *end = strchr(buf, '\n');
    if (end) *end = 0;
    puts(buf);
    return 0;
}

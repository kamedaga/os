#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { BENCH_BUF_BYTES = 64 * 1024 };

static long long now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

static unsigned long long parse_u64(const char *text) {
    unsigned long long value = 0;
    if (text == 0 || *text == 0) return 0;
    while (*text >= '0' && *text <= '9') {
        value = value * 10ULL + (unsigned long long)(*text - '0');
        text++;
    }
    return *text == 0 ? value : 0;
}

static int connect_tcp(const char *host, const char *port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = 0;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0 || res == 0) {
        fprintf(stderr, "net_bench: getaddrinfo failed host=%s\n", host);
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *it = res; it != 0; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int write_all(int fd, const void *data, size_t len) {
    const char *p = (const char *)data;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int bench_write_only(const char *path, unsigned long long bytes, unsigned long long chunk) {
    if (chunk == 0 || chunk > BENCH_BUF_BYTES) chunk = BENCH_BUF_BYTES;
    char *buf = malloc((size_t)chunk);
    if (buf == 0) return 1;
    for (unsigned long long i = 0; i < chunk; i++) buf[i] = (char)('A' + (i % 23));

    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        perror("open");
        free(buf);
        return 1;
    }

    long long start = now_ms();
    unsigned long long done = 0;
    while (done < bytes) {
        unsigned long long n = bytes - done;
        if (n > chunk) n = chunk;
        if (write_all(fd, buf, (size_t)n) != 0) {
            perror("write");
            close(fd);
            free(buf);
            return 1;
        }
        done += n;
    }
    if (fsync(fd) != 0) perror("fsync");
    close(fd);
    long long elapsed = now_ms() - start;
    printf("net_bench mode=write-only bytes=%llu chunk=%llu ms=%lld\n", done, chunk, elapsed);
    free(buf);
    return 0;
}

static int bench_fetch(const char *mode, const char *host, const char *path, const char *out_path) {
    int fd = connect_tcp(host, "80");
    if (fd < 0) {
        perror("connect");
        return 1;
    }

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    if (req_len <= 0 || req_len >= (int)sizeof(req)) {
        close(fd);
        return 2;
    }

    int out_fd = -1;
    if (out_path != 0) {
        out_fd = open(out_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (out_fd < 0) {
            perror("open");
            close(fd);
            return 1;
        }
    }

    char *buf = malloc(BENCH_BUF_BYTES);
    if (buf == 0) {
        if (out_fd >= 0) close(out_fd);
        close(fd);
        return 1;
    }

    long long start = now_ms();
    if (write_all(fd, req, (size_t)req_len) != 0) {
        perror("write request");
        free(buf);
        if (out_fd >= 0) close(out_fd);
        close(fd);
        return 1;
    }

    int header_done = 0;
    char header_tail[4] = {0, 0, 0, 0};
    unsigned long long body = 0;
    for (;;) {
        ssize_t n = read(fd, buf, BENCH_BUF_BYTES);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("read");
            free(buf);
            if (out_fd >= 0) close(out_fd);
            close(fd);
            return 1;
        }
        if (n == 0) break;

        size_t off = 0;
        if (!header_done) {
            for (off = 0; off < (size_t)n; off++) {
                header_tail[0] = header_tail[1];
                header_tail[1] = header_tail[2];
                header_tail[2] = header_tail[3];
                header_tail[3] = buf[off];
                if (header_tail[0] == '\r' && header_tail[1] == '\n' &&
                    header_tail[2] == '\r' && header_tail[3] == '\n') {
                    off++;
                    header_done = 1;
                    break;
                }
            }
            if (!header_done) continue;
        }

        size_t body_len = (size_t)n - off;
        if (body_len != 0) {
            if (out_fd >= 0 && write_all(out_fd, buf + off, body_len) != 0) {
                perror("write output");
                free(buf);
                close(out_fd);
                close(fd);
                return 1;
            }
            body += (unsigned long long)body_len;
        }
    }
    if (out_fd >= 0) {
        if (fsync(out_fd) != 0) perror("fsync");
        close(out_fd);
    }
    close(fd);
    long long elapsed = now_ms() - start;
    printf("net_bench mode=%s host=%s path=%s bytes=%llu ms=%lld\n", mode, host, path, body, elapsed);
    free(buf);
    return header_done ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "write-only") == 0) {
        const char *path = argc >= 3 ? argv[2] : "/tmp/net-bench.bin";
        unsigned long long bytes = argc >= 4 ? parse_u64(argv[3]) : 2595434ULL;
        unsigned long long chunk = argc >= 5 ? parse_u64(argv[4]) : BENCH_BUF_BYTES;
        return bench_write_only(path, bytes, chunk);
    }
    if (argc >= 5 && strcmp(argv[1], "fetch-write") == 0) {
        return bench_fetch("fetch-write", argv[2], argv[3], argv[4]);
    }
    if (argc >= 4 && strcmp(argv[1], "fetch-discard") == 0) {
        return bench_fetch("fetch-discard", argv[2], argv[3], 0);
    }
    fprintf(stderr,
        "usage:\n"
        "  net_bench write-only <path> <bytes> [chunk]\n"
        "  net_bench fetch-discard <host> <path>\n"
        "  net_bench fetch-write <host> <path> <out-path>\n");
    return 2;
}

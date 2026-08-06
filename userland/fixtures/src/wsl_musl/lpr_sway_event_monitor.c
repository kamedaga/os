#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum {
    I3_IPC_SUBSCRIBE = 2,
    IPC_PAYLOAD_BYTES = 64 * 1024,
};

struct ipc_header {
    char magic[6];
    uint32_t length;
    uint32_t type;
} __attribute__((packed));

static int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *cursor = data;
    while (length != 0) {
        const ssize_t n = write(fd, cursor, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        cursor += (size_t)n;
        length -= (size_t)n;
    }
    return 1;
}

static int read_all(int fd, void *data, size_t length)
{
    unsigned char *cursor = data;
    while (length != 0) {
        const ssize_t n = read(fd, cursor, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        cursor += (size_t)n;
        length -= (size_t)n;
    }
    return 1;
}

static int connect_when_ready(const char *path)
{
    const struct timespec retry = { .tv_sec = 0, .tv_nsec = 100000000 };
    for (;;) {
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        if (strlen(path) >= sizeof(address.sun_path)) {
            close(fd);
            return -1;
        }
        strcpy(address.sun_path, path);
        if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0) {
            return fd;
        }
        const int saved_errno = errno;
        close(fd);
        if (saved_errno != ENOENT && saved_errno != ECONNREFUSED &&
            saved_errno != EAGAIN && saved_errno != EINTR)
        {
            return -1;
        }
        (void)nanosleep(&retry, 0);
    }
}

int main(int argc, char **argv)
{
    static const char subscription[] = "[\"window\",\"tick\"]";
    if (argc != 2) return 2;
    const int fd = connect_when_ready(argv[1]);
    if (fd < 0) return 1;
    const struct ipc_header request = {
        .magic = { 'i', '3', '-', 'i', 'p', 'c' },
        .length = sizeof(subscription) - 1,
        .type = I3_IPC_SUBSCRIBE,
    };
    if (!write_all(fd, &request, sizeof(request)) ||
        !write_all(fd, subscription, sizeof(subscription) - 1))
    {
        close(fd);
        return 1;
    }

    unsigned char payload[IPC_PAYLOAD_BYTES];
    for (;;) {
        struct ipc_header response;
        if (!read_all(fd, &response, sizeof(response))) break;
        if (memcmp(response.magic, "i3-ipc", 6) != 0 ||
            response.length >= sizeof(payload))
        {
            close(fd);
            return 1;
        }
        if (!read_all(fd, payload, response.length)) break;
        payload[response.length] = '\n';
        if (!write_all(STDOUT_FILENO, payload, response.length + 1)) break;
    }
    close(fd);
    return 0;
}

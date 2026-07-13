#define _GNU_SOURCE

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int parse_iterations(const char *text)
{
    char *end = NULL;
    errno = 0;
    const long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value <= 0 || value > 100000) {
        return -1;
    }
    return (int)value;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s iterations socket-path\n", argv[0]);
        return 2;
    }
    const int iterations = parse_iterations(argv[1]);
    const char *path = argv[2];
    if (iterations < 0 || path[0] != '/' || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "EXT4_UNLINK_JOURNAL_SOCKET_ARGUMENTS=FAIL\n");
        return 2;
    }

    (void)unlink(path);
    printf("EXT4_UNLINK_JOURNAL_SOCKET_START iterations=%d\n", iterations);
    fflush(stdout);
    for (int iteration = 1; iteration <= iterations; iteration++) {
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            printf("EXT4_UNLINK_JOURNAL_SOCKET_CREATE=FAIL iteration=%d errno=%d\n", iteration, errno);
            return 1;
        }

        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, path, strlen(path) + 1u);
        const socklen_t address_length =
            (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1u);
        if (bind(fd, (const struct sockaddr *)&address, address_length) != 0) {
            printf("EXT4_UNLINK_JOURNAL_SOCKET_BIND=FAIL iteration=%d errno=%d\n", iteration, errno);
            (void)close(fd);
            return 1;
        }

        sync();
        struct stat status;
        const int lookup_result = lstat(path, &status);
        if (lookup_result != 0 || !S_ISSOCK(status.st_mode)) {
            printf("EXT4_UNLINK_JOURNAL_SOCKET_LOOKUP=FAIL iteration=%d errno=%d mode=%o\n",
                iteration,
                errno,
                lookup_result == 0 ? (unsigned int)status.st_mode : 0u);
            (void)close(fd);
            return 1;
        }
        if (close(fd) != 0) {
            printf("EXT4_UNLINK_JOURNAL_SOCKET_CLOSE=FAIL iteration=%d errno=%d\n", iteration, errno);
            return 1;
        }
        if (unlink(path) != 0) {
            printf("EXT4_UNLINK_JOURNAL_SOCKET_UNLINK=FAIL iteration=%d errno=%d\n", iteration, errno);
            return 1;
        }
        sync();
        errno = 0;
        if (lstat(path, &status) == 0 || errno != ENOENT) {
            printf("EXT4_UNLINK_JOURNAL_SOCKET_ENOENT=FAIL iteration=%d errno=%d\n", iteration, errno);
            return 1;
        }
        printf("EXT4_UNLINK_JOURNAL_SOCKET_ITERATION=%d status=OK\n", iteration);
        fflush(stdout);
    }

    printf("EXT4_UNLINK_JOURNAL_SOCKET_DONE iterations=%d failures=0\n", iterations);
    return 0;
}

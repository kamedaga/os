#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct resource_create {
    uint32_t target, format, bind, width, height, depth, array_size;
    uint32_t last_level, nr_samples, flags, bo_handle, res_handle, size, stride;
};
struct prime_handle { uint32_t handle, flags; int32_t fd; };
#define RESOURCE_CREATE _IOWR('d', 0x44, struct resource_create)
#define PRIME_EXPORT _IOWR('d', 0x2d, struct prime_handle)

static uint32_t width = 32, height = 32;
static size_t mapping_bytes = 4096;

static pid_t wait_child(pid_t pid, int *status)
{
    pid_t result;
    do { result = waitpid(pid, status, 0); } while (result < 0 && errno == EINTR);
    return result;
}

static ssize_t byte_io(int fd, char *byte, int writing)
{
    ssize_t result;
    do { result = writing ? write(fd, byte, 1) : read(fd, byte, 1); }
    while (result < 0 && errno == EINTR);
    return result;
}

static void child(int socket)
{
    int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    struct resource_create resource = {.target = 2, .format = 1, .bind = 2,
        .width = width, .height = height, .depth = 1, .array_size = 1,
        .size = mapping_bytes, .stride = width * 4};
    if (drm < 0 || ioctl(drm, RESOURCE_CREATE, &resource)) {
        perror("PRIME resource create"); _exit(10);
    }
    struct prime_handle prime = {.handle = resource.bo_handle,
        .flags = O_CLOEXEC | O_RDWR, .fd = -1};
    if (ioctl(drm, PRIME_EXPORT, &prime)) { perror("PRIME_EXPORT"); _exit(11); }
    /* Fork while the original export lease is live, before SCM_RIGHTS. */
    pid_t nested = fork();
    if (nested < 0) { perror("PRIME fork"); _exit(14); }
    if (!nested) {
        volatile uint32_t *check = mmap(NULL, mapping_bytes, PROT_READ | PROT_WRITE,
            MAP_SHARED, prime.fd, 0);
        if (check == MAP_FAILED) _exit(15);
        check[0] = 0x98765432;
        if (munmap((void *)check, mapping_bytes) || close(prime.fd)) _exit(16);
        _exit(0);
    }
    int nested_status = 0;
    if (wait_child(nested, &nested_status) != nested || nested_status) _exit(17);
    char byte = 'P';
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } ancillary = {0};
    struct iovec iov = {.iov_base = &byte, .iov_len = 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes)};
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &prime.fd, sizeof(int));
    ssize_t sent;
    do { sent = sendmsg(socket, &msg, 0); } while (sent < 0 && errno == EINTR);
    if (sent != 1 || byte_io(socket, &byte, 0) != 1) _exit(12);
    /* Intentionally bypass Linux close/exit cleanup: native fault retirement. */
    __asm__ volatile("int3");
    _exit(13);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "fhd")) {
        width = 1920; height = 1080; mapping_bytes = (size_t)width * height * 4;
    } else if (argc != 1) return 64;
    setbuf(stdout, NULL);
    printf("PRIME_DEATH bytes=%zu\n", mapping_bytes);
    for (unsigned round = 0; round < 96; ++round) {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 1;
        pid_t pid = fork();
        if (pid < 0) return 2;
        if (!pid) { close(pair[0]); child(pair[1]); }
        close(pair[1]);
        char byte;
        union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } ancillary = {0};
        struct iovec iov = {.iov_base = &byte, .iov_len = 1};
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes)};
        ssize_t received;
        do { received = recvmsg(pair[0], &msg, 0); }
        while (received < 0 && errno == EINTR);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        if (received != 1 || !cmsg || cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
            int saved_errno = errno, status = 0;
            /* Do not wait while the child is blocked on our handshake. */
            close(pair[0]);
            pid_t waited = wait_child(pid, &status);
            fprintf(stderr, "PRIME_DEATH receive round=%u child=%d waited=%d errno=%d\n",
                round, status, (int)waited, saved_errno);
            return 3;
        }
        int fd; memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
        volatile uint32_t *data = mmap(NULL, mapping_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) return 4;
        data[0] = 0xaabb0000u | round;
        data[mapping_bytes / sizeof(*data) - 1] = 0x76543210u;
        byte = 'x';
        if (byte_io(pair[0], &byte, 1) != 1) return 5;
        int status = 0;
        if (wait_child(pid, &status) != pid || status == 0) return 6;
        /* Original owner is dead, transferred FD and mapping must remain. */
        if (data[0] != (0xaabb0000u | round)) return 7;
        if (data[mapping_bytes / sizeof(*data) - 1] != 0x76543210u) return 7;
        if (close(fd)) return 8;
        data[1] = data[0] ^ 0x12345678u;
        if (munmap((void *)data, mapping_bytes) || close(pair[0])) return 9;
        if ((round + 1) % 16 == 0) printf("PRIME_DEATH completed=%u\n", round + 1);
    }
    puts("PRIME_DEATH PASS 96 owner faults, SCM_RIGHTS and surviving mappings");
    return 0;
}

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    PAGE_BYTES = 4096,
};

static const char persist_path[] = "/lpr-shared-mapping-persist.bin";
static const char persist_value[] = "pacha-map-shared-persist-v1";

static void emit(const char *message)
{
    (void)write(1, message, strlen(message));
}

static int wait_child(pid_t pid)
{
    int status = 0;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int wait_for_byte(volatile unsigned char *byte, unsigned char expected)
{
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 1000000 };
    for (unsigned int i = 0; i < 3000; ++i) {
        if (*byte == expected) {
            return 1;
        }
        (void)nanosleep(&delay, 0);
    }
    return 0;
}

static int file_write_phase(void)
{
    const size_t value_bytes = sizeof(persist_value);
    int fd = open(persist_path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0 || ftruncate(fd, PAGE_BYTES) != 0) {
        if (fd >= 0) close(fd);
        return 0;
    }
    unsigned char *mapped = mmap(
        0,
        PAGE_BYTES,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd,
        0);
    if (mapped == MAP_FAILED) {
        close(fd);
        return 0;
    }
    memcpy(mapped, persist_value, value_bytes);

    char readback[sizeof(persist_value)];
    memset(readback, 0, sizeof(readback));
    if (lseek(fd, 0, SEEK_SET) != 0 ||
        read(fd, readback, sizeof(readback)) != (ssize_t)sizeof(readback) ||
        memcmp(readback, persist_value, sizeof(readback)) != 0)
    {
        munmap(mapped, PAGE_BYTES);
        close(fd);
        return 0;
    }

    pid_t child = fork();
    if (child < 0) {
        munmap(mapped, PAGE_BYTES);
        close(fd);
        return 0;
    }
    if (child == 0) {
        char child_readback[sizeof(persist_value)];
        int child_fd = open(persist_path, O_RDONLY);
        memset(child_readback, 0, sizeof(child_readback));
        const int good = child_fd >= 0 &&
            read(child_fd, child_readback, sizeof(child_readback)) == (ssize_t)sizeof(child_readback) &&
            memcmp(child_readback, persist_value, sizeof(child_readback)) == 0;
        if (child_fd >= 0) close(child_fd);
        _exit(good ? 0 : 1);
    }
    if (!wait_child(child)) {
        munmap(mapped, PAGE_BYTES);
        close(fd);
        return 0;
    }
    emit("SHMAP_FILE_COHERENCE=OK\n");

    const int sync_ok = msync(mapped, PAGE_BYTES, MS_SYNC) == 0 && fsync(fd) == 0;
    munmap(mapped, PAGE_BYTES);
    close(fd);
    if (!sync_ok) {
        return 0;
    }
    emit("SHMAP_FILE_MSYNC=OK\n");
    emit("SHMAP_WRITE_DONE\n");
    return 1;
}

static int persistence_phase(void)
{
    char readback[sizeof(persist_value)];
    int fd = open(persist_path, O_RDONLY);
    memset(readback, 0, sizeof(readback));
    const int good = fd >= 0 &&
        read(fd, readback, sizeof(readback)) == (ssize_t)sizeof(readback) &&
        memcmp(readback, persist_value, sizeof(readback)) == 0;
    if (fd >= 0) close(fd);
    if (!good) {
        return 0;
    }
    emit("SHMAP_FILE_PERSISTENCE=OK\n");
    return 1;
}

static int memfd_two_map_phase(void)
{
    int fd = memfd_create("pacha-shared-map", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, PAGE_BYTES) != 0) {
        if (fd >= 0) close(fd);
        return 0;
    }
    volatile unsigned char *first = mmap(
        0,
        PAGE_BYTES,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd,
        0);
    volatile unsigned char *second = mmap(
        0,
        PAGE_BYTES,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd,
        0);
    if (first == MAP_FAILED || second == MAP_FAILED) {
        if (first != MAP_FAILED) munmap((void *)first, PAGE_BYTES);
        if (second != MAP_FAILED) munmap((void *)second, PAGE_BYTES);
        close(fd);
        return 0;
    }
    first[17] = 0x5a;
    const int good = second[17] == 0x5a;
    munmap((void *)first, PAGE_BYTES);
    munmap((void *)second, PAGE_BYTES);
    close(fd);
    if (!good) {
        return 0;
    }
    emit("SHMAP_MEMFD_TWO_MAP=OK\n");
    return 1;
}

static int fork_shared_phase(int anonymous)
{
    int fd = -1;
    int flags = MAP_SHARED;
    if (anonymous) {
        flags |= MAP_ANONYMOUS;
    } else {
        fd = memfd_create("pacha-fork-map", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, PAGE_BYTES) != 0) {
            if (fd >= 0) close(fd);
            return 0;
        }
    }
    volatile unsigned char *mapped = mmap(
        0,
        PAGE_BYTES,
        PROT_READ | PROT_WRITE,
        flags,
        fd,
        0);
    if (mapped == MAP_FAILED) {
        if (fd >= 0) close(fd);
        return 0;
    }
    mapped[0] = 0;
    mapped[1] = 0;
    pid_t child = fork();
    if (child < 0) {
        munmap((void *)mapped, PAGE_BYTES);
        if (fd >= 0) close(fd);
        return 0;
    }
    if (child == 0) {
        if (!wait_for_byte(&mapped[0], 'P')) {
            _exit(1);
        }
        mapped[1] = 'C';
        _exit(0);
    }
    mapped[0] = 'P';
    const int good = wait_child(child) && mapped[1] == 'C';
    munmap((void *)mapped, PAGE_BYTES);
    if (fd >= 0) close(fd);
    return good;
}

static int verify_phase(void)
{
    if (!persistence_phase()) {
        return 0;
    }
    if (!memfd_two_map_phase()) {
        return 0;
    }
    if (!fork_shared_phase(0)) {
        return 0;
    }
    emit("SHMAP_MEMFD_FORK_BIDIR=OK\n");
    if (!fork_shared_phase(1)) {
        return 0;
    }
    emit("SHMAP_ANON_FORK_BIDIR=OK\n");
    (void)unlink(persist_path);
    emit("SHMAP_VERIFY_DONE\n");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    if (strcmp(argv[1], "write") == 0) {
        return file_write_phase() ? 0 : 1;
    }
    if (strcmp(argv[1], "verify") == 0) {
        return verify_phase() ? 0 : 1;
    }
    return 2;
}

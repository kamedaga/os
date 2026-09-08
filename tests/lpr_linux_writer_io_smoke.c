#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "FAIL line %d: %s errno=%d\n", __LINE__, #test, errno); return 1; } } while (0)

int main(void)
{
    char path[] = "/tmp/writer-io-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(unlink(path) == 0);
    CHECK(write(fd, "abcdef", 6) == 6);
    CHECK(lseek(fd, 2, SEEK_SET) == 2);
    int duplicate = dup(fd);
    CHECK(duplicate >= 0);
    CHECK(pwrite(duplicate, "XY", 2, 3) == 2);
    CHECK(lseek(fd, 0, SEEK_CUR) == 2);
    char small[8] = {0};
    CHECK(pread(fd, small, 6, 0) == 6);
    CHECK(memcmp(small, "abcXYf", 6) == 0);
    CHECK(pwrite(fd, "x", 1, -1) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_pwrite64, -1, "x", 1, 0) == -1 && errno == EBADF);
    int readonly = open("/etc/hosts", O_RDONLY);
    CHECK(readonly >= 0);
    CHECK(pwrite(readonly, "x", 1, 0) == -1 && errno == EBADF);
    CHECK(close(readonly) == 0);
    int pipes[2];
    CHECK(pipe(pipes) == 0);
    CHECK(pwrite(pipes[1], "x", 1, 0) == -1 && errno == ESPIPE);
    CHECK(close(pipes[0]) == 0 && close(pipes[1]) == 0);
    CHECK(pwrite(fd, NULL, 0, 0) == 0);
    unsigned char data[20000], copy[20000];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)(i * 37);
    CHECK(pwrite(fd, data, sizeof(data), 8192) == sizeof(data));
    CHECK(pread(fd, copy, sizeof(copy), 8192) == sizeof(copy));
    CHECK(memcmp(data, copy, sizeof(data)) == 0);
    CHECK(lseek(duplicate, 0, SEEK_CUR) == 2);
    CHECK(fcntl(fd, F_SETFL, O_APPEND) == 0);
    /* Recent musl uses pwritev2(RWF_NOAPPEND) for POSIX pwrite().  Exercise
     * the Linux pwrite64 syscall's distinct O_APPEND behavior explicitly. */
    CHECK(syscall(SYS_pwrite64, duplicate, "end", 3, 0) == 3);
    CHECK(lseek(fd, 0, SEEK_CUR) == 2);
    CHECK(pread(fd, small, 3, 28192) == 3);
    CHECK(memcmp(small, "end", 3) == 0);
    CHECK(fsync(fd) == 0);
    CHECK(close(duplicate) == 0 && close(fd) == 0);
    puts("WRITER_POSITIONAL_IO_PASS");
    return 0;
}

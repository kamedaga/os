#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    printf("STATAT_SMOKE FAIL line=%d errno=%d: %s\n", __LINE__, errno, #expr); return 1; \
} } while (0)

int main(void)
{
    char temp[] = "/tmp/statat-smoke-XXXXXX";
    struct stat a, b;
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(mkdtemp(temp) != NULL);
    int dir = open(temp, O_RDONLY | O_DIRECTORY);
    CHECK(dir >= 0);
    int fd = openat(dir, "data", O_RDWR | O_CREAT | O_EXCL, 0640);
    CHECK(fd >= 0);
    CHECK(write(fd, "abc", 3) == 3);
    CHECK(fstat(fd, &a) == 0 && a.st_size == 3);
    CHECK(fstatat(dir, "data", &b, 0) == 0 && b.st_ino == a.st_ino && b.st_size == 3 && S_ISREG(b.st_mode));
    CHECK(fstatat(-123, temp, &b, 0) == 0 && S_ISDIR(b.st_mode));
    CHECK(fstatat(-123, "data", &b, 0) == -1 && errno == EBADF);
    CHECK(fstatat(dir, "", &b, 0) == -1 && errno == ENOENT);
    CHECK(fstatat(fd, "", &b, AT_EMPTY_PATH) == 0 && b.st_ino == a.st_ino);
    CHECK(fstatat(dir, "data", &b, 0x80000000) == -1 && errno == EINVAL);
    CHECK(fstatat(dir, "data", &b, AT_NO_AUTOMOUNT) == 0);
    CHECK(symlinkat("data", dir, "sym") == 0);
    CHECK(fstatat(dir, "sym", &b, 0) == 0 && b.st_ino == a.st_ino);
    CHECK(fstatat(dir, "sym", &b, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(b.st_mode) && b.st_size == 4);
    CHECK(symlinkat("absent", dir, "dangling") == 0);
    CHECK(fstatat(dir, "dangling", &b, 0) == -1 && errno == ENOENT);
    CHECK(fstatat(dir, "dangling", &b, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(b.st_mode));
    CHECK(linkat(dir, "data", dir, "hard", 0) == 0);
    CHECK(fstatat(dir, "hard", &b, 0) == 0 && b.st_ino == a.st_ino && b.st_nlink == 2);
    CHECK(lseek(fd, 1, SEEK_SET) == 1);
    for (unsigned i = 0; i < 1000; ++i) {
        CHECK(fstatat(dir, "data", &b, 0) == 0 && b.st_size == 3);
        CHECK(fstatat(dir, "absent", &b, 0) == -1 && errno == ENOENT);
    }
    char c;
    CHECK(read(fd, &c, 1) == 1 && c == 'b');
    CHECK(pwrite(fd, "d", 1, 3) == 1);
    CHECK(fstatat(dir, "data", &b, 0) == 0 && b.st_size == 4);
    CHECK(renameat(dir, "data", dir, "renamed") == 0);
    CHECK(fstatat(dir, "data", &b, 0) == -1 && errno == ENOENT);
    CHECK(fstatat(dir, "renamed", &b, 0) == 0 && b.st_size == 4);
    CHECK(faccessat(dir, "renamed", F_OK, 0) == 0);
    CHECK(stat("/dev/null", &b) == 0 && S_ISCHR(b.st_mode));
    CHECK(stat("/proc/self/status", &b) == 0 && S_ISREG(b.st_mode));
    char proc_fd[64];
    snprintf(proc_fd, sizeof(proc_fd), "/proc/self/fd/%d", fd);
    CHECK(stat(proc_fd, &b) == 0 && b.st_ino == a.st_ino && b.st_size == 4);
    CHECK(close(fd) == 0);
    CHECK(unlinkat(dir, "renamed", 0) == 0);
    CHECK(unlinkat(dir, "hard", 0) == 0);
    CHECK(unlinkat(dir, "sym", 0) == 0);
    CHECK(unlinkat(dir, "dangling", 0) == 0);
    CHECK(close(dir) == 0 && rmdir(temp) == 0);
    printf("STATAT_SMOKE PASS checks=%u\n", checks);
    return 0;
}

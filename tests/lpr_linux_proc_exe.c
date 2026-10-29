/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>

int main(void)
{
    char target[PATH_MAX], pid_path[64];
    const ssize_t bytes = readlink("/proc/self/exe", target, sizeof(target) - 1);
    if (bytes <= 0 || target[0] != '/') return 1;
    target[bytes] = '\0';
    struct stat link, followed, actual;
    if (lstat("/proc/self/exe", &link) || !S_ISLNK(link.st_mode)) return 2;
    if (link.st_size != bytes) return 3;
    if (stat("/proc/self/exe", &followed) || !S_ISREG(followed.st_mode)) return 4;
    if (stat(target, &actual) || actual.st_ino != followed.st_ino ||
        actual.st_dev != followed.st_dev) return 5;
    snprintf(pid_path, sizeof(pid_path), "/proc/%ld", (long)getpid());
    errno = 0;
    if (readlink(pid_path, target, sizeof(target)) != -1 || errno != EINVAL) return 6;
    /* musl realpath walks each component with readlink; PID directories must
     * report EINVAL rather than ENOENT before it can reach the exe link. */
    char *resolved = realpath("/proc/self/exe", NULL);
    if (!resolved || resolved[0] != '/') return 7;
    if (stat(resolved, &actual) || actual.st_ino != followed.st_ino ||
        actual.st_dev != followed.st_dev) return 8;
    char at_target[PATH_MAX];
    /* Go uses the Linux readlinkat syscall directly, unlike musl readlink. */
    if (syscall(SYS_readlinkat, AT_FDCWD, "/proc/self/exe", at_target,
                sizeof(at_target)) != bytes || memcmp(at_target, resolved, bytes)) return 9;
    char directory[] = "/tmp/lpr-readlinkat-XXXXXX";
    if (!mkdtemp(directory)) return 10;
    int dir = open(directory, O_RDONLY | O_DIRECTORY);
    if (dir < 0 || symlinkat("readlink-target", dir, "link")) return 11;
    if (syscall(SYS_readlinkat, dir, "link", at_target, 1) != 1 ||
        at_target[0] != 'r') return 12;
    unlinkat(dir, "link", 0);
    close(dir);
    rmdir(directory);
    errno = 0;
    if (syscall(SYS_readlinkat, -1, "self/exe", at_target,
                sizeof(at_target)) != -1 || errno != EBADF) return 12;
    free(resolved);
    puts("PROC_EXE: readlink, symlink metadata, followed metadata and realpath PASS");
    return 0;
}

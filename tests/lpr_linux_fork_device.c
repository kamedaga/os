#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int check_devices(const char *phase)
{
    const char *paths[] = {"/dev/null", "/dev/zero", "/dev/urandom"};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        /* Non-interactive ash closes stdin in its forked async child and
         * expects the next open to reuse 0, without a separate dup2 call. */
        if (i == 0 && close(STDIN_FILENO) != 0 && errno != EBADF) return 4;
        errno = 0;
        int fd = open(paths[i], O_RDONLY | O_CLOEXEC);
        int saved_errno = errno;
        errno = 0;
        long direct = syscall(SYS_openat, AT_FDCWD, paths[i], O_RDONLY, 0);
        printf("FORK_DEVICE phase=%s path=%s libc_fd=%d errno=%d syscall_fd=%ld syscall_errno=%d\n",
            phase, paths[i], fd, saved_errno, direct, errno);
        if (fd != STDIN_FILENO || direct < 0) return 1;
        if (dup2(fd, STDIN_FILENO) != STDIN_FILENO) return 2;
        if (i == 0) {
            char byte;
            if (read(STDIN_FILENO, &byte, 1) != 0) return 3;
        }
        close(fd);
        close((int)direct);
    }
    int pipefd[2];
    if (pipe(pipefd) || pipefd[0] != STDIN_FILENO) return 5;
    if (close(pipefd[0]) || dup(pipefd[1]) != STDIN_FILENO) return 6;
    close(STDIN_FILENO);
    close(pipefd[1]);
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (check_devices("parent")) return 1;
    pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) _exit(check_devices("child"));
    int status;
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 3;
    puts("FORK_DEVICE PASS open dup2 read after fork");
    return 0;
}

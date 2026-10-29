#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int available(int fd)
{
    int bytes = -1;
    assert(ioctl(fd, FIONREAD, &bytes) == 0);
    return bytes;
}

int main(void)
{
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    assert(available(pipefd[0]) == 0 && available(pipefd[1]) == 0);
    assert(write(pipefd[1], "abcdef", 6) == 6);
    assert(available(pipefd[0]) == 6 && available(pipefd[1]) == 6);
    assert(available(pipefd[0]) == 6); /* Inspection must not consume bytes. */
    char data[6];
    assert(read(pipefd[0], data, 2) == 2 && memcmp(data, "ab", 2) == 0);
    assert(available(pipefd[0]) == 4);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(dup2(pipefd[0], STDIN_FILENO) == STDIN_FILENO);
        _exit(available(STDIN_FILENO) == 4 ? 0 : 1);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && status == 0);
    assert(ioctl(pipefd[0], FIONREAD, NULL) == -1 && errno == EFAULT);
    close(pipefd[1]);
    assert(available(pipefd[0]) == 4);
    assert(read(pipefd[0], data, sizeof(data)) == 4 && memcmp(data, "cdef", 4) == 0);
    assert(available(pipefd[0]) == 0 && read(pipefd[0], data, 1) == 0);
    close(pipefd[0]);
    puts("PIPE_AVAILABLE=PASS empty both-ends non-consuming partial-read inherited-stdin EOF null");
    return 0;
}

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t handled_signals;

static void signal_handler(int signo)
{
    if (signo == SIGUSR1) handled_signals++;
}

static int fd_is_open(int fd)
{
    for (;;) {
        errno = 0;
        const int result = fcntl(fd, F_GETFD);
        if (result >= 0) return 1;
        if (errno == EINTR) continue;
        return errno == EBADF ? 0 : -1;
    }
}

static int close_until_closed(int fd)
{
    for (;;) {
        (void)close(fd);
        const int open = fd_is_open(fd);
        if (open == 0) return 0;
        if (open < 0) return -1;
    }
}

static int wait_child(pid_t child, int *status)
{
    for (;;) {
        const pid_t result = waitpid(child, status, 0);
        if (result == child) return 0;
        if (result < 0 && errno == EINTR) continue;
        return -1;
    }
}

static int exec_pipe_writer_mode(const char *fd_text)
{
    char *end = NULL;
    errno = 0;
    const long parsed = strtol(fd_text, &end, 10);
    if (errno != 0 || end == fd_text || *end != '\0' || parsed < 0 ||
        parsed > 0x7fffffffL)
    {
        return 30;
    }
    const int fd = (int)parsed;
    static const char payload[] = "exec-pipe-eof";
    if (write(fd, payload, sizeof(payload) - 1u) !=
        (ssize_t)(sizeof(payload) - 1u))
    {
        return 31;
    }
    return close(fd) == 0 ? 0 : 32;
}

static int check_exec_pipe_eof(void)
{
    int pipe_fds[2];
    if (pipe2(pipe_fds, 0) != 0) return 33;

    const pid_t writer = fork();
    if (writer < 0) return 34;
    if (writer == 0) {
        (void)close(pipe_fds[0]);
        char fd_text[32];
        if (snprintf(fd_text, sizeof(fd_text), "%d", pipe_fds[1]) <= 0)
            _exit(125);
        execl(
            "/cmd/lpr_exec_pipe_race.elf",
            "lpr_exec_pipe_race.elf",
            "--pipe-writer",
            fd_text,
            (char *)0);
        _exit(126);
    }

    if (close(pipe_fds[1]) != 0) return 35;
    static const char expected[] = "exec-pipe-eof";
    char got[sizeof(expected)];
    size_t used = 0;
    while (used < sizeof(expected) - 1u) {
        const ssize_t n = read(
            pipe_fds[0], got + used, sizeof(expected) - 1u - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 36;
        used += (size_t)n;
    }
    if (memcmp(got, expected, sizeof(expected) - 1u) != 0) return 37;

    char byte = 0;
    ssize_t eof;
    do {
        eof = read(pipe_fds[0], &byte, 1);
    } while (eof < 0 && errno == EINTR);
    if (eof != 0) return 38;
    if (close(pipe_fds[0]) != 0) return 39;

    int status = 0;
    if (wait_child(writer, &status) != 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
    {
        return 40;
    }
    puts("LPR_EXEC_PIPE_EOF_DONE");
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--pipe-writer") == 0)
        return exec_pipe_writer_mode(argv[2]);

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, 0) != 0) return 10;

    puts("LPR_EXEC_PIPE_RACE_START");
    fflush(stdout);

    const int exec_pipe_status = check_exec_pipe_eof();
    if (exec_pipe_status != 0) return exec_pipe_status;

    const pid_t signaler = fork();
    if (signaler < 0) return 11;
    if (signaler == 0) {
        const pid_t parent = getppid();
        for (int i = 0; i < 128; ++i) {
            if (kill(parent, SIGUSR1) != 0) _exit(0);
        }
        _exit(0);
    }

    int leaked_read_fd = -1;
    int iterations = 0;
    for (; iterations < 16 && leaked_read_fd < 0; ++iterations) {
        int pipe_fds[2];
        if (pipe2(pipe_fds, 0) != 0) return 12;

        /* musl intentionally maps close(2)'s EINTR to success because Linux
         * has already detached the descriptor before reporting a late error. */
        if (close(pipe_fds[0]) != 0) return 13;
        const int read_open = fd_is_open(pipe_fds[0]);
        if (read_open < 0) return 14;
        if (read_open != 0) leaked_read_fd = pipe_fds[0];

        if (close_until_closed(pipe_fds[1]) != 0) return 15;
    }

    (void)kill(signaler, SIGTERM);
    int signaler_status = 0;
    if (wait_child(signaler, &signaler_status) != 0) return 16;

    printf(
        "LPR_EXEC_PIPE_RACE_WINDOW iterations=%d signals=%d leaked_read=%d\n",
        iterations,
        (int)handled_signals,
        leaked_read_fd >= 0 ? 1 : 0);
    fflush(stdout);

    const pid_t probe = fork();
    if (probe < 0) return 17;
    if (probe == 0) {
        const int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0) _exit(126);
        if (null_fd != STDIN_FILENO) (void)close(null_fd);
        execl("/bin/sync", "sync", (char *)0);
        _exit(127);
    }
    int probe_status = 0;
    if (wait_child(probe, &probe_status) != 0 ||
        !WIFEXITED(probe_status) || WEXITSTATUS(probe_status) != 0)
    {
        return 18;
    }

    if (leaked_read_fd >= 0 && close_until_closed(leaked_read_fd) != 0) return 19;
    puts("LPR_EXEC_PIPE_RACE_DONE");
    return 0;
}

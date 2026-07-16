#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char runtime_dir[] = "/run/user/0";
static const char seatd_socket[] = "/run/user/0/seatd.sock";

static int ensure_directory(const char *path, mode_t mode)
{
    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        return 1;
    }
    return 0;
}

static int prepare_runtime(void)
{
    int status = ensure_directory("/run", 0755);
    if (status != 0) return 10 + status;
    status = ensure_directory("/run/user", 0755);
    if (status != 0) return 20 + status;
    status = ensure_directory(runtime_dir, 0700);
    if (status != 0) return 30 + status;
    if (unlink(seatd_socket) != 0 && errno != ENOENT) {
        return 41;
    }
    return 0;
}

static int prepare_environment(void)
{
    static const struct {
        const char *name;
        const char *value;
    } environment[] = {
        { "XDG_RUNTIME_DIR", "/run/user/0" },
        { "LIBSEAT_BACKEND", "seatd" },
        { "SEATD_SOCK", "/run/user/0/seatd.sock" },
        { "SEATD_VTBOUND", "0" },
        { "WLR_BACKENDS", "drm,libinput" },
        { "WLR_RENDERER", "gles2" },
        { "WLR_RENDERER_ALLOW_SOFTWARE", "1" },
        { "FONTCONFIG_FILE", "/etc/fonts/fonts.conf" },
        { "PATH", "/bin:/usr/bin:/cmd" },
        { "HOME", "/home" },
        { "SHELL", "/bin/bash" },
        { "TERM", "xterm" },
        { "LD_LIBRARY_PATH", "/lib/linux:/usr/lib" },
    };
    if (unsetenv("LP_NUM_THREADS") != 0 || unsetenv("LD_PRELOAD") != 0) {
        return -1;
    }
    for (size_t i = 0; i < sizeof(environment) / sizeof(environment[0]); i++) {
        if (setenv(environment[i].name, environment[i].value, 1) != 0) {
            return -1;
        }
    }
    return 0;
}

static int wait_for_child(pid_t child, int *status)
{
    pid_t result;
    do {
        result = waitpid(child, status, 0);
    } while (result < 0 && errno == EINTR);
    return result == child ? 0 : -1;
}

static int stop_seatd(pid_t seatd)
{
    int result = 0;
    if (kill(seatd, SIGTERM) != 0 && errno != ESRCH) {
        result = -1;
    }
    int status = 0;
    const pid_t waited = waitpid(seatd, &status, WNOHANG);
    if (waited == seatd || (waited < 0 && errno == ECHILD)) {
        return result;
    }
    if (waited < 0) result = -1;
    if (kill(seatd, SIGKILL) != 0 && errno != ESRCH) {
        result = -1;
    }
    if (wait_for_child(seatd, &status) != 0) {
        result = -1;
    }
    return result;
}

static pid_t start_seatd(int read_fd, int write_fd)
{
    const pid_t child = fork();
    if (child != 0) {
        return child;
    }
    (void)close(read_fd);
    char notify_fd[32];
    if (snprintf(notify_fd, sizeof(notify_fd), "%d", write_fd) <= 0) {
        _exit(127);
    }
    execl("/usr/bin/seatd", "seatd", "-n", notify_fd,
          "-z", (char *)NULL);
    _exit(127);
}

static int wait_for_ready_byte(int fd)
{
    unsigned char ready;
    ssize_t received;
    do {
        received = read(fd, &ready, sizeof(ready));
    } while (received < 0 && errno == EINTR);
    return received == 1 ? 0 : -1;
}

static int readiness_failure_code(pid_t child)
{
    int status = 0;
    if (wait_for_child(child, &status) != 0) {
        return 36;
    }
    if (WIFEXITED(status)) {
        const int code = WEXITSTATUS(status);
        return code < 64 ? 64 + code : 127;
    }
    if (WIFSIGNALED(status)) {
        const int signal = WTERMSIG(status);
        return signal < 31 ? 96 + signal : 126;
    }
    return 125;
}

static pid_t start_shell(int *release_fd)
{
    int gate[2];
    if (release_fd == NULL || pipe(gate) != 0) {
        return -1;
    }
    const pid_t child = fork();
    if (child < 0) {
        (void)close(gate[0]);
        (void)close(gate[1]);
        return child;
    }
    if (child != 0) {
        (void)close(gate[0]);
        *release_fd = gate[1];
        return child;
    }
    (void)close(gate[1]);
    if (setpgid(0, 0) != 0) {
        _exit(127);
    }
    unsigned char release;
    ssize_t received;
    do {
        received = read(gate[0], &release, sizeof(release));
    } while (received < 0 && errno == EINTR);
    (void)close(gate[0]);
    if (received != 1) {
        perror("wait foreground shell release");
        _exit(127);
    }
    execl("/bin/bash", "bash", "--noprofile", "--norc", "-i",
        (char *)NULL);
    perror("exec shell");
    _exit(127);
}

static int release_child(int fd)
{
    const unsigned char release = 1;
    ssize_t written;
    do {
        written = write(fd, &release, sizeof(release));
    } while (written < 0 && errno == EINTR);
    const int close_status = close(fd);
    return written == 1 && close_status == 0 ? 0 : -1;
}

static int child_exit_code(int status)
{
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

int main(void)
{
    const int runtime_status = prepare_runtime();
    if (runtime_status != 0) {
        perror("prepare runtime");
        return runtime_status;
    }
    if (prepare_environment() != 0) {
        perror("prepare environment");
        return 21;
    }
    int ready_pipe[2];
    if (pipe(ready_pipe) != 0) {
        perror("pipe seatd readiness");
        return 31;
    }
    const pid_t seatd = start_seatd(ready_pipe[0], ready_pipe[1]);
    if (seatd < 0) {
        perror("fork seatd");
        (void)close(ready_pipe[0]);
        (void)close(ready_pipe[1]);
        return 32;
    }
    (void)close(ready_pipe[1]);
    const int ready_status = wait_for_ready_byte(ready_pipe[0]);
    const int ready_errno = errno;
    (void)close(ready_pipe[0]);
    errno = ready_errno;
    if (ready_status != 0) {
        perror("seatd readiness");
        const int failure = readiness_failure_code(seatd);
        return failure;
    }
    int shell_release_fd = -1;
    const pid_t shell = start_shell(&shell_release_fd);
    if (shell < 0) {
        perror("fork shell");
        (void)stop_seatd(seatd);
        return 41;
    }
    if (setpgid(shell, shell) != 0 && errno != EACCES) {
        perror("set shell process group");
        (void)close(shell_release_fd);
        (void)kill(shell, SIGTERM);
        int ignored_status = 0;
        (void)wait_for_child(shell, &ignored_status);
        (void)stop_seatd(seatd);
        return 42;
    }
    if (tcsetpgrp(STDIN_FILENO, shell) != 0) {
        perror("set foreground shell");
        (void)close(shell_release_fd);
        (void)kill(shell, SIGTERM);
        int ignored_status = 0;
        (void)wait_for_child(shell, &ignored_status);
        (void)stop_seatd(seatd);
        return 43;
    }
    if (release_child(shell_release_fd) != 0) {
        perror("release foreground shell");
        (void)kill(shell, SIGTERM);
        int ignored_status = 0;
        (void)wait_for_child(shell, &ignored_status);
        (void)stop_seatd(seatd);
        return 44;
    }
    int shell_status = 0;
    if (wait_for_child(shell, &shell_status) != 0) {
        perror("wait shell");
        (void)stop_seatd(seatd);
        return 45;
    }
    const int exit_code = child_exit_code(shell_status);
    if (exit_code != 0) {
        fprintf(stderr, "pacha-user-session: shell exited with status %d\n",
            exit_code);
    }
    if (stop_seatd(seatd) != 0 && exit_code == 0) {
        return 1;
    }
    return exit_code;
}

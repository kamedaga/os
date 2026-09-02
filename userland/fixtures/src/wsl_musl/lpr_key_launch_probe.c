#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *runtime_dir = "/run/user/0";

static uint64_t now_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

static void path_for(char *buffer, size_t capacity, const char *app, const char *suffix)
{
    (void)snprintf(buffer, capacity, "%s/key-phase-%s.%s", runtime_dir, app, suffix);
}

static void emit(const char *format, ...)
{
    char line[768];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(line, sizeof(line) - 2u, format, arguments);
    va_end(arguments);
    if (length < 0) return;
    if ((size_t)length > sizeof(line) - 2u) length = (int)sizeof(line) - 2;
    line[length++] = '\n';
    const int fd = open("/dev/hvc0", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    size_t offset = 0;
    while (offset < (size_t)length) {
        const ssize_t written = write(fd, line + offset, (size_t)length - offset);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        break;
    }
    (void)close(fd);
}

static int write_pid_file(const char *path, pid_t pid)
{
    char text[32];
    const int length = snprintf(text, sizeof(text), "%ld\n", (long)pid);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    const ssize_t written = write(fd, text, (size_t)length);
    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
    return written == length ? 0 : -1;
}

static pid_t read_pid_file(const char *path)
{
    char text[32];
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    const ssize_t count = read(fd, text, sizeof(text) - 1u);
    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
    if (count <= 0) return -1;
    text[count] = '\0';
    char *end = NULL;
    const long value = strtol(text, &end, 10);
    if (end == text || value <= 0) return -1;
    return (pid_t)value;
}

static int redirect_log(const char *path)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        const int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }
    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) (void)close(fd);
    return 0;
}

static void exec_app(const char *app, const char *log_path)
{
    if (redirect_log(log_path) != 0) _exit(126);
    if (strcmp(app, "foot") == 0) {
        execl("/usr/bin/foot", "foot", "/bin/sh", "-c", "exec /bin/sleep 60", (char *)NULL);
    } else {
        (void)setenv("GDK_BACKEND", "wayland", 1);
        if (strcmp(getenv("KEY_PHASE_GLYCIN_TRACE") != NULL ?
                   getenv("KEY_PHASE_GLYCIN_TRACE") : "", "1") == 0) {
            (void)setenv("LD_PRELOAD", "/cmd/lpr_glycin_timing_trace.so", 1);
        }
        execl("/usr/bin/thunar", "thunar", (char *)NULL);
    }
    _exit(errno == ENOENT ? 127 : 126);
}

static int launch(const char *app)
{
    char pid_path[128];
    char done_path[128];
    char log_path[128];
    path_for(pid_path, sizeof(pid_path), app, "pid");
    path_for(done_path, sizeof(done_path), app, "done");
    path_for(log_path, sizeof(log_path), app, "log");
    (void)unlink(done_path);
    (void)unlink(log_path);

    const uint64_t launcher_ns = now_ns();
    emit("KEY_PHASE_PROBE app=%s stage=launcher_start guest_mono_ns=%llu launcher_pid=%ld",
        app, (unsigned long long)launcher_ns, (long)getpid());
    const uint64_t fork_begin_ns = now_ns();
    const pid_t child = fork();
    const uint64_t fork_end_ns = now_ns();
    if (child < 0) {
        emit("KEY_PHASE_FAIL stage=%s_fork errno=%d", app, errno);
        return 1;
    }
    if (child == 0) exec_app(app, log_path);
    if (write_pid_file(pid_path, child) != 0) {
        (void)kill(child, SIGTERM);
        emit("KEY_PHASE_FAIL stage=%s_pid_file errno=%d", app, errno);
        return 1;
    }
    emit("KEY_PHASE_PROBE app=%s stage=process_created guest_mono_ns=%llu pid=%ld "
         "fork_ns=%llu",
        app,
        (unsigned long long)fork_end_ns,
        (long)child,
        (unsigned long long)(fork_end_ns - fork_begin_ns));

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        status = 255 << 8;
        break;
    }
    const int exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    emit("KEY_PHASE_PROBE app=%s stage=process_exit guest_mono_ns=%llu pid=%ld status=%d",
        app, (unsigned long long)now_ns(), (long)child, exit_status);
    const int done_fd = open(done_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (done_fd >= 0) (void)close(done_fd);
    return 0;
}

static int close_app(const char *app)
{
    char pid_path[128];
    path_for(pid_path, sizeof(pid_path), app, "pid");
    const pid_t pid = read_pid_file(pid_path);
    emit("KEY_PHASE_CONTROL app=%s stage=close_start guest_mono_ns=%llu pid=%ld",
        app, (unsigned long long)now_ns(), (long)(pid > 0 ? pid : 0));
    /* This is benchmark cleanup after the last visible-frame timestamp, not
     * an application shutdown measurement.  Foot exits on SIGTERM, which also
     * lets the diagnostic runtime flush its startup profile.  Thunar handles
     * SIGTERM without exiting in some session configurations, so only it needs
     * deterministic SIGKILL cleanup. */
    const int signal_number = strcmp(app, "foot") == 0 ? SIGTERM : SIGKILL;
    if (pid > 0 && kill(pid, signal_number) != 0 && errno != ESRCH) {
        emit("KEY_PHASE_FAIL stage=%s_close errno=%d", app, errno);
        return 1;
    }
    emit("KEY_PHASE_CONTROL app=%s stage=close_requested guest_mono_ns=%llu pid=%ld",
        app, (unsigned long long)now_ns(), (long)(pid > 0 ? pid : 0));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[2], "foot") != 0 && strcmp(argv[2], "thunar") != 0)) {
        emit("KEY_PHASE_FAIL stage=invalid_probe");
        return 2;
    }
    if (strcmp(argv[1], "launch") == 0) return launch(argv[2]);
    if (strcmp(argv[1], "close") == 0) return close_app(argv[2]);
    emit("KEY_PHASE_FAIL stage=invalid_probe");
    return 2;
}

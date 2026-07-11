#define _XOPEN_SOURCE 600

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_command(const char *mode)
{
    pid_t child = fork();
    if (child < 0) {
        return 13;
    }
    if (child == 0) {
        if (strcmp(mode, "grep") == 0) {
            execl("/cmd/busybox", "busybox", "grep", "-q", "^pty-teardown$",
                "/tmp/pty_teardown_input", (char *)NULL);
        } else if (strcmp(mode, "sleep") == 0) {
            execl("/cmd/busybox", "busybox", "sleep", "0", (char *)NULL);
        }
        _exit(24);
    }

    int status = 0;
    if (waitpid(child, &status, 0) != child) {
        return 14;
    }
    if (!WIFEXITED(status)) {
        return 16;
    }
    return WEXITSTATUS(status);
}

static int run_session(int master, const char *path)
{
    pid_t session = fork();
    if (session < 0) {
        return 13;
    }
    if (session == 0) {
        close(master);
        if (setsid() < 0) {
            _exit(20);
        }
        int slave = open(path, O_RDWR);
        if (slave < 0) {
            _exit(21);
        }
        if (tcsetpgrp(slave, getpgrp()) != 0) {
            _exit(22);
        }
        if (tcgetpgrp(slave) != getpgrp()) {
            _exit(25);
        }
        if (dup2(slave, STDIN_FILENO) < 0 ||
            dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0)
        {
            _exit(23);
        }
        if (slave > STDERR_FILENO) {
            close(slave);
        }
        for (int i = 0; i < 3; i++) {
            int status = run_command("grep");
            if (status == 0) {
                status = run_command("sleep");
            }
            if (status != 0) {
                _exit(status);
            }
        }
        _exit(0);
    }

    int status = 0;
    if (waitpid(session, &status, 0) != session) {
        return 14;
    }
    if (!WIFEXITED(status)) {
        return 16;
    }
    return WEXITSTATUS(status);
}

static int run_pty_sequence(void)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        return 10;
    }
    char *slave_name = ptsname(master);
    if (slave_name == NULL) {
        close(master);
        return 11;
    }
    char path[64];
    if (snprintf(path, sizeof(path), "%s", slave_name) >= (int)sizeof(path)) {
        close(master);
        return 12;
    }

    int status = run_session(master, path);
    if (close(master) != 0) {
        return 15;
    }
    return status;
}

int main(void)
{
    return run_pty_sequence();
}

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int wait_with_timeout(pid_t child, int *status, int ticks) {
    for (int tick = 0; tick < ticks; ++tick) {
        const pid_t result = waitpid(child, status, WNOHANG);
        if (result == child) {
            return 0;
        }
        if (result < 0 && errno != EINTR) {
            return -1;
        }
        usleep(100000);
    }
    (void)kill(child, SIGKILL);
    while (waitpid(child, status, 0) < 0 && errno == EINTR) {}
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: lpr_sway_launcher command [args...]\n");
        return 2;
    }
    (void)unlink("/run/seatd.sock");
    (void)setenv("LIBSEAT_BACKEND", "seatd", 1);
    (void)setenv("SEATD_SOCK", "/run/seatd.sock", 1);
    (void)setenv("SEATD_VTBOUND", "0", 1);
    /* card0 has no render node; wlroots must opt in before selecting llvmpipe. */
    (void)setenv("WLR_RENDERER_ALLOW_SOFTWARE", "1", 1);
    const pid_t seatd = fork();
    if (seatd < 0) {
        perror("fork seatd");
        return 3;
    }
    if (seatd == 0) {
        execl("/usr/bin/seatd", "seatd", "-l", "debug", (char *)NULL);
        perror("exec seatd");
        _exit(126);
    }
    printf("M51_LAUNCHER_SEATD_PID=%d\n", (int)seatd);
    fflush(stdout);
    sleep(1);

    const pid_t sway = fork();
    if (sway < 0) {
        perror("fork sway");
        (void)kill(seatd, SIGTERM);
        return 4;
    }
    if (sway == 0) {
        execv(argv[1], &argv[1]);
        perror("exec sway");
        _exit(126);
    }
    printf("M51_LAUNCHER_SWAY_PID=%d\n", (int)sway);
    fflush(stdout);

    if (getenv("M55_FIRST_FRAME") != NULL) {
        int first_frame_status = 0;
        for (int tick = 0; tick < 350; ++tick) {
            const pid_t result = waitpid(sway, &first_frame_status, WNOHANG);
            if (result == sway) {
                (void)kill(seatd, SIGKILL);
                fprintf(stderr, "M55_SWAY_EXITED_BEFORE_FRAME\n");
                return WIFEXITED(first_frame_status) ? WEXITSTATUS(first_frame_status) : 125;
            }
            if (result < 0 && errno != EINTR) {
                (void)kill(seatd, SIGKILL);
                perror("waitpid sway first frame");
                return 5;
            }
            usleep(100000);
        }
        printf("M55_SWAY_FIRST_FRAME_READY\n");
        fflush(stdout);
    }

    const char *client_path = getenv("M51_CLIENT");
    if (client_path != NULL && client_path[0] != '\0') {
        sleep(3);
        const pid_t client = fork();
        if (client == 0) {
            execl(client_path, client_path, (char *)NULL);
            perror("exec Wayland client");
            _exit(126);
        }
        if (client > 0) {
            int client_status = 0;
            const int client_timeout = wait_with_timeout(client, &client_status, 100);
            if (client_timeout > 0) {
                printf("M51_LAUNCHER_CLIENT_TIMEOUT=10\n");
            } else if (client_timeout == 0 && WIFEXITED(client_status)) {
                printf("M51_LAUNCHER_CLIENT_EXIT=%d\n", WEXITSTATUS(client_status));
            } else if (client_timeout == 0 && WIFSIGNALED(client_status)) {
                printf("M51_LAUNCHER_CLIENT_SIGNAL=%d\n", WTERMSIG(client_status));
            }
            fflush(stdout);
        }
        (void)kill(sway, SIGKILL);
    }

    int status = 0;
    const int timed_out = wait_with_timeout(sway, &status, 300);
    (void)kill(seatd, SIGKILL);
    (void)unlink("/run/seatd.sock");

    if (timed_out > 0) {
        printf("M51_LAUNCHER_SWAY_TIMEOUT=30\n");
        return 124;
    }
    if (timed_out < 0) {
        perror("waitpid sway");
        return 5;
    }
    if (WIFEXITED(status)) {
        printf("M51_LAUNCHER_SWAY_EXIT=%d\n", WEXITSTATUS(status));
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        printf("M51_LAUNCHER_SWAY_SIGNAL=%d\n", WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return 6;
}

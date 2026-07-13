#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int lifecycle_failed;
static int wait_with_timeout(pid_t child, int *status, int ticks);

static const char *runtime_dir(void) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    return runtime != NULL && runtime[0] != '\0' ? runtime : "/tmp";
}

static const char *wayland_display(void) {
    const char *display = getenv("WAYLAND_DISPLAY");
    return display != NULL && display[0] != '\0' ? display : "wayland-1";
}

static int wait_for_wayland_socket(int ticks) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", runtime_dir(), wayland_display());
    for (int tick = 0; tick < ticks; ++tick) {
        if (access(path, F_OK) == 0) return 0;
        usleep(100000);
    }
    return -1;
}

static int find_sway_ipc_socket(char *path, size_t capacity) {
    DIR *dir = opendir(runtime_dir());
    if (dir == NULL) return -1;
    int found = -1;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "sway-ipc.", 9) != 0) continue;
        const size_t length = strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name + length - 5, ".sock") != 0) continue;
        if (snprintf(path, capacity, "%s/%s", runtime_dir(), entry->d_name) < (int)capacity)
            found = 0;
        break;
    }
    closedir(dir);
    return found;
}

static int request_sway_exit(void) {
    char path[256];
    for (int tick = 0; tick < 100; ++tick) {
        if (find_sway_ipc_socket(path, sizeof(path)) == 0) break;
        if (tick == 99) return -1;
        usleep(100000);
    }
    const pid_t client = fork();
    if (client < 0) return -1;
    if (client == 0) {
        execl("/usr/bin/swaymsg", "swaymsg", "-s", path, "exit", (char *)NULL);
        _exit(126);
    }
    int status = 0;
    return wait_with_timeout(client, &status, 50) == 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static void cleanup_runtime_sockets(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", runtime_dir(), wayland_display());
    (void)unlink(path);
    snprintf(path, sizeof(path), "%s/%s.lock", runtime_dir(), wayland_display());
    (void)unlink(path);
    DIR *dir = opendir(runtime_dir());
    if (dir != NULL) {
        const struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp(entry->d_name, "sway-ipc.", 9) != 0) continue;
            snprintf(path, sizeof(path), "%s/%s", runtime_dir(), entry->d_name);
            (void)unlink(path);
        }
        closedir(dir);
    }
    (void)unlink("/run/seatd.sock");
}

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
    if (kill(child, SIGKILL) != 0 && errno != ESRCH) return -1;
    pid_t result;
    do {
        result = waitpid(child, status, 0);
    } while (result < 0 && errno == EINTR);
    return result == child ? 1 : -1;
}

static void terminate_and_reap(pid_t child, int signal_number, const char *name) {
    if (child <= 0) return;
    if (kill(child, signal_number) != 0 && errno != ESRCH) lifecycle_failed = 1;
    int status = 0;
    const int result = wait_with_timeout(child, &status, 50);
    if (result != 0 && !(result < 0 && errno == ECHILD)) {
        lifecycle_failed = 1;
        printf("M56_LIFECYCLE_REAP_FAIL process=%s result=%d errno=%d\n",
               name, result, errno);
    }
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
        /* PachaOS PRIME has no implicit fence. Zero raster threads makes
         * llvmpipe finish each scene synchronously before wlroots' glFlush. */
        (void)setenv("LP_NUM_THREADS", "0", 1);
        execv(argv[1], &argv[1]);
        perror("exec sway");
        _exit(126);
    }
    printf("M51_LAUNCHER_SWAY_PID=%d\n", (int)sway);
    fflush(stdout);
    const char *lifecycle_mode = getenv("M56_LIFECYCLE_MODE");
    const int lifecycle = lifecycle_mode != NULL && lifecycle_mode[0] != '\0';
    if (lifecycle && wait_for_wayland_socket(300) != 0) {
        lifecycle_failed = 1;
        printf("M56_LIFECYCLE_WAYLAND_TIMEOUT\n");
    }

    if (getenv("M55_FIRST_FRAME") != NULL) {
        int first_frame_status = 0;
        /* Cold llvmpipe (fresh shader cache) needs ~50s+ to reach the first
         * page flips; the wait must outlast that or READY races the frame.
         * Sway creates its IPC socket ~10s (warm) to ~12s+ (cold) before the
         * first output commit lands, so pad generously after seeing it. */
        int ipc_seen_tick = -1;
        for (int tick = 0; tick < 1200; ++tick) {
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
            if (ipc_seen_tick < 0) {
                DIR *dir = opendir("/tmp");
                if (dir != NULL) {
                    const struct dirent *entry;
                    while ((entry = readdir(dir)) != NULL) {
                        if (strncmp(entry->d_name, "sway-ipc", 8) == 0) {
                            ipc_seen_tick = tick;
                            printf("M55_SWAY_IPC_SEEN tick=%d\n", tick);
                            fflush(stdout);
                            break;
                        }
                    }
                    closedir(dir);
                }
            } else if (tick - ipc_seen_tick >= 250) {
                break;
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
        if (!lifecycle) (void)kill(sway, SIGKILL);
    }

    if (lifecycle && !lifecycle_failed) {
        if (strcmp(lifecycle_mode, "normal") == 0) {
            if (request_sway_exit() != 0 &&
                kill(sway, SIGINT) != 0 && errno != ESRCH)
                lifecycle_failed = 1;
        } else if (strcmp(lifecycle_mode, "term") == 0) {
            if (kill(sway, SIGTERM) != 0) lifecycle_failed = 1;
        } else if (strcmp(lifecycle_mode, "kill") == 0) {
            if (kill(sway, SIGKILL) != 0) lifecycle_failed = 1;
        } else {
            lifecycle_failed = 1;
        }
    }

    int status = 0;
    const int timed_out = wait_with_timeout(sway, &status, lifecycle ? 30 : 300);
    terminate_and_reap(seatd, SIGTERM, "seatd");
    cleanup_runtime_sockets();
    if (lifecycle && timed_out < 0) lifecycle_failed = 1;
    int extra_status = 0;
    if (lifecycle && waitpid(-1, &extra_status, WNOHANG) > 0) lifecycle_failed = 1;
    if (lifecycle) {
        if (timed_out > 0)
            printf("M56_LIFECYCLE_ESCALATED mode=%s signal=9\n", lifecycle_mode);
        if (!lifecycle_failed) {
            printf("M56_LIFECYCLE_CLEAN mode=%s orphan=0 stale=0 waitpid=1\n",
                   lifecycle_mode);
            return 0;
        }
        printf("M56_LIFECYCLE_FAIL mode=%s timeout=%d\n", lifecycle_mode, timed_out);
        return 1;
    }

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

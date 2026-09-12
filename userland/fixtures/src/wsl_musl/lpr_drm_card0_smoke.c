#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DRM_IOCTL_BASE 'd'
#define DRM_IOCTL_VERSION _IOWR(DRM_IOCTL_BASE, 0x00, struct drm_version)
#define DRM_IOCTL_GET_CAP _IOWR(DRM_IOCTL_BASE, 0x0c, struct drm_get_cap)
#define DRM_CAP_DUMB_BUFFER 0x1u
#define DRM_CAP_PRIME 0x5u

struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
};

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

static int kill_open_client(const char *path, int render)
{
    int ready[2];
    if (pipe(ready) != 0) return -1;
    const pid_t child = fork();
    if (child < 0) {
        close(ready[0]);
        close(ready[1]);
        return -1;
    }
    if (child == 0) {
        close(ready[0]);
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        struct drm_get_cap cap = {
            .capability = render ? DRM_CAP_PRIME : DRM_CAP_DUMB_BUFFER,
        };
        const char marker = 'R';
        if (fd < 0 || ioctl(fd, DRM_IOCTL_GET_CAP, &cap) != 0 ||
            cap.value != (render ? 3u : 1u) || write(ready[1], &marker, 1) != 1)
            _exit(1);
        for (;;) pause();
    }
    close(ready[1]);
    char marker = 0;
    const int ready_status = read(ready[0], &marker, 1) == 1 && marker == 'R';
    close(ready[0]);
    if (!ready_status || kill(child, SIGKILL) != 0) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, NULL, 0);
        return -1;
    }
    int status = 0;
    return waitpid(child, &status, 0) == child &&
        WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL ? 0 : -1;
}

static int reopen_client(const char *path, int render)
{
    const pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        struct drm_get_cap cap = {
            .capability = render ? DRM_CAP_PRIME : DRM_CAP_DUMB_BUFFER,
        };
        const int query = fd >= 0 ? ioctl(fd, DRM_IOCTL_GET_CAP, &cap) : -1;
        const int closed = fd >= 0 ? close(fd) : -1;
        _exit(query == 0 && closed == 0 &&
            cap.value == (render ? 3u : 1u) ? 0 : 1);
    }
    int status = 0;
    return waitpid(child, &status, 0) == child &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int restart_sandbox(const char *path, int render, int old_fd)
{
    struct drm_get_cap cap = {
        .capability = render ? DRM_CAP_PRIME : DRM_CAP_DUMB_BUFFER,
    };
    if (ioctl(old_fd, DRM_IOCTL_GET_CAP, &cap) != 0)
        return -1;

    const int marker = open("/tmp/gpud-restart-ready",
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (marker < 0 || write(marker, "1\n", 2) != 2 || close(marker) != 0) {
        if (marker >= 0) close(marker);
        close(old_fd);
        return -1;
    }

    const struct timespec pause = { .tv_nsec = 10000000 };
    int retired = 0;
    for (unsigned i = 0; i < 6000; ++i) {
        cap.value = 0;
        if (ioctl(old_fd, DRM_IOCTL_GET_CAP, &cap) != 0) {
            retired = 1;
            break;
        }
        nanosleep(&pause, NULL);
    }
    (void)close(old_fd);
    if (!retired)
        return -1;

    const int new_fd = open(path, O_RDWR | O_CLOEXEC);
    cap.value = 0;
    if (new_fd < 0 || ioctl(new_fd, DRM_IOCTL_GET_CAP, &cap) != 0 ||
        cap.value != (render ? 3u : 1u)) {
        if (new_fd >= 0) close(new_fd);
        return -1;
    }
    printf("GPUD_RESTART_CLIENT_OK old_generation_retired=1 reopen=1\n");
    return new_fd;
}

int main(int argc, char **argv)
{
    const int render = argc >= 2 && strcmp(argv[1], "--render") == 0;
    const int restart = argc == 3 && strcmp(argv[2], "--restart") == 0;
    if ((argc != 1 && !render) || argc > 3 || (argc == 3 && !restart)) return 64;
    const char *path = render ? "/dev/dri/renderD128" : "/dev/dri/card0";
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "%s open failed: errno=%d\n", path, errno);
        return 1;
    }
    if (restart) {
        fd = restart_sandbox(path, render, fd);
        if (fd < 0) {
            fprintf(stderr, "%s reopen after sandbox restart failed: errno=%d\n",
                path, errno);
            return 2;
        }
    }
    const int duplicate = dup(fd);
    if (duplicate < 0) {
        fprintf(stderr, "%s dup failed: errno=%d\n", path, errno);
        close(fd);
        return 2;
    }
    if (close(fd) != 0) {
        fprintf(stderr, "%s original close failed: errno=%d\n", path, errno);
        close(duplicate);
        return 3;
    }
    const pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "%s fork failed: errno=%d\n", path, errno);
        close(duplicate);
        return 4;
    }
    if (child == 0) {
        struct drm_get_cap child_cap = {
            .capability = render ? DRM_CAP_PRIME : DRM_CAP_DUMB_BUFFER,
        };
        const int query = ioctl(duplicate, DRM_IOCTL_GET_CAP, &child_cap);
        const int closed = close(duplicate);
        _exit(query == 0 && closed == 0 &&
            child_cap.value == (render ? 3u : 1u) ? 0 : 1);
    }
    int child_status = 0;
    if (waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        fprintf(stderr, "%s fork lease child failed: status=%d errno=%d\n",
            path, child_status, errno);
        close(duplicate);
        return 4;
    }

    char name[64] = {0};
    char date[32] = {0};
    char desc[128] = {0};
    struct drm_version version = {
        .name_len = sizeof(name) - 1,
        .name = name,
        .date_len = sizeof(date) - 1,
        .date = date,
        .desc_len = sizeof(desc) - 1,
        .desc = desc,
    };
    if (ioctl(duplicate, DRM_IOCTL_VERSION, &version) != 0 ||
        version.name_len == 0 || name[0] == '\0') {
        fprintf(stderr, "DRM_IOCTL_VERSION failed: errno=%d name_len=%zu\n", errno, version.name_len);
        close(duplicate);
        return 5;
    }

    struct drm_get_cap cap = {
        .capability = render ? DRM_CAP_PRIME : DRM_CAP_DUMB_BUFFER,
    };
    if (ioctl(duplicate, DRM_IOCTL_GET_CAP, &cap) != 0) {
        fprintf(stderr, "DRM_IOCTL_GET_CAP failed: errno=%d\n", errno);
        close(duplicate);
        return 6;
    }
    if (close(duplicate) != 0) {
        fprintf(stderr, "%s last close failed: errno=%d\n", path, errno);
        return 7;
    }
    if (kill_open_client(path, render) != 0) {
        fprintf(stderr, "%s forced client termination failed: errno=%d\n", path, errno);
        return 8;
    }
    if (reopen_client(path, render) != 0) {
        fprintf(stderr, "%s reopen after client recovery failed: errno=%d\n", path, errno);
        return 9;
    }
    if (render && (strcmp(name, "virtio_gpu") != 0 || cap.value != 3)) return 11;
    printf(render ?
        "DRM_RENDER_OK name=%s version=%d.%d.%d prime=%llu dup=1 fork_lease=1 last_close=1 client_kill=1 reopen_client=1\n" :
        "DRM_CARD0_OK name=%s version=%d.%d.%d dumb=%llu dup=1 fork_lease=1 last_close=1 client_kill=1 reopen_client=1\n",
        name,
        version.version_major,
        version.version_minor,
        version.version_patchlevel,
        (unsigned long long)cap.value);
    return cap.value == (render ? 3u : 1u) ? 0 : 11;
}

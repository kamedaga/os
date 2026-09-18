#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DRM_IOCTL_BASE 'd'
#define DRM_IOCTL_VERSION _IOWR(DRM_IOCTL_BASE, 0x00, struct drm_version)
#define DRM_IOCTL_GET_CAP _IOWR(DRM_IOCTL_BASE, 0x0c, struct drm_get_cap)
#define DRM_IOCTL_GEM_CLOSE _IOW(DRM_IOCTL_BASE, 0x09, struct drm_gem_close)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD \
    _IOWR(DRM_IOCTL_BASE, 0x2d, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE \
    _IOWR(DRM_IOCTL_BASE, 0x2e, struct drm_prime_handle)
#define DRM_IOCTL_SYNCOBJ_CREATE _IOWR(DRM_IOCTL_BASE, 0xbf, struct drm_syncobj_create)
#define DRM_IOCTL_SYNCOBJ_DESTROY _IOWR(DRM_IOCTL_BASE, 0xc0, struct drm_syncobj_destroy)
#define DRM_IOCTL_SYNCOBJ_WAIT _IOWR(DRM_IOCTL_BASE, 0xc3, struct drm_syncobj_wait)
#define DRM_IOCTL_SYNCOBJ_RESET _IOWR(DRM_IOCTL_BASE, 0xc4, struct drm_syncobj_array)
#define DRM_IOCTL_SYNCOBJ_SIGNAL _IOWR(DRM_IOCTL_BASE, 0xc5, struct drm_syncobj_array)
#define DRM_IOCTL_VIRTGPU_MAP \
    _IOWR(DRM_IOCTL_BASE, 0x40 + 0x01, struct drm_virtgpu_map)
#define DRM_IOCTL_VIRTGPU_EXECBUFFER _IOWR(DRM_IOCTL_BASE, 0x42, struct drm_virtgpu_execbuffer)
#define DRM_IOCTL_VIRTGPU_GETPARAM _IOWR(DRM_IOCTL_BASE, 0x43, struct drm_virtgpu_getparam)
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE _IOWR(DRM_IOCTL_BASE, 0x44, struct drm_virtgpu_resource_create)
#define DRM_IOCTL_VIRTGPU_RESOURCE_INFO _IOWR(DRM_IOCTL_BASE, 0x45, struct drm_virtgpu_resource_info)
#define DRM_IOCTL_VIRTGPU_GET_CAPS _IOWR(DRM_IOCTL_BASE, 0x49, struct drm_virtgpu_get_caps)
#define DRM_IOCTL_VIRTGPU_CONTEXT_INIT _IOWR(DRM_IOCTL_BASE, 0x4b, struct drm_virtgpu_context_init)
#define DRM_CAP_DUMB_BUFFER 0x1u
#define DRM_CAP_PRIME 0x5u
#define DRM_CAP_SYNCOBJ 0x13u
#define DRM_SYNCOBJ_CREATE_SIGNALED 0x1u
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT 0x2u
#define VIRTGPU_EXECBUF_SYNCOBJ_RESET 0x1u

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

struct drm_virtgpu_getparam {
    uint64_t param;
    uint64_t value;
};

struct drm_virtgpu_map {
    uint64_t offset;
    uint32_t handle;
    uint32_t pad;
};

struct drm_virtgpu_execbuffer {
    uint32_t flags, size;
    uint64_t command, bo_handles;
    uint32_t num_bo_handles;
    int32_t fence_fd;
    uint32_t ring_idx, syncobj_stride, num_in_syncobjs, num_out_syncobjs;
    uint64_t in_syncobjs, out_syncobjs;
};

struct drm_virtgpu_execbuffer_syncobj {
    uint32_t handle, flags;
    uint64_t point;
};

struct drm_virtgpu_context_set_param {
    uint64_t param, value;
};

struct drm_virtgpu_context_init {
    uint32_t num_params, pad;
    uint64_t ctx_set_params;
};

struct drm_gem_close {
    uint32_t handle;
    uint32_t pad;
};

struct drm_prime_handle {
    uint32_t handle, flags;
    int32_t fd;
};

struct drm_syncobj_create {
    uint32_t handle, flags;
};

struct drm_syncobj_destroy {
    uint32_t handle, pad;
};

struct drm_syncobj_wait {
    uint64_t handles;
    int64_t timeout_nsec;
    uint32_t count_handles, flags, first_signaled, pad;
    uint64_t deadline_nsec;
};

struct drm_syncobj_array {
    uint64_t handles;
    uint32_t count_handles, pad;
};

struct drm_virtgpu_resource_create {
    uint32_t target, format, bind, width, height, depth, array_size;
    uint32_t last_level, nr_samples, flags, bo_handle, res_handle, size, stride;
};

struct drm_virtgpu_resource_info {
    uint32_t bo_handle, res_handle, size, blob_mem;
};

struct drm_virtgpu_get_caps {
    uint32_t cap_set_id, cap_set_ver;
    uint64_t addr;
    uint32_t size, pad;
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

static int mapping_churn(int fd)
{
    /* Keep the DRM file open while exceeding the service's 64 mapping slots.
     * Each VMA must outlive GEM_CLOSE, then retire its lease on munmap. */
    for (unsigned i = 0; i < 96; ++i) {
        struct drm_virtgpu_resource_create create = {
            .target = 2, .format = 1, .bind = 2,
            .width = 16, .height = 16, .depth = 1, .array_size = 1,
            .size = 4096, .stride = 64,
        };
        if (ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &create) != 0)
            return -1;
        struct drm_virtgpu_map map = {.handle = create.bo_handle};
        if (ioctl(fd, DRM_IOCTL_VIRTGPU_MAP, &map) != 0) {
            fprintf(stderr, "DRM_MAP_CHURN_FAIL iteration=%u errno=%d\n", i, errno);
            return -1;
        }
        struct drm_virtgpu_map repeated = {.handle = create.bo_handle};
        if (!map.offset ||
            ioctl(fd, DRM_IOCTL_VIRTGPU_MAP, &repeated) != 0 ||
            repeated.offset != map.offset)
            return -1;
        volatile uint32_t *view = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
            MAP_SHARED, fd, map.offset);
        if (view == MAP_FAILED) return -1;
        view[0] = i;
        struct drm_gem_close close_gem = {.handle = create.bo_handle};
        if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_gem) != 0 || view[0] != i)
            return -1;
        view[1] = ~i;
        if (view[1] != ~i || munmap((void *)view, 4096) != 0)
            return -1;
    }
    puts("DRM_MAP_CHURN_OK iterations=96 gem_close_vma_alive=1");
    return 0;
}

int main(int argc, char **argv)
{
    const int render = argc >= 2 && strcmp(argv[1], "--render") == 0;
    const int restart = argc == 3 && strcmp(argv[2], "--restart") == 0;
    if ((argc != 1 && !render) || argc > 3 || (argc == 3 && !restart)) return 64;
    const char *path = render ? "/dev/dri/renderD128" : "/dev/dri/card0";
    int fd = open(path, O_RDWR | O_CLOEXEC);
    volatile uint32_t *mapped_resource = MAP_FAILED;
    size_t mapped_resource_size = 0;
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

    struct pollfd pollfd = {.fd = duplicate, .events = POLLIN};
    char event[32];
    if (poll(&pollfd, 1, 0) != 0 || pollfd.revents != 0 ||
        fcntl(duplicate, F_SETFL, O_NONBLOCK) != 0 ||
        read(duplicate, event, sizeof(event)) != -1 || errno != EAGAIN) {
        fprintf(stderr, "%s poll/read gate failed: revents=%x errno=%d\n",
            path, pollfd.revents, errno);
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
    uint32_t virgl = 0;
    const char *context_mode = "none";
    struct drm_virtgpu_getparam getparam = {
        .param = 1,
        .value = (uintptr_t)&virgl,
    };
    if (render && (ioctl(duplicate, DRM_IOCTL_VIRTGPU_GETPARAM, &getparam) != 0 ||
        virgl != 1)) {
        fprintf(stderr, "DRM_IOCTL_VIRTGPU_GETPARAM failed: errno=%d virgl=%u\n",
            errno, virgl);
        close(duplicate);
        return 6;
    }
    if (render) {
        struct drm_get_cap syncobj_cap = {.capability = DRM_CAP_SYNCOBJ};
        struct drm_syncobj_create syncobj = {0};
        if (ioctl(duplicate, DRM_IOCTL_GET_CAP, &syncobj_cap) != 0 ||
            syncobj_cap.value != 1 ||
            ioctl(duplicate, DRM_IOCTL_SYNCOBJ_CREATE, &syncobj) != 0 ||
            !syncobj.handle) {
            fprintf(stderr, "DRM syncobj create failed: errno=%d cap=%llu handle=%u\n",
                errno, (unsigned long long)syncobj_cap.value, syncobj.handle);
            close(duplicate);
            return 6;
        }
        uint32_t syncobj_handle = syncobj.handle;
        struct drm_syncobj_array syncobj_array = {
            .handles = (uintptr_t)&syncobj_handle, .count_handles = 1,
        };
        struct drm_syncobj_wait syncobj_wait = {
            .handles = (uintptr_t)&syncobj_handle, .timeout_nsec = 0,
            .count_handles = 1,
        };
        if (ioctl(duplicate, DRM_IOCTL_SYNCOBJ_SIGNAL, &syncobj_array) != 0 ||
            ioctl(duplicate, DRM_IOCTL_SYNCOBJ_WAIT, &syncobj_wait) != 0 ||
            syncobj_wait.first_signaled != 0 ||
            ioctl(duplicate, DRM_IOCTL_SYNCOBJ_RESET, &syncobj_array) != 0) {
            fprintf(stderr, "DRM syncobj signal/wait/reset failed: errno=%d first=%u\n",
                errno, syncobj_wait.first_signaled);
            close(duplicate);
            return 6;
        }
        syncobj_wait.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
        errno = 0;
        if (ioctl(duplicate, DRM_IOCTL_SYNCOBJ_WAIT, &syncobj_wait) == 0 ||
            (errno != ETIME && errno != ETIMEDOUT)) {
            fprintf(stderr, "DRM syncobj reset timeout failed: errno=%d\n", errno);
            close(duplicate);
            return 6;
        }
        struct drm_syncobj_destroy syncobj_destroy = {.handle = syncobj.handle};
        if (ioctl(duplicate, DRM_IOCTL_SYNCOBJ_DESTROY, &syncobj_destroy) != 0) {
            fprintf(stderr, "DRM syncobj destroy failed: errno=%d\n", errno);
            close(duplicate);
            return 6;
        }
        unsigned char caps[4096] = {0};
        struct drm_virtgpu_get_caps get_caps = {
            .cap_set_id = 1,
            .cap_set_ver = 1,
            .addr = (uintptr_t)caps,
            .size = sizeof(caps),
        };
        if (ioctl(duplicate, DRM_IOCTL_VIRTGPU_GET_CAPS, &get_caps) != 0) {
            fprintf(stderr, "DRM_IOCTL_VIRTGPU_GET_CAPS failed: errno=%d\n", errno);
            close(duplicate);
            return 6;
        }
        size_t caps_byte = 0;
        while (caps_byte < sizeof(caps) && caps[caps_byte] == 0) ++caps_byte;
        if (caps_byte == sizeof(caps)) {
            fprintf(stderr, "DRM_IOCTL_VIRTGPU_GET_CAPS returned no capability data\n");
            close(duplicate);
            return 6;
        }
        uint32_t context_init_supported = 0;
        getparam.param = 6;
        getparam.value = (uintptr_t)&context_init_supported;
        if (ioctl(duplicate, DRM_IOCTL_VIRTGPU_GETPARAM, &getparam) != 0 ||
            context_init_supported > 1) {
            fprintf(stderr, "virtgpu context-init support query failed: errno=%d value=%u\n",
                errno, context_init_supported);
            close(duplicate);
            return 6;
        }
        struct drm_virtgpu_context_set_param context_param = {
            .param = 1, .value = 1,
        };
        struct drm_virtgpu_context_init context = {
            .num_params = 1,
            .ctx_set_params = (uintptr_t)&context_param,
        };
        errno = 0;
        const int context_status = ioctl(duplicate, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &context);
        if ((context_init_supported && context_status != 0) ||
            (!context_init_supported && (context_status == 0 || errno != EINVAL))) {
            fprintf(stderr,
                "DRM_IOCTL_VIRTGPU_CONTEXT_INIT capability mismatch: supported=%u status=%d errno=%d\n",
                context_init_supported, context_status, errno);
            close(duplicate);
            return 6;
        }
        context_mode = context_init_supported ? "explicit" : "implicit";
        const uint32_t nop = 0;
        struct drm_syncobj_create exec_input = {
            .flags = DRM_SYNCOBJ_CREATE_SIGNALED,
        };
        struct drm_syncobj_create exec_output = {0};
        if (ioctl(duplicate, DRM_IOCTL_SYNCOBJ_CREATE, &exec_input) != 0 ||
            ioctl(duplicate, DRM_IOCTL_SYNCOBJ_CREATE, &exec_output) != 0) {
            fprintf(stderr, "exec syncobj create failed: errno=%d\n", errno);
            close(duplicate);
            return 6;
        }
        struct drm_virtgpu_execbuffer_syncobj input_dependency = {
            .handle = exec_input.handle,
            .flags = VIRTGPU_EXECBUF_SYNCOBJ_RESET,
        };
        struct drm_virtgpu_execbuffer_syncobj output_fence = {
            .handle = exec_output.handle,
        };
        struct drm_virtgpu_execbuffer exec = {
            .size = sizeof(nop),
            .command = (uintptr_t)&nop,
            .fence_fd = -1,
            .syncobj_stride = sizeof(input_dependency),
            .num_in_syncobjs = 1,
            .num_out_syncobjs = 1,
            .in_syncobjs = (uintptr_t)&input_dependency,
            .out_syncobjs = (uintptr_t)&output_fence,
        };
        if (ioctl(duplicate, DRM_IOCTL_VIRTGPU_EXECBUFFER, &exec) != 0) {
            fprintf(stderr, "DRM_IOCTL_VIRTGPU_EXECBUFFER failed: errno=%d\n", errno);
            close(duplicate);
            return 6;
        }
        struct timespec now;
        uint32_t output_handle = exec_output.handle;
        struct drm_syncobj_wait output_wait = {
            .handles = (uintptr_t)&output_handle,
            .count_handles = 1,
        };
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            fprintf(stderr, "clock_gettime for exec fence failed: errno=%d\n", errno);
            close(duplicate);
            return 6;
        }
        output_wait.timeout_nsec = (int64_t)now.tv_sec * 1000000000ll +
            now.tv_nsec + 5000000000ll;
        struct drm_syncobj_destroy destroy_input = {.handle = exec_input.handle};
        struct drm_syncobj_destroy destroy_output = {.handle = exec_output.handle};
        if (ioctl(duplicate, DRM_IOCTL_SYNCOBJ_WAIT, &output_wait) != 0 ||
            ioctl(duplicate, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy_input) != 0 ||
            ioctl(duplicate, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy_output) != 0) {
            fprintf(stderr, "exec syncobj fence failed: errno=%d\n", errno);
            close(duplicate);
            return 6;
        }
        struct drm_virtgpu_resource_create create = {
            .target = 2, .format = 1, .bind = 2,
            .width = 16, .height = 16, .depth = 1, .array_size = 1,
            .size = 1024, .stride = 64,
        };
        if (ioctl(duplicate, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &create) != 0 ||
            create.bo_handle == 0 || create.res_handle == 0) {
            fprintf(stderr, "DRM_IOCTL_VIRTGPU_RESOURCE_CREATE failed: errno=%d bo=%u res=%u\n",
                errno, create.bo_handle, create.res_handle);
            close(duplicate);
            return 6;
        }
        struct drm_virtgpu_resource_info info = {.bo_handle = create.bo_handle};
        struct drm_gem_close gem_close = {.handle = create.bo_handle};
        struct drm_virtgpu_map map = {.handle = create.bo_handle};
        if (ioctl(duplicate, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info) != 0 ||
            info.res_handle != create.res_handle || info.size < create.size ||
            ioctl(duplicate, DRM_IOCTL_VIRTGPU_MAP, &map) != 0 || !map.offset ||
            (mapped_resource = mmap(NULL, info.size,
                PROT_READ | PROT_WRITE, MAP_SHARED, duplicate, map.offset)) == MAP_FAILED) {
            fprintf(stderr, "virtgpu resource map failed: errno=%d size=%u offset=%llu\n",
                errno, info.size, (unsigned long long)map.offset);
            close(duplicate);
            return 6;
        }
        mapped_resource_size = info.size;
        mapped_resource[0] = UINT32_C(0x6b62326d);
        if (mapped_resource[0] != UINT32_C(0x6b62326d)) {
            fprintf(stderr, "virtgpu mapped resource verification failed\n");
            close(duplicate);
            return 6;
        }
        struct drm_prime_handle exported = {
            .handle = create.bo_handle,
            .flags = O_CLOEXEC | O_RDWR,
            .fd = -1,
        };
        int shared_client = -1;
        volatile uint32_t *mapped_prime = MAP_FAILED;
        struct drm_prime_handle imported = {.fd = -1};
        if (ioctl(duplicate, DRM_IOCTL_PRIME_HANDLE_TO_FD, &exported) != 0 ||
            exported.fd < 0 ||
            (mapped_prime = mmap(NULL, info.size, PROT_READ | PROT_WRITE,
                MAP_SHARED, exported.fd, 0)) == MAP_FAILED ||
            (shared_client = open(path, O_RDWR | O_CLOEXEC)) < 0) {
            fprintf(stderr, "PRIME export/map/open failed: errno=%d dma_fd=%d\n",
                errno, exported.fd);
            close(duplicate);
            return 6;
        }
        imported.fd = exported.fd;
        if (ioctl(shared_client, DRM_IOCTL_PRIME_FD_TO_HANDLE, &imported) != 0 ||
            !imported.handle) {
            fprintf(stderr, "PRIME import failed: errno=%d handle=%u\n",
                errno, imported.handle);
            close(duplicate);
            return 6;
        }
        mapped_resource[0] = UINT32_C(0x5052494d);
        mapped_prime[1] = UINT32_C(0x53484152);
        if (mapped_prime[0] != UINT32_C(0x5052494d) ||
            mapped_resource[1] != UINT32_C(0x53484152) ||
            ioctl(duplicate, DRM_IOCTL_GEM_CLOSE, &gem_close) != 0 ||
            close(exported.fd) != 0) {
            fprintf(stderr, "PRIME alias/source-close failed: errno=%d\n", errno);
            close(duplicate);
            return 6;
        }
        struct drm_virtgpu_resource_info shared_info = {
            .bo_handle = imported.handle,
        };
        struct drm_gem_close imported_close = {.handle = imported.handle};
        mapped_prime[2] = UINT32_C(0x4c494645);
        if (mapped_resource[2] != UINT32_C(0x4c494645) ||
            ioctl(shared_client, DRM_IOCTL_VIRTGPU_RESOURCE_INFO,
                &shared_info) != 0 || shared_info.res_handle != create.res_handle ||
            ioctl(shared_client, DRM_IOCTL_GEM_CLOSE, &imported_close) != 0 ||
            close(shared_client) != 0 ||
            munmap((void *)mapped_prime, info.size) != 0) {
            fprintf(stderr, "PRIME shared lifetime failed: errno=%d\n", errno);
            close(duplicate);
            return 6;
        }
    }
    if (render && mapping_churn(duplicate) != 0) {
        close(duplicate);
        return 7;
    }
    if (close(duplicate) != 0) {
        fprintf(stderr, "%s last close failed: errno=%d\n", path, errno);
        return 7;
    }
    if (mapped_resource != MAP_FAILED) {
        mapped_resource[1] = UINT32_C(0x70616368);
        if (mapped_resource[0] != UINT32_C(0x5052494d) ||
            mapped_resource[1] != UINT32_C(0x70616368) ||
            munmap((void *)mapped_resource, mapped_resource_size) != 0) {
            fprintf(stderr, "virtgpu mapping lifetime failed: errno=%d\n", errno);
            return 7;
        }
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
        "DRM_RENDER_OK name=%s version=%d.%d.%d prime=%llu prime_share=1 virgl=%u caps=1 syncobj=1 context_gate=1 context_mode=%s exec=1 resource=1 mmap=1 poll=1 read=1 dup=1 fork_lease=1 last_close=1 client_kill=1 reopen_client=1\n" :
        "DRM_CARD0_OK name=%s version=%d.%d.%d dumb=%llu dup=1 fork_lease=1 last_close=1 client_kill=1 reopen_client=1\n",
        name,
        version.version_major,
        version.version_minor,
        version.version_patchlevel,
        (unsigned long long)cap.value,
        virgl,
        context_mode);
    return cap.value == (render ? 3u : 1u) ? 0 : 11;
}

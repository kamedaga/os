#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Linux DRM UAPI layouts; no libdrm dependency in the guest probe. */
struct resource_create {
    uint32_t target, format, bind, width, height, depth, array_size;
    uint32_t last_level, nr_samples, flags, bo_handle, res_handle, size, stride;
};
struct gem_close { uint32_t handle, pad; };
#define RESOURCE_CREATE _IOWR('d', 0x44, struct resource_create)
#define GEM_CLOSE _IOW('d', 0x09, struct gem_close)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    unsigned counts[3] = {0};
    for (unsigned round = 0; round < 3; ++round) {
        int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (fd < 0) { perror("render open"); return 1; }
        uint32_t handles[40];
        unsigned n = 0;
        for (; n < 40; ++n) {
            struct resource_create resource = {
                .target = 2, .format = 1, .bind = 2,
                .width = 1920, .height = 1080, .depth = 1, .array_size = 1,
                .size = 1920 * 1080 * 4, .stride = 1920 * 4,
            };
            if (ioctl(fd, RESOURCE_CREATE, &resource)) {
                printf("GPU_PRESSURE round=%u allocated=%u errno=%d\n", round, n, errno);
                if (errno != ENOMEM) return 1;
                break;
            }
            handles[n] = resource.bo_handle;
        }
        counts[round] = n;
        for (unsigned i = 0; i < n; ++i) {
            struct gem_close release = {.handle = handles[i]};
            if (ioctl(fd, GEM_CLOSE, &release)) { perror("gem close"); return 1; }
        }
        if (close(fd)) return 1;
        /* Virtio resource unrefs complete asynchronously. */
        sleep(2);
    }
    printf("GPU_PRESSURE counts=%u,%u,%u\n", counts[0], counts[1], counts[2]);
    return !counts[0] || counts[2] + 1 < counts[0];
}

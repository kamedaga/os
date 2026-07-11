#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DRM_IOCTL_BASE 'd'
#define DRM_IOCTL_VERSION _IOWR(DRM_IOCTL_BASE, 0x00, struct drm_version)
#define DRM_IOCTL_GET_CAP _IOWR(DRM_IOCTL_BASE, 0x0c, struct drm_get_cap)
#define DRM_CAP_DUMB_BUFFER 0x1u

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

int main(void)
{
    const int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "card0 open failed: errno=%d\n", errno);
        return 1;
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
    if (ioctl(fd, DRM_IOCTL_VERSION, &version) != 0 || version.name_len == 0 || name[0] == '\0') {
        fprintf(stderr, "DRM_IOCTL_VERSION failed: errno=%d name_len=%zu\n", errno, version.name_len);
        close(fd);
        return 2;
    }

    struct drm_get_cap cap = {
        .capability = DRM_CAP_DUMB_BUFFER,
    };
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) != 0) {
        fprintf(stderr, "DRM_IOCTL_GET_CAP failed: errno=%d\n", errno);
        close(fd);
        return 3;
    }
    if (close(fd) != 0) {
        fprintf(stderr, "card0 close failed: errno=%d\n", errno);
        return 4;
    }

    printf("DRM_CARD0_OK name=%s version=%d.%d.%d dumb=%llu\n",
        name,
        version.version_major,
        version.version_minor,
        version.version_patchlevel,
        (unsigned long long)cap.value);
    return cap.value == 1 ? 0 : 5;
}

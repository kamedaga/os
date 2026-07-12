#define _GNU_SOURCE
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

struct udev;
struct udev_device;
struct udev_enumerate;
struct udev_list_entry;
struct udev_monitor;

extern struct udev *udev_new(void);
extern struct udev *udev_unref(struct udev *);
extern struct udev_enumerate *udev_enumerate_new(struct udev *);
extern struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *);
extern int udev_enumerate_scan_devices(struct udev_enumerate *);
extern struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *);
extern struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *);
extern const char *udev_list_entry_get_name(struct udev_list_entry *);
extern struct udev_device *udev_device_new_from_syspath(struct udev *, const char *);
extern struct udev_device *udev_device_unref(struct udev_device *);
extern const char *udev_device_get_subsystem(struct udev_device *);
extern const char *udev_device_get_sysname(struct udev_device *);
extern const char *udev_device_get_devnode(struct udev_device *);
extern struct udev_monitor *udev_monitor_new_from_netlink(struct udev *, const char *);
extern struct udev_monitor *udev_monitor_unref(struct udev_monitor *);
extern int udev_monitor_enable_receiving(struct udev_monitor *);
extern int udev_monitor_get_fd(struct udev_monitor *);
extern char *drmGetDeviceNameFromFd2(int);

static long elapsed_ms(const struct timespec *start, const struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000L +
        (end->tv_nsec - start->tv_nsec) / 1000000L;
}

static int scan(struct udev *udev, int pass)
{
    struct udev_enumerate *enumerate = udev_enumerate_new(udev);
    if (enumerate == NULL || udev_enumerate_scan_devices(enumerate) != 0) {
        printf("M53_SCAN_PASS=%d status=failed\n", pass);
        if (enumerate != NULL) udev_enumerate_unref(enumerate);
        return 1;
    }
    int card0 = 0, event0 = 0, event1 = 0;
    for (struct udev_list_entry *entry = udev_enumerate_get_list_entry(enumerate);
         entry != NULL; entry = udev_list_entry_get_next(entry)) {
        const char *path = udev_list_entry_get_name(entry);
        struct udev_device *device = udev_device_new_from_syspath(udev, path);
        if (device == NULL) continue;
        const char *subsystem = udev_device_get_subsystem(device);
        const char *sysname = udev_device_get_sysname(device);
        const char *devnode = udev_device_get_devnode(device);
        if (subsystem != NULL && sysname != NULL) {
            if (strcmp(subsystem, "drm") == 0 && strcmp(sysname, "card0") == 0) card0 = 1;
            if (strcmp(subsystem, "input") == 0 && strcmp(sysname, "event0") == 0) event0 = 1;
            if (strcmp(subsystem, "input") == 0 && strcmp(sysname, "event1") == 0) event1 = 1;
            if ((strcmp(sysname, "card0") == 0 || strncmp(sysname, "event", 5) == 0))
                printf("M53_DEVICE subsystem=%s sysname=%s devnode=%s\n",
                    subsystem, sysname, devnode != NULL ? devnode : "-");
        }
        udev_device_unref(device);
    }
    udev_enumerate_unref(enumerate);
    printf("M53_SCAN_PASS=%d card0=%d event0=%d event1=%d\n",
        pass, card0, event0, event1);
    return !(card0 && event0 && event1);
}

int main(void)
{
    struct udev *udev = udev_new();
    if (udev == NULL) return 2;
    int failed = scan(udev, 1) | scan(udev, 2);
    const int drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    struct stat drm_stat = {0};
    char *drm_name = drm_fd >= 0 ? drmGetDeviceNameFromFd2(drm_fd) : NULL;
    const int drm_name_errno = errno;
    const int stat_status = drm_fd >= 0 ? fstat(drm_fd, &drm_stat) : -1;
    FILE *uevent = fopen("/sys/dev/char/226:0/uevent", "r");
    char *uevent_line = NULL;
    size_t uevent_capacity = 0;
    int devname = 0;
    if (uevent != NULL) {
        while (getline(&uevent_line, &uevent_capacity, uevent) >= 0)
            if (strcmp(uevent_line, "DEVNAME=dri/card0\n") == 0) devname = 1;
    }
    printf("M53_DRM_FD status=%d mode=%o major=%u minor=%u name=%s errno=%d uevent=%d devname=%d\n",
        stat_status, stat_status == 0 ? drm_stat.st_mode : 0,
        stat_status == 0 ? major(drm_stat.st_rdev) : 0,
        stat_status == 0 ? minor(drm_stat.st_rdev) : 0,
        drm_name != NULL ? drm_name : "-", drm_name_errno, uevent != NULL, devname);
    free(uevent_line);
    if (uevent != NULL) fclose(uevent);
    if (stat_status != 0 || major(drm_stat.st_rdev) != 226 ||
        minor(drm_stat.st_rdev) != 0 || drm_name == NULL ||
        strcmp(drm_name, "/dev/dri/card0") != 0) failed = 1;
    free(drm_name);
    if (drm_fd >= 0) close(drm_fd);
    struct udev_monitor *monitor = udev_monitor_new_from_netlink(udev, "udev");
    if (monitor == NULL || udev_monitor_enable_receiving(monitor) != 0) {
        printf("M53_MONITOR status=failed\n");
        failed = 1;
    } else {
        struct pollfd pfd = { .fd = udev_monitor_get_fd(monitor), .events = POLLIN };
        struct timespec start = {0}, end = {0};
        clock_gettime(CLOCK_MONOTONIC, &start);
        const int status = poll(&pfd, 1, 250);
        clock_gettime(CLOCK_MONOTONIC, &end);
        const long milliseconds = elapsed_ms(&start, &end);
        printf("M53_MONITOR status=ready poll=%d revents=%d idle_ms=%ld\n",
            status, pfd.revents, milliseconds);
        if (status != 0 || pfd.revents != 0 || milliseconds < 200) failed = 1;
    }
    if (monitor != NULL) udev_monitor_unref(monitor);
    udev_unref(udev);
    printf("M53_UDEV_DISCOVERY_STATUS=%d\n", failed);
    return failed;
}

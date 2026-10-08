/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <libudev.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* Use Xorg's enumeration contract without opening a GPU. Optional event
 * reads also check the host clock domain, separating namespace/evdev failures
 * from the physical display during integration testing. */
int main(int argc, char **argv)
{
    int watch = argc == 2 && !strcmp(argv[1], "--events");
    int pointer_fd = -1;
    struct udev *udev = udev_new();
    struct udev_enumerate *scan = udev ? udev_enumerate_new(udev) : NULL;
    int status = scan ? udev_enumerate_add_match_subsystem(scan, "input") : -1;
    if (!status) status = udev_enumerate_add_match_is_initialized(scan);
    if (!status) status = udev_enumerate_scan_devices(scan);
    if (status < 0) {
        fprintf(stderr, "INPUT_ENUM_FAIL stage=scan status=%d errno=%d\n", status, errno);
        udev_enumerate_unref(scan);
        udev_unref(udev);
        return 1;
    }
    unsigned devices = 0, keyboards = 0, pointers = 0;
    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(scan)) {
        struct udev_device *device = udev_device_new_from_syspath(udev,
            udev_list_entry_get_name(entry));
        if (!device) continue;
        const char *node = udev_device_get_devnode(device);
        const char *input = udev_device_get_property_value(device, "ID_INPUT");
        printf("INPUT_ENUM_ENTRY path=%s node=%s input=%s subsystem=%s\n",
            udev_list_entry_get_name(entry), node ? node : "-",
            input ? input : "-", udev_device_get_subsystem(device));
        if (node && input && input[0] == '1') {
            const char *keyboard = udev_device_get_property_value(device, "ID_INPUT_KEYBOARD");
            const char *pointer = udev_device_get_property_value(device, "ID_INPUT_MOUSE");
            ++devices;
            keyboards += keyboard && keyboard[0] == '1';
            pointers += pointer && pointer[0] == '1';
            printf("INPUT_DEVICE node=%s initialized=%d keyboard=%d pointer=%d\n",
                node, udev_device_get_is_initialized(device),
                keyboard && keyboard[0] == '1', pointer && pointer[0] == '1');
            if (watch && pointer && pointer[0] == '1' && pointer_fd < 0) {
                pointer_fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (pointer_fd < 0) {
                    printf("INPUT_EVENT_FAIL stage=open errno=%d\n", errno);
                    return 1;
                }
            }
        }
        udev_device_unref(device);
    }
    udev_enumerate_unref(scan);
    udev_unref(udev);
    printf("INPUT_ENUM_%s devices=%u keyboards=%u pointers=%u\n",
        devices ? "PASS" : "FAIL", devices, keyboards, pointers);
    if (watch) {
        struct input_id id = {0};
        int version = 0;
        int clock_id = CLOCK_MONOTONIC;
        char name[256] = {0};
        if (pointer_fd < 0 || ioctl(pointer_fd, EVIOCGVERSION, &version) < 0 ||
            ioctl(pointer_fd, EVIOCGID, &id) < 0 ||
            ioctl(pointer_fd, EVIOCGNAME(sizeof(name)), name) < 0 ||
            ioctl(pointer_fd, EVIOCSCLOCKID, &clock_id) < 0) {
            printf("INPUT_EVENT_FAIL stage=ioctl errno=%d\n", errno);
            return 1;
        }
        printf("INPUT_EVENT_READY name=%s version=%x bus=%x\n", name, version, id.bustype);
        fflush(stdout);
        unsigned motion = 0, buttons = 0, syncs = 0;
        struct pollfd pfd = {.fd = pointer_fd, .events = POLLIN};
        for (unsigned attempt = 0; attempt < 5 && !(motion && buttons && syncs); ++attempt) {
            int result = poll(&pfd, 1, 2000);
            if (result < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
            if (!result) continue;
            struct input_event events[64];
            ssize_t bytes = read(pointer_fd, events, sizeof(events));
            if (bytes < 0) {
                if (errno == EAGAIN) continue;
                printf("INPUT_EVENT_FAIL stage=read errno=%d\n", errno);
                return 1;
            }
            if (bytes == 0 || (size_t)bytes % sizeof(events[0])) return 1;
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 1;
            long long now_us = (long long)now.tv_sec * 1000000 + now.tv_nsec / 1000;
            for (size_t i = 0; i < (size_t)bytes / sizeof(events[0]); ++i) {
                long long event_us = (long long)events[i].time.tv_sec * 1000000 + events[i].time.tv_usec;
                long long age_us = now_us - event_us;
                printf("INPUT_EVENT_TIME age_us=%lld\n", age_us);
                if (age_us < 0 || age_us > 1000000) {
                    puts("INPUT_EVENT_FAIL stage=clock-domain");
                    return 1;
                }
                printf("INPUT_EVENT type=%u code=%u value=%d\n",
                    events[i].type, events[i].code, events[i].value);
                motion += events[i].type == EV_REL && events[i].value != 0;
                buttons += events[i].type == EV_KEY && events[i].code == BTN_LEFT;
                syncs += events[i].type == EV_SYN && events[i].code == SYN_REPORT;
            }
        }
        close(pointer_fd);
        int passed = motion && buttons && syncs;
        printf("INPUT_EVENT_%s motion=%u buttons=%u syncs=%u\n",
            passed ? "PASS" : "FAIL", motion, buttons, syncs);
        return passed ? 0 : 1;
    }
    return devices ? 0 : 1;
}

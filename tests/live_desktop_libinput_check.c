/* SPDX-License-Identifier: MIT */
#include <libinput.h>
#include <libudev.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Exercise the desktop's actual libinput/udev ABI without a display or GPU.
 * Events come from USB HID through inputd and LPR, never from a test injector
 * inside the guest. QEMU supplies movement/buttons at the physical boundary. */
static int open_device(const char *path, int flags, void *context)
{
    (void)context;
    int fd = open(path, flags);
    return fd < 0 ? -errno : fd;
}

static void close_device(int fd, void *context)
{
    (void)context;
    close(fd);
}

int main(int argc, char **argv)
{
    int hotplug = argc == 2 && !strcmp(argv[1], "--hotplug");
    const struct libinput_interface interface = {
        .open_restricted = open_device, .close_restricted = close_device,
    };
    struct udev *udev = udev_new();
    struct libinput *input = udev ? libinput_udev_create_context(&interface, NULL, udev) : NULL;
    if (!input || libinput_udev_assign_seat(input, "seat0") != 0) {
        fprintf(stderr, "LIBINPUT_FAIL stage=assign-seat errno=%d\n", errno);
        libinput_unref(input);
        udev_unref(udev);
        return 1;
    }
    unsigned devices = 0, pointers = 0, keyboards = 0, motion = 0, buttons = 0;
    unsigned pressed = 0, released = 0;
    unsigned removed = 0, restored = 0;
    puts("LIBINPUT_READY");
    fflush(stdout);
    struct pollfd pfd = {.fd = libinput_get_fd(input), .events = POLLIN};
    for (unsigned attempt = 0; attempt < 30; ++attempt) {
        if (libinput_dispatch(input) < 0) break;
        struct libinput_event *event;
        while ((event = libinput_get_event(input))) {
            enum libinput_event_type type = libinput_event_get_type(event);
            if (type == LIBINPUT_EVENT_DEVICE_ADDED) {
                struct libinput_device *device = libinput_event_get_device(event);
                ++devices;
                pointers += libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER);
                keyboards += libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD);
                restored += removed && libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER);
                printf("LIBINPUT_DEVICE name=%s\n", libinput_device_get_name(device));
            } else if (type == LIBINPUT_EVENT_DEVICE_REMOVED) {
                struct libinput_device *device = libinput_event_get_device(event);
                if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER)) {
                    ++removed;
                    /* Require fresh events after reconnection, not events
                     * already delivered by the removed device. */
                    motion = pressed = released = buttons = 0;
                    printf("LIBINPUT_REMOVED name=%s\n", libinput_device_get_name(device));
                }
            } else if (type == LIBINPUT_EVENT_POINTER_MOTION) {
                struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
                double dx = libinput_event_pointer_get_dx(pointer);
                double dy = libinput_event_pointer_get_dy(pointer);
                motion += dx != 0 || dy != 0;
                printf("LIBINPUT_MOTION dx=%.3f dy=%.3f\n", dx, dy);
            } else if (type == LIBINPUT_EVENT_POINTER_BUTTON) {
                struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
                ++buttons;
                enum libinput_button_state state = libinput_event_pointer_get_button_state(pointer);
                pressed += state == LIBINPUT_BUTTON_STATE_PRESSED;
                released += state == LIBINPUT_BUTTON_STATE_RELEASED;
                printf("LIBINPUT_BUTTON code=%u state=%u\n",
                    libinput_event_pointer_get_button(pointer),
                    libinput_event_pointer_get_button_state(pointer));
            }
            libinput_event_destroy(event);
        }
        fflush(stdout);
        if (motion && pressed && released && (!hotplug || restored)) break;
        if (poll(&pfd, 1, 1000) < 0) break;
    }
    libinput_unref(input);
    udev_unref(udev);
    int passed = pointers && motion && pressed && released && (!hotplug || restored);
    printf("LIBINPUT_%s devices=%u keyboards=%u pointers=%u motion=%u buttons=%u pressed=%u released=%u removed=%u restored=%u\n",
        passed ? "PASS" : "FAIL", devices, keyboards, pointers, motion, buttons, pressed, released,
        removed, restored);
    return passed ? 0 : 1;
}

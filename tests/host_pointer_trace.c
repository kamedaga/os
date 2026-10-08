/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>
#include <poll.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned long long now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) exit(1);
    return (unsigned long long)t.tv_sec * 1000000000 + t.tv_nsec;
}

static int focused(Display *d, Window target)
{
    Window w;
    int revert;
    XGetInputFocus(d, &w, &revert);
    for (unsigned depth = 0; w > PointerRoot && depth < 32; ++depth) {
        if (w == target) return 1;
        Window root, parent, *children = NULL;
        unsigned count;
        if (!XQueryTree(d, w, &root, &parent, &children, &count)) return 0;
        if (children) XFree(children);
        if (parent == w) return 0;
        w = parent;
    }
    return 0;
}

/* XI2 raw events do not inject, warp or grab the pointer. Record no keys and
 * retain mouse data only while this exact QEMU window has input focus. Host
 * raw valuators are not assumed to have USB mouse units or clock origin. */
int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    Window target = strtoul(argv[1], NULL, 0);
    unsigned long pid = strtoul(argv[2], NULL, 10);
    unsigned long seconds = strtoul(argv[3], NULL, 10);
    if (!target || !pid || !seconds || seconds > 600) return 2;
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    Atom actual;
    int format;
    unsigned long count, remaining;
    unsigned char *data = NULL;
    int status = XGetWindowProperty(d, target, XInternAtom(d, "_NET_WM_PID", False),
        0, 1, False, XA_CARDINAL, &actual, &format, &count, &remaining, &data);
    int matches = !status && actual == XA_CARDINAL && format == 32 &&
        count == 1 && data && *(unsigned long *)data == pid;
    if (data) XFree(data);
    if (!matches) { fprintf(stderr, "QEMU window/PID mismatch\n"); XCloseDisplay(d); return 1; }
    /* XI 2.0 suppresses another client's raw events during a grab; QEMU
     * capture must not make our observer silently stop seeing the mouse. */
    int opcode, event, error, major = 2, minor = 1;
    if (!XQueryExtension(d, "XInputExtension", &opcode, &event, &error) ||
        XIQueryVersion(d, &major, &minor) != Success ||
        major < 2 || (major == 2 && minor < 1)) {
        fprintf(stderr, "XI2 unavailable\n"); XCloseDisplay(d); return 1;
    }
    int device_count = 0;
    XIDeviceInfo *devices = XIQueryDevice(d, XIAllDevices, &device_count);
    for (int i = 0; devices && i < device_count; ++i) {
        XIDeviceInfo *device = &devices[i];
        for (int j = 0; j < device->num_classes; ++j) {
            if (device->classes[j]->type != XIValuatorClass) continue;
            XIValuatorClassInfo *axis = (XIValuatorClassInfo *)device->classes[j];
            char *label = axis->label ? XGetAtomName(d, axis->label) : NULL;
            printf("HOST_AXIS device=%d name=%s axis=%d mode=%s min=%.6f max=%.6f label=%s\n",
                device->deviceid, device->name, axis->number,
                axis->mode == XIModeAbsolute ? "absolute" : "relative",
                axis->min, axis->max, label ? label : "-");
            if (label) XFree(label);
        }
    }
    if (devices) XIFreeDeviceInfo(devices);
    unsigned char bits[XIMaskLen(XI_RawMotion)] = {0};
    XISetMask(bits, XI_RawMotion);
    XISetMask(bits, XI_RawButtonPress);
    XISetMask(bits, XI_RawButtonRelease);
    XIEventMask mask = {.deviceid = XIAllMasterDevices, .mask_len = sizeof(bits), .mask = bits};
    if (XISelectEvents(d, DefaultRootWindow(d), &mask, 1) != Success) return 1;
    XSync(d, False);
    printf("HOST_TRACE_READY window=0x%lx pid=%lu seconds=%lu xi=%d.%d focus_only=1 no_grab=1\n",
        target, pid, seconds, major, minor);
    fflush(stdout);
    unsigned long long until = now_ns() + seconds * 1000000000ULL, focus_at = 0;
    unsigned long recorded = 0;
    int active = 0;
    struct pollfd fd = {.fd = ConnectionNumber(d), .events = POLLIN};
    while (now_ns() < until) {
        if (now_ns() >= focus_at) {
            int next = focused(d, target);
            if (next != active) {
                printf("HOST_FOCUS receive_ns=%llu active=%d\n", now_ns(), next);
                fflush(stdout);
            }
            active = next;
            focus_at = now_ns() + 100000000ULL;
        }
        if (!XPending(d)) {
            if (poll(&fd, 1, 10) < 0 || (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
            continue;
        }
        XEvent e;
        XNextEvent(d, &e);
        if (e.type != GenericEvent || e.xcookie.extension != opcode ||
            !XGetEventData(d, &e.xcookie)) continue;
        if (active && (e.xcookie.evtype == XI_RawMotion ||
            e.xcookie.evtype == XI_RawButtonPress || e.xcookie.evtype == XI_RawButtonRelease)) {
            XIRawEvent *raw = e.xcookie.data;
            printf("HOST_RAW receive_ns=%llu time_ms=%lu type=%d device=%d source=%d detail=%d flags=%d",
                now_ns(), raw->time, raw->evtype, raw->deviceid, raw->sourceid, raw->detail, raw->flags);
            unsigned value = 0;
            for (int axis = 0; axis < raw->valuators.mask_len * 8; ++axis) {
                if (!XIMaskIsSet(raw->valuators.mask, axis)) continue;
                printf(" axis%d=%.6f/%.6f", axis, raw->raw_values[value], raw->valuators.values[value]);
                ++value;
            }
            putchar('\n');
            fflush(stdout);
            ++recorded;
        }
        XFreeEventData(d, &e.xcookie);
    }
    printf("HOST_TRACE_DONE records=%lu\n", recorded);
    XCloseDisplay(d);
    return 0;
}

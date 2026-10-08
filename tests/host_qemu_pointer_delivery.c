/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/record.h>
#include <X11/extensions/XI2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned long records;
static int xi_opcode;
static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) exit(1);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static uint16_t u16(const unsigned char *p) { uint16_t n; memcpy(&n, p, 2); return n; }
static uint32_t u32(const unsigned char *p) { uint32_t n; memcpy(&n, p, 4); return n; }
static double fixed(const unsigned char *p) { return (int32_t)u32(p) / 65536.0; }

/* Observe only the specified QEMU client's motion/button delivery and warp
 * requests. Never request input or grab/warp it ourselves. Generic events
 * outside XI motion/button are discarded, including every keyboard event. */
static void observe(XPointer closure, XRecordInterceptData *data)
{
    (void)closure;
    if (!data->client_swapped) {
        size_t bytes = data->data_len * 4;
        for (size_t at = 0; at + 4 <= bytes;) {
            const unsigned char *p = data->data + at;
            size_t size = data->category == XRecordFromClient ? (size_t)u16(p + 2) * 4 : 32;
            unsigned type = p[0] & 127;
            if (data->category == XRecordFromServer && type == GenericEvent && at + 8 <= bytes)
                size += (size_t)u32(p + 4) * 4;
            if (!size || size > bytes - at) break;
            if (data->category == XRecordFromServer && size >= 32 &&
                type >= ButtonPress && type <= MotionNotify) {
                printf("DELIVERY_CORE receive_ns=%llu server_ms=%u type=%u detail=%u root=%d,%d event=%d,%d window=0x%x\n",
                    (unsigned long long)now_ns(), u32(p + 4), type, p[1],
                    (int16_t)u16(p + 20), (int16_t)u16(p + 22),
                    (int16_t)u16(p + 24), (int16_t)u16(p + 26), u32(p + 12));
                ++records;
            } else if (data->category == XRecordFromServer && type == GenericEvent &&
                p[1] == xi_opcode && size >= 80 &&
                u16(p + 8) >= XI_ButtonPress && u16(p + 8) <= XI_Motion) {
                printf("DELIVERY_XI receive_ns=%llu server_ms=%u type=%u device=%u source=%u detail=%u root=%.6f,%.6f event=%.6f,%.6f window=0x%x\n",
                    (unsigned long long)now_ns(), u32(p + 12), u16(p + 8),
                    u16(p + 10), u16(p + 52), u32(p + 16), fixed(p + 32),
                    fixed(p + 36), fixed(p + 40), fixed(p + 44), u32(p + 24));
                ++records;
            } else if (data->category == XRecordFromClient) {
                printf("DELIVERY_WARP_REQUEST receive_ns=%llu major=%u minor=%u bytes=%zu",
                    (unsigned long long)now_ns(), p[0], p[1], size);
                if (p[0] == xi_opcode && p[1] == 41 && size >= 36)
                    printf(" source_window=0x%x destination_window=0x%x source=%.6f,%.6f source_size=%u,%u destination=%.6f,%.6f device=%u",
                        u32(p + 4), u32(p + 8), fixed(p + 12), fixed(p + 16),
                        u16(p + 20), u16(p + 22), fixed(p + 24), fixed(p + 28), u16(p + 32));
                else if (p[0] == 41 && size >= 24)
                    printf(" source_window=0x%x destination_window=0x%x source=%d,%d source_size=%u,%u destination=%d,%d",
                        u32(p + 4), u32(p + 8), (int16_t)u16(p + 12), (int16_t)u16(p + 14),
                        u16(p + 16), u16(p + 18), (int16_t)u16(p + 20), (int16_t)u16(p + 22));
                putchar('\n');
                ++records;
            }
            at += size;
        }
    }
    XRecordFreeData(data);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int self_test = argc == 2 && !strcmp(argv[1], "--self-test");
    if (argc != 4 && !self_test) return 2;
    Window window = self_test ? 1 : strtoul(argv[1], NULL, 0);
    unsigned long pid = self_test ? (unsigned long)getpid() : strtoul(argv[2], NULL, 10);
    unsigned long seconds = self_test ? 1 : strtoul(argv[3], NULL, 10);
    if (!window || !pid || !seconds || seconds > 600) return 2;
    Display *control = XOpenDisplay(NULL), *stream = XOpenDisplay(NULL);
    if (!control || !stream) return 1;
    if (self_test) {
        /* Deliver a synthetic event only to our own unmapped window: this
         * verifies the RECORD path without touching QEMU or user input. */
        window = XCreateSimpleWindow(control, DefaultRootWindow(control), 0, 0, 1, 1, 0, 0, 0);
        XChangeProperty(control, window, XInternAtom(control, "_NET_WM_PID", False),
            XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
        XSelectInput(control, window, PointerMotionMask);
    }
    Atom actual;
    int format, major, minor, event, error;
    unsigned long count, remaining;
    unsigned char *value = NULL;
    int status = XGetWindowProperty(control, window, XInternAtom(control, "_NET_WM_PID", False),
        0, 1, False, XA_CARDINAL, &actual, &format, &count, &remaining, &value);
    int matches = !status && actual == XA_CARDINAL && format == 32 && count == 1 &&
        value && *(unsigned long *)value == pid;
    if (value) XFree(value);
    if (!matches || !XRecordQueryVersion(control, &major, &minor) ||
        !XQueryExtension(control, "XInputExtension", &xi_opcode, &event, &error)) return 1;
    XRecordRange *ranges[3] = {XRecordAllocRange(), XRecordAllocRange(), XRecordAllocRange()};
    if (!ranges[0] || !ranges[1] || !ranges[2]) return 1;
    ranges[0]->delivered_events.first = ButtonPress;
    ranges[0]->delivered_events.last = MotionNotify;
    ranges[1]->delivered_events.first = GenericEvent;
    ranges[1]->delivered_events.last = GenericEvent;
    ranges[2]->core_requests.first = 41; /* X_WarpPointer */
    ranges[2]->core_requests.last = 41;
    ranges[2]->ext_requests.ext_major.first = xi_opcode;
    ranges[2]->ext_requests.ext_major.last = xi_opcode;
    ranges[2]->ext_requests.ext_minor.first = 41; /* X_XIWarpPointer */
    ranges[2]->ext_requests.ext_minor.last = 41;
    XRecordClientSpec client = window;
    XRecordContext context = XRecordCreateContext(control, 0, &client, 1, ranges, 3);
    for (unsigned i = 0; i < 3; ++i) XFree(ranges[i]);
    XSync(control, False);
    if (!context || !XRecordEnableContextAsync(stream, context, observe, NULL)) return 1;
    printf("DELIVERY_READY window=0x%lx pid=%lu seconds=%lu record=%d.%d no_grab=1\n",
        window, pid, seconds, major, minor);
    fflush(stdout);
    if (self_test) {
        XEvent e = {0};
        e.xmotion.type = MotionNotify;
        e.xmotion.display = control;
        e.xmotion.window = window;
        e.xmotion.root = DefaultRootWindow(control);
        e.xmotion.x = 17;
        e.xmotion.y = 23;
        XSendEvent(control, window, False, PointerMotionMask, &e);
        XSync(control, False);
    }
    struct pollfd fd = {.fd = ConnectionNumber(stream), .events = POLLIN};
    uint64_t until = now_ns() + seconds * UINT64_C(1000000000);
    while (now_ns() < until) {
        int ready = poll(&fd, 1, 50);
        if (ready < 0 || (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (ready) XRecordProcessReplies(stream);
    }
    XRecordDisableContext(control, context);
    XSync(control, False);
    XRecordProcessReplies(stream);
    XRecordFreeContext(control, context);
    if (self_test) XDestroyWindow(control, window);
    XCloseDisplay(stream);
    XCloseDisplay(control);
    printf("DELIVERY_DONE records=%lu\n", records);
    return self_test && !records ? 1 : 0;
}

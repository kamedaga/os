/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

enum { DEVICES = 16, SAMPLES = 320 };
struct sample { uint64_t us; int phase, x, y; };
struct totals { unsigned rel, saturated, dropped; int maximum; };
static struct sample samples[SAMPLES];
static struct totals totals[3];
static volatile sig_atomic_t stopped;
static void stop(int signal) { (void)signal; stopped = 1; }
static uint64_t now_us(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* Hide only the cursor, never grab or inject input. Keep the upload below the
 * suspect partial-send boundary and buffer samples so logging cannot stall
 * capture. Closing the X connection also releases this client's hide count. */
int main(void)
{
    Display *d = XOpenDisplay(NULL);
    int event, error, major = 4, minor = 0;
    if (!d || !XFixesQueryExtension(d, &event, &error) ||
        !XFixesQueryVersion(d, &major, &minor) || major < 4) {
        fprintf(stderr, "FEEDBACK_ERROR XFixes 4 required\n");
        if (d) XCloseDisplay(d);
        return 1;
    }
    struct pollfd fds[DEVICES];
    unsigned devices = 0;
    for (unsigned node = 0; node < 64 && devices < DEVICES; ++node) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%u", node);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        uint64_t rel = 0;
        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), &rel) < 0 || (rel & 3) != 3) {
            close(fd);
            continue;
        }
        fds[devices++] = (struct pollfd){.fd = fd, .events = POLLIN};
    }
    if (!devices) { XCloseDisplay(d); return 1; }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    uint64_t start = now_us(), next = start, phase_at[3] = {start, 0, 0};
    unsigned used = 0;
    int phase = 0, hidden = 0, status = 0;
    while (!stopped) {
        uint64_t t = now_us(), elapsed = t - start;
        if (elapsed >= 10000000) break;
        int next_phase = elapsed < 3000000 ? 0 : elapsed < 7000000 ? 1 : 2;
        if (next_phase != phase) {
            if (next_phase == 1 && !hidden) XFixesHideCursor(d, DefaultRootWindow(d));
            else if (next_phase != 1 && hidden) XFixesShowCursor(d, DefaultRootWindow(d));
            hidden = next_phase == 1;
            XSync(d, False);
            phase = next_phase;
            phase_at[phase] = now_us();
        }
        int ready = poll(fds, devices, 10);
        if (ready < 0) { if (errno == EINTR) continue; status = 1; break; }
        for (unsigned i = 0; i < devices; ++i) {
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { status = 1; stopped = 1; break; }
            if (!(fds[i].revents & POLLIN)) continue;
            struct input_event input[64];
            ssize_t bytes = read(fds[i].fd, input, sizeof(input));
            if (bytes < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (bytes <= 0 || bytes % sizeof(input[0])) { status = 1; stopped = 1; break; }
            for (size_t j = 0; j < (size_t)bytes / sizeof(input[0]); ++j) {
                if (input[j].type == EV_SYN && input[j].code == SYN_DROPPED) ++totals[phase].dropped;
                if (input[j].type != EV_REL || input[j].code > REL_Y) continue;
                int value = input[j].value;
                int64_t magnitude = value < 0 ? -(int64_t)value : value;
                ++totals[phase].rel;
                if (value == 127 || value == -127 || value == -128) ++totals[phase].saturated;
                if (magnitude > totals[phase].maximum)
                    totals[phase].maximum = magnitude > INT32_MAX ? INT32_MAX : (int)magnitude;
            }
        }
        if (now_us() >= next && used < SAMPLES) {
            Window root, child;
            int x, y, wx, wy;
            unsigned mask;
            if (XQueryPointer(d, DefaultRootWindow(d), &root, &child, &x, &y, &wx, &wy, &mask))
                samples[used++] = (struct sample){now_us(), phase, x, y};
            next = now_us() + 40000;
        }
    }
    if (hidden) XFixesShowCursor(d, DefaultRootWindow(d));
    XSync(d, False);
    XCloseDisplay(d);
    for (unsigned i = 0; i < devices; ++i) close(fds[i].fd);
    printf("FEEDBACK_BEGIN start_us=%llu devices=%u no_grab=1\n", (unsigned long long)start, devices);
    for (unsigned i = 0; i < 3; ++i)
        printf("FEEDBACK_PHASE phase=%u hidden=%u begin_us=%llu rel=%u saturated=%u max_abs=%d dropped=%u\n",
            i, i == 1, (unsigned long long)phase_at[i], totals[i].rel,
            totals[i].saturated, totals[i].maximum, totals[i].dropped);
    for (unsigned i = 0; i < used; ++i)
        printf("FEEDBACK_X us=%llu phase=%d x=%d y=%d\n",
            (unsigned long long)samples[i].us, samples[i].phase, samples[i].x, samples[i].y);
    printf("FEEDBACK_DONE samples=%u status=%d interrupted=%d restored=1\n", used, status, !!stopped);
    return status || stopped ? 1 : 0;
}

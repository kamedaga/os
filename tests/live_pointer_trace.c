/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { MAX_DEVICES = 16, MAX_RECORDS = 65536 };
struct record {
    uint64_t received, captured;
    int node, type, code, value;
};
static struct record records[MAX_RECORDS];
static unsigned used, overflow;

static uint64_t now_us(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock"); exit(1); }
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void save(uint64_t received, uint64_t captured, int node,
    int type, int code, int value)
{
    if (used == MAX_RECORDS) { ++overflow; return; }
    records[used++] = (struct record){received, captured, node, type, code, value};
}

/* No EVIOCGRAB or event injection. Buffer during capture so per-event logging
 * cannot create the delivery stalls this diagnostic is meant to distinguish.
 * XQueryPointer is sampled independently; it is not a per-frame completion. */
int main(int argc, char **argv)
{
    char *end = NULL;
    unsigned long seconds = argc == 2 ? strtoul(argv[1], &end, 10) : 0;
    if (argc != 2 || !end || *end || seconds < 1 || seconds > 30) return 2;
    struct pollfd fds[MAX_DEVICES];
    unsigned nodes[MAX_DEVICES], devices = 0;
    for (unsigned node = 0; node < 64 && devices < MAX_DEVICES; ++node) {
        char path[64], name[128] = {0}, phys[128] = {0};
        snprintf(path, sizeof(path), "/dev/input/event%u", node);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        uint64_t rel = 0, abs = 0, ev = 0;
        struct input_id id = {0};
        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), &rel) < 0 ||
            (rel & 3) != 3) { close(fd); continue; }
        int clock = CLOCK_MONOTONIC;
        if (ioctl(fd, EVIOCSCLOCKID, &clock)) { perror("clock-id"); close(fd); return 1; }
        (void)ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        (void)ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys);
        (void)ioctl(fd, EVIOCGID, &id);
        (void)ioctl(fd, EVIOCGBIT(0, sizeof(ev)), &ev);
        (void)ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), &abs);
        printf("TRACE_DEVICE node=%u name=%s phys=%s id=%x:%x:%x:%x ev=%llx rel=%llx abs=%llx\n",
            node, name, phys, id.bustype, id.vendor, id.product, id.version,
            (unsigned long long)ev, (unsigned long long)rel, (unsigned long long)abs);
        fds[devices] = (struct pollfd){.fd = fd, .events = POLLIN};
        nodes[devices++] = node;
    }
    Display *display = XOpenDisplay(NULL);
    printf("TRACE_READY seconds=%lu devices=%u xorg=%d no_grab=1\n", seconds, devices, !!display);
    fflush(stdout);
    if (!devices) { if (display) XCloseDisplay(display); return 1; }
    uint64_t start = now_us(), until = start + seconds * 1000000, next_x = start;
    int old_x = -1, old_y = -1, status = 0;
    while (now_us() < until) {
        int ready = poll(fds, devices, 10);
        if (ready < 0) { if (errno == EINTR) continue; perror("poll"); status = 1; break; }
        for (unsigned i = 0; i < devices; ++i) {
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                save(now_us(), 0, nodes[i], -2, 0, fds[i].revents);
                fds[i].fd = -1; status = 1; continue;
            }
            if (!(fds[i].revents & POLLIN)) continue;
            /* Bound each drain so high-rate input cannot starve X sampling
             * or the deadline; the next poll will resume unread records. */
            for (unsigned batch = 0; batch < 8 && now_us() < until; ++batch) {
                struct input_event events[64];
                ssize_t bytes = read(fds[i].fd, events, sizeof(events));
                if (bytes < 0 && errno == EINTR) continue;
                if (bytes < 0 && errno == EAGAIN) break;
                if (bytes <= 0 || bytes % sizeof(events[0])) {
                    save(now_us(), 0, nodes[i], -2, 1, bytes < 0 ? errno : 0);
                    fds[i].fd = -1; status = 1; break;
                }
                uint64_t received = now_us();
                for (size_t j = 0; j < (size_t)bytes / sizeof(events[0]); ++j) {
                    const struct input_event *e = &events[j];
                    save(received, (uint64_t)e->time.tv_sec * 1000000 + e->time.tv_usec,
                        nodes[i], e->type, e->code, e->value);
                }
            }
        }
        uint64_t sampled = now_us();
        if (display && sampled >= next_x) {
            Window root, child;
            int x, y, wx, wy;
            unsigned mask;
            if (XQueryPointer(display, DefaultRootWindow(display), &root, &child,
                    &x, &y, &wx, &wy, &mask) && (x != old_x || y != old_y)) {
                save(now_us(), 0, -1, -1, x, y);
                old_x = x; old_y = y;
            }
            next_x = now_us() + 10000;
        }
    }
    uint64_t elapsed = now_us() - start;
    for (unsigned i = 0; i < used; ++i) {
        const struct record *r = &records[i];
        if (r->type == -1)
            printf("TRACE_X receive_us=%llu x=%d y=%d\n",
                (unsigned long long)r->received, r->code, r->value);
        else
            printf("TRACE_EV receive_us=%llu capture_us=%llu node=%d type=%d code=%d value=%d\n",
                (unsigned long long)r->received, (unsigned long long)r->captured,
                r->node, r->type, r->code, r->value);
    }
    printf("TRACE_DONE elapsed_us=%llu records=%u overflow=%u status=%d\n",
        (unsigned long long)elapsed, used, overflow, status);
    for (unsigned i = 0; i < devices; ++i) if (fds[i].fd >= 0) close(fds[i].fd);
    if (display) XCloseDisplay(display);
    return status || overflow ? 1 : 0;
}

/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>

static uint64_t now_us(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock"); exit(1); }
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static unsigned clients(Display *d, Window root, Atom atom, Window *out) {
    Atom type; int format; unsigned long count, remaining;
    unsigned char *data = NULL;
    if (XGetWindowProperty(d, root, atom, 0, 256, False, XA_WINDOW,
            &type, &format, &count, &remaining, &data) != Success ||
        type != XA_WINDOW || format != 32 || count > 256 || remaining) {
        if (data) XFree(data);
        return 0;
    }
    memcpy(out, data, count * sizeof(Window));
    if (data) XFree(data);
    return (unsigned)count;
}
static int ignore_gone_window(Display *d, XErrorEvent *e) {
    (void)d;
    /* A client may disappear between the WM list and attribute query. */
    if (e->error_code != BadWindow) {
        fprintf(stderr, "X11 error=%u request=%u\n", e->error_code, e->request_code);
    }
    return 0;
}
static int compare_us(const void *left, const void *right) {
    const uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}
int main(int argc, char **argv) {
    int listing = argc == 2 && !strcmp(argv[1], "--list");
    int watching = argc == 3 && !strcmp(argv[1], "--watch");
    int snapshot = argc == 3 && !strcmp(argv[1], "--snapshot");
    int panel = argc == 5 && !strcmp(argv[1], "--panel");
    int roundtrip = argc == 2 && !strcmp(argv[1], "--roundtrip");
    const char *wanted = watching || panel ? argv[2] : argc > 1 ? argv[1] : "";
    if (!listing && !watching && !snapshot && !panel && !roundtrip && (argc < 4 || strcmp(argv[2], "--"))) {
        fprintf(stderr, "usage: %s --list | --roundtrip | --snapshot FILE.ppm | --watch WM_CLASS | --panel WM_CLASS X Y | WM_CLASS -- PROGRAM [ARG...]\n", argv[0]); return 2;
    }
    Display *d = XOpenDisplay(NULL);
    if (!d) { fputs("No X display\n", stderr); return 1; }
    XSetErrorHandler(ignore_gone_window);
    if (roundtrip) {
        uint64_t samples[100], sum = 0;
        /* A NoOperation + server reply isolates X/IPC responsiveness from
         * shader execution. Sleep between samples so this observer does not
         * become a tight loop that competes with the application being timed. */
        for (unsigned i = 0; i < 100; i++) {
            const uint64_t start = now_us();
            XNoOp(d);
            XSync(d, False);
            samples[i] = now_us() - start;
            sum += samples[i];
            struct timespec pause = {.tv_nsec = 20000000};
            while (nanosleep(&pause, &pause) && errno == EINTR) {}
        }
        qsort(samples, 100, sizeof(samples[0]), compare_us);
        printf("X_ROUNDTRIP count=100 mean_us=%llu p50_us=%llu p95_us=%llu max_us=%llu\n",
            (unsigned long long)(sum / 100), (unsigned long long)samples[49],
            (unsigned long long)samples[94], (unsigned long long)samples[99]);
        XCloseDisplay(d);
        return 0;
    }
    Window root = DefaultRootWindow(d), before[256];
    if (snapshot) {
        /* GL scanout has no QMP CPU surface. Read the X screen once, outside
         * launch timings; this deliberately incurs GPU readback overhead. */
        XWindowAttributes attr;
        if (!XGetWindowAttributes(d, root, &attr)) return 1;
        XImage *image = XGetImage(d, root, 0, 0, (unsigned)attr.width,
            (unsigned)attr.height, AllPlanes, ZPixmap);
        FILE *output = image ? fopen(argv[2], "wb") : NULL;
        if (!output) { perror("snapshot"); return 1; }
        fprintf(output, "P6\n%d %d\n255\n", attr.width, attr.height);
        for (int y = 0; y < attr.height; y++) for (int x = 0; x < attr.width; x++) {
            unsigned long pixel = XGetPixel(image, x, y);
            const unsigned long masks[] = {image->red_mask, image->green_mask, image->blue_mask};
            for (unsigned k = 0; k < 3; k++) {
                unsigned long mask = masks[k];
                unsigned shift = 0;
                while (mask && !(mask & 1)) { shift++; mask >>= 1; }
                fputc(mask ? (int)(((pixel >> shift) & mask) * 255 / mask) : 0, output);
            }
        }
        int result = fclose(output) != 0;
        XDestroyImage(image);
        XCloseDisplay(d);
        return result;
    }
    Atom list = XInternAtom(d, "_NET_CLIENT_LIST", False);
    unsigned original = clients(d, root, list, before);
    if (listing) {
        for (unsigned i = 0; i < original; ++i) {
            XClassHint hint = {0}; char *name = NULL;
            (void)XGetClassHint(d, before[i], &hint);
            (void)XFetchName(d, before[i], &name);
            XWindowAttributes attr = {0};
            int x = 0, y = 0;
            Window child;
            (void)XGetWindowAttributes(d, before[i], &attr);
            (void)XTranslateCoordinates(d, before[i], root, 0, 0, &x, &y, &child);
            printf("APP_CLIENT window=0x%lx class=%s name=%s x=%d y=%d width=%d height=%d\n", before[i],
                hint.res_class ? hint.res_class : "", name ? name : "", x, y, attr.width, attr.height);
            if (hint.res_name) XFree(hint.res_name);
            if (hint.res_class) XFree(hint.res_class);
            if (name) XFree(name);
        }
        XCloseDisplay(d); return 0;
    }
    XSelectInput(d, root, PropertyChangeMask | SubstructureNotifyMask);
    XSync(d, False);
    uint64_t panel_start = 0;
    if (panel) {
        /* Invoke the real launcher in the existing panel. Forking the app
         * from this small observer omits the panel's spawn cost and context. */
        int x, y;
        XWindowAttributes attr;
        char extra;
        if (sscanf(argv[3], "%d%c", &x, &extra) != 1 ||
            sscanf(argv[4], "%d%c", &y, &extra) != 1 ||
            !XGetWindowAttributes(d, root, &attr) ||
            x < 0 || y < 0 || x >= attr.width || y >= attr.height) return 2;
        void *library = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
        int (*motion)(Display *, int, int, int, unsigned long) = library ?
            dlsym(library, "XTestFakeMotionEvent") : NULL;
        int (*button)(Display *, unsigned, Bool, unsigned long) = library ?
            dlsym(library, "XTestFakeButtonEvent") : NULL;
        if (!motion || !button || !motion(d, DefaultScreen(d), x, y, 0)) {
            fputs("XTEST pointer injection unavailable\n", stderr); return 1;
        }
        XSync(d, False);
        /* Let an unchanged auto-hiding panel reveal itself before clicking.
         * Pointer positioning is setup, outside the click-to-map interval;
         * disabling autohide here would alter the desktop under test. */
        struct timespec reveal = {.tv_nsec = 500000000};
        while (nanosleep(&reveal, &reveal) && errno == EINTR) {}
        panel_start = now_us();
        if (!button(d, 1, True, 0) || !button(d, 1, False, 0)) return 1;
        XFlush(d);
        /* Reuse the watch loop, anchoring its interval at the click rather
         * than including time spent positioning the pointer. */
        watching = 1;
        printf("APP_PANEL_CLICK class=%s monotonic_us=%llu x=%d y=%d\n", wanted,
            (unsigned long long)panel_start, x, y);
        fflush(stdout);
    }
    int errors[2] = {-1, -1};
    if (!watching && pipe2(errors, O_CLOEXEC | O_NONBLOCK)) { perror("pipe"); return 1; }
    uint64_t start = panel_start ? panel_start : now_us(), exec_us = 0;
    /* Observe the real panel process chain without replacing it by a fork
     * from this much smaller Xlib observer. Host timestamps anchor the click. */
    pid_t child = watching ? 0 : fork();
    if (child < 0) { perror("fork"); return 1; }
    if (!watching && !child) {
        close(errors[0]); close(ConnectionNumber(d));
        execvp(argv[3], argv + 3);
        int error = errno;
        (void)write(errors[1], &error, sizeof(error));
        _exit(127);
    }
    if (errors[1] >= 0) close(errors[1]);
    printf("%s class=%s pid=%ld\n", watching ? "APP_WATCH_READY" : "APP_START",
        wanted, (long)child); fflush(stdout);
    int result = 1;
    for (;;) {
        while (XPending(d)) { XEvent e; XNextEvent(d, &e); }
        int error = 0;
        ssize_t n = watching ? -1 : read(errors[0], &error, sizeof(error));
        if (n > 0) { fprintf(stderr, "APP_EXEC_ERROR errno=%d\n", error); break; }
        if (!n && !exec_us) exec_us = now_us() - start;
        Window current[256];
        unsigned count = clients(d, root, list, current);
        for (unsigned i = 0; i < count; ++i) {
            unsigned j;
            for (j = 0; j < original && before[j] != current[i]; ++j) { }
            if (j != original) continue;
            XClassHint hint = {0};
            int matches = XGetClassHint(d, current[i], &hint) &&
                hint.res_class && !strcmp(hint.res_class, wanted);
            if (hint.res_name) XFree(hint.res_name);
            if (hint.res_class) XFree(hint.res_class);
            XWindowAttributes attr;
            if (!matches || !XGetWindowAttributes(d, current[i], &attr) ||
                attr.map_state != IsViewable) continue;
            printf("APP_VIEWABLE class=%s exec_observed_us=%llu start_to_map_us=%llu window=0x%lx\n",
                wanted, (unsigned long long)exec_us,
                (unsigned long long)(now_us() - start), current[i]);
            /* Leave the measured application visible. This tool must not
             * change the lifetime of the user's desktop or its applications. */
            result = 0; goto done;
        }
        if (now_us() - start >= 30000000) { puts("APP_MAP_TIMEOUT seconds=30"); break; }
        struct pollfd events[2] = {
            {.fd = ConnectionNumber(d), .events = POLLIN},
            {.fd = exec_us ? -1 : errors[0], .events = POLLIN},
        };
        if (poll(events, 2, 20) < 0 && errno != EINTR) { perror("poll"); break; }
    }
done:
    if (errors[0] >= 0) close(errors[0]);
    XCloseDisplay(d);
    if (!watching) (void)waitpid(child, NULL, WNOHANG);
    return result;
}

#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) { perror("clock_gettime"); exit(2); }
    return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

// Windows can disappear between XQueryTree and XGetWindowAttributes. The
// probe treats that normal desktop race as a missed sample and retries.
static int ignore_x_error(Display *display, XErrorEvent *event) {
    (void)display;
    (void)event;
    return 0;
}

static void pause_ms(long milliseconds) {
    struct timespec value = { .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L };
    while (nanosleep(&value, &value) != 0 && errno == EINTR) { }
}

static int contains(const char *text, const char *needle) {
    return text && strstr(text, needle) != NULL;
}

static int window_matches(Display *display, Window window, const char *needle) {
    char *name = NULL;
    XClassHint hint = {0};
    int match = 0;
    if (XFetchName(display, window, &name) && contains(name, needle)) match = 1;
    if (!match && XGetClassHint(display, window, &hint)) {
        match = contains(hint.res_name, needle) || contains(hint.res_class, needle);
    }
    if (name) XFree(name);
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
    return match;
}

static Window find_window(Display *display, Window parent, const char *needle) {
    Window root, ignored_parent, *children = NULL;
    unsigned count = 0;
    if (window_matches(display, parent, needle)) return parent;
    if (!XQueryTree(display, parent, &root, &ignored_parent, &children, &count)) return None;
    Window result = None;
    for (unsigned index = 0; index < count && result == None; index++) {
        XWindowAttributes attributes;
        if (XGetWindowAttributes(display, children[index], &attributes) &&
                attributes.map_state == IsViewable) {
            result = find_window(display, children[index], needle);
        }
    }
    if (children) XFree(children);
    return result;
}

static Window wait_window(Display *display, const char *needle, long timeout_ms) {
    uint64_t deadline = monotonic_ns() + (uint64_t)timeout_ms * 1000000ull;
    Window root = DefaultRootWindow(display);
    do {
        Window result = find_window(display, root, needle);
        if (result != None) return result;
        pause_ms(10);
    } while (monotonic_ns() < deadline);
    return None;
}

static void list_windows(Display *display, Window parent, int depth) {
    Window root, ignored_parent, *children = NULL;
    unsigned count = 0;
    if (!XQueryTree(display, parent, &root, &ignored_parent, &children, &count)) return;
    for (unsigned index = 0; index < count; index++) {
        XWindowAttributes attributes;
        char *name = NULL;
        XClassHint hint = {0};
        XGetWindowAttributes(display, children[index], &attributes);
        XFetchName(display, children[index], &name);
        XGetClassHint(display, children[index], &hint);
        if (attributes.map_state == IsViewable) {
            printf("0x%lx\t%d\t%d,%d %dx%d\t%s\t%s\t%s\n", children[index], depth,
                    attributes.x, attributes.y, attributes.width, attributes.height,
                    name ? name : "", hint.res_name ? hint.res_name : "",
                    hint.res_class ? hint.res_class : "");
        }
        if (name) XFree(name);
        if (hint.res_name) XFree(hint.res_name);
        if (hint.res_class) XFree(hint.res_class);
        list_windows(display, children[index], depth + 1);
    }
    if (children) XFree(children);
}

static pid_t launch(char **arguments) {
    pid_t child = fork();
    if (child < 0) { perror("fork"); exit(2); }
    if (child == 0) {
        execvp(arguments[0], arguments);
        perror("execvp");
        _exit(127);
    }
    return child;
}

static int screenshot(Display *display, Window window, const char *path) {
    XWindowAttributes attributes;
    if (!XGetWindowAttributes(display, window, &attributes)) return 0;
    XImage *image = XGetImage(display, window, 0, 0, attributes.width, attributes.height,
            AllPlanes, ZPixmap);
    if (!image) return 0;
    FILE *output = fopen(path, "wb");
    if (!output) { perror(path); XDestroyImage(image); return 0; }
    fprintf(output, "P6\n%d %d\n255\n", attributes.width, attributes.height);
    for (int y = 0; y < attributes.height; y++) {
        for (int x = 0; x < attributes.width; x++) {
            unsigned long pixel = XGetPixel(image, x, y);
            unsigned char rgb[3] = {
                (unsigned char)((pixel & image->red_mask) * 255 / image->red_mask),
                (unsigned char)(((pixel & image->green_mask) * 255 / image->green_mask)),
                (unsigned char)((pixel & image->blue_mask) * 255 / image->blue_mask),
            };
            fwrite(rgb, sizeof(rgb), 1, output);
        }
    }
    fclose(output);
    XDestroyImage(image);
    return 1;
}

static void usage(const char *name) {
    fprintf(stderr, "usage: %s now|windows|wait NAME MS|focus NAME|key KEYSYM [down|up]|"
            "shot NAME FILE|run-wait NAME MS -- CMD...|run-wait-file FILE MS -- CMD...\n", name);
    exit(2);
}

int main(int argc, char **argv) {
    if (argc < 2) usage(argv[0]);
    if (!strcmp(argv[1], "now")) { printf("%llu\n", (unsigned long long)monotonic_ns()); return 0; }
    Display *display = XOpenDisplay(NULL);
    if (!display) { fprintf(stderr, "cannot open DISPLAY\n"); return 2; }
    XSetErrorHandler(ignore_x_error);
    if (!strcmp(argv[1], "windows")) {
        list_windows(display, DefaultRootWindow(display), 0);
    } else if (!strcmp(argv[1], "wait") && argc == 4) {
        uint64_t start = monotonic_ns();
        Window window = wait_window(display, argv[2], atol(argv[3]));
        if (window == None) return 1;
        printf("window=0x%lx wait_ms=%.3f\n", window, (monotonic_ns() - start) / 1e6);
    } else if (!strcmp(argv[1], "focus") && argc == 3) {
        Window window = wait_window(display, argv[2], 1000);
        if (window == None) return 1;
        XRaiseWindow(display, window);
        XSetInputFocus(display, window, RevertToParent, CurrentTime);
        XFlush(display);
    } else if (!strcmp(argv[1], "key") && (argc == 3 || argc == 4)) {
        KeySym symbol = XStringToKeysym(argv[2]);
        KeyCode code = XKeysymToKeycode(display, symbol);
        if (symbol == NoSymbol || code == 0) return 2;
        if (argc == 3 || !strcmp(argv[3], "down")) XTestFakeKeyEvent(display, code, True, CurrentTime);
        if (argc == 3 || !strcmp(argv[3], "up")) XTestFakeKeyEvent(display, code, False, CurrentTime);
        XFlush(display);
    } else if (!strcmp(argv[1], "shot") && argc == 4) {
        Window window = wait_window(display, argv[2], 1000);
        if (window == None || !screenshot(display, window, argv[3])) return 1;
    } else if (!strcmp(argv[1], "run-wait") && argc >= 7 && !strcmp(argv[4], "--")) {
        uint64_t start = monotonic_ns();
        pid_t child = launch(&argv[5]);
        Window window = wait_window(display, argv[2], atol(argv[3]));
        if (window == None) { kill(child, 15); return 1; }
        printf("child=%ld window=0x%lx start_to_map_ms=%.3f\n", (long)child, window,
                (monotonic_ns() - start) / 1e6);
    } else if (!strcmp(argv[1], "run-wait-file") && argc >= 7 && !strcmp(argv[4], "--")) {
        unlink(argv[2]);
        uint64_t start = monotonic_ns();
        pid_t child = launch(&argv[5]);
        uint64_t deadline = start + (uint64_t)atol(argv[3]) * 1000000ull;
        struct stat status;
        while (stat(argv[2], &status) != 0 && monotonic_ns() < deadline) pause_ms(10);
        if (stat(argv[2], &status) != 0) { kill(child, 15); return 1; }
        printf("child=%ld start_to_file_ms=%.3f\n", (long)child, (monotonic_ns() - start) / 1e6);
    } else {
        usage(argv[0]);
    }
    XCloseDisplay(display);
    return 0;
}

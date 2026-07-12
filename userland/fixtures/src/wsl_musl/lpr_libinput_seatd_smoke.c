#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <libseat.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct seat_state {
    struct libseat *seat;
    int enabled;
    struct { int fd; int device_id; } devices[16];
};

static void seat_enable(struct libseat *seat, void *data) { (void)seat; ((struct seat_state *)data)->enabled = 1; }
static void seat_disable(struct libseat *seat, void *data) {
    ((struct seat_state *)data)->enabled = 0;
    (void)libseat_disable_seat(seat);
}
static const struct libseat_seat_listener seat_listener = {
    .enable_seat = seat_enable,
    .disable_seat = seat_disable,
};

static int open_restricted(const char *path, int flags, void *data)
{
    struct seat_state *state = data;
    if (state->seat == NULL) return open(path, flags);
    int fd = -1;
    int id = libseat_open_device(state->seat, path, &fd);
    if (id < 0) return -1;
    for (size_t i = 0; i < sizeof(state->devices) / sizeof(state->devices[0]); i++) {
        if (state->devices[i].fd == 0) {
            state->devices[i].fd = fd;
            state->devices[i].device_id = id;
            break;
        }
    }
    return fd;
}

static void close_restricted(int fd, void *data)
{
    struct seat_state *state = data;
    if (state->seat == NULL) { close(fd); return; }
    for (size_t i = 0; i < sizeof(state->devices) / sizeof(state->devices[0]); i++) {
        if (state->devices[i].fd == fd) {
            (void)libseat_close_device(state->seat, state->devices[i].device_id);
            state->devices[i].fd = 0;
            state->devices[i].device_id = 0;
            return;
        }
    }
}

static const struct libinput_interface interface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

static int handle_events(struct libinput *li, int *added, int *key_down, int *key_up,
    int *motion, int *button_down, int *button_up)
{
    if (libinput_dispatch(li) != 0) return -1;
    struct libinput_event *event;
    while ((event = libinput_get_event(li)) != NULL) {
        enum libinput_event_type type = libinput_event_get_type(event);
        if (type == LIBINPUT_EVENT_DEVICE_ADDED) {
            struct libinput_device *device = libinput_event_get_device(event);
            printf("LIBINPUT_DEVICE_ADDED name=%s sysname=%s\n",
                libinput_device_get_name(device), libinput_device_get_sysname(device));
            (*added)++;
        } else if (type == LIBINPUT_EVENT_KEYBOARD_KEY) {
            struct libinput_event_keyboard *key = libinput_event_get_keyboard_event(event);
            uint32_t code = libinput_event_keyboard_get_key(key);
            enum libinput_key_state state = libinput_event_keyboard_get_key_state(key);
            printf("LIBINPUT_KEY code=%u state=%u\n", code, state);
            if (code == 30 && state == LIBINPUT_KEY_STATE_PRESSED) *key_down = 1;
            if (code == 30 && state == LIBINPUT_KEY_STATE_RELEASED) *key_up = 1;
        } else if (type == LIBINPUT_EVENT_POINTER_MOTION) {
            struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
            double dx = libinput_event_pointer_get_dx_unaccelerated(pointer);
            double dy = libinput_event_pointer_get_dy_unaccelerated(pointer);
            printf("LIBINPUT_MOTION dx=%.1f dy=%.1f\n", dx, dy);
            if (fabs(dx - 7.0) < 0.1 || fabs(dy + 4.0) < 0.1) *motion = 1;
        } else if (type == LIBINPUT_EVENT_POINTER_BUTTON) {
            struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
            uint32_t code = libinput_event_pointer_get_button(pointer);
            enum libinput_button_state state = libinput_event_pointer_get_button_state(pointer);
            printf("LIBINPUT_BUTTON code=%u state=%u\n", code, state);
            if (code == 272 && state == LIBINPUT_BUTTON_STATE_PRESSED) *button_down = 1;
            if (code == 272 && state == LIBINPUT_BUTTON_STATE_RELEASED) *button_up = 1;
        }
        libinput_event_destroy(event);
    }
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    const int direct_path = argc == 2 && strcmp(argv[1], "--path") == 0;
    struct seat_state state = {0};
    if (!direct_path) {
        (void)mkdir("/run", 0755);
        (void)unlink("/run/seatd.sock");
        (void)setenv("LIBSEAT_BACKEND", "seatd", 1);
        (void)setenv("SEATD_SOCK", "/run/seatd.sock", 1);
        (void)setenv("SEATD_VTBOUND", "0", 1);
        const pid_t seatd_pid = fork();
        if (seatd_pid < 0) { perror("fork seatd"); return 1; }
        if (seatd_pid == 0) {
            execl("/usr/bin/seatd", "seatd", "-l", "debug", (char *)NULL);
            _exit(127);
        }
        const struct timespec delay = { .tv_sec = 1, .tv_nsec = 0 };
        (void)nanosleep(&delay, NULL);
        state.seat = libseat_open_seat(&seat_listener, &state);
        if (state.seat == NULL) { perror("libseat_open_seat"); return 1; }
        for (int i = 0; i < 100 && !state.enabled; i++) (void)libseat_dispatch(state.seat, 10);
        if (!state.enabled) { fprintf(stderr, "LIBSEAT_ENABLE_FAIL\n"); return 1; }
        printf("LIBSEAT_READY name=%s\n", libseat_seat_name(state.seat));
    }

    struct libinput *li = libinput_path_create_context(&interface, &state);
    if (li == NULL) { fprintf(stderr, "LIBINPUT_CONTEXT_FAIL\n"); return 1; }
    libinput_log_set_priority(li, LIBINPUT_LOG_PRIORITY_DEBUG);
    if (libinput_path_add_device(li, "/dev/input/event0") == NULL ||
        libinput_path_add_device(li, "/dev/input/event1") == NULL) {
        fprintf(stderr, "LIBINPUT_PATH_ADD_FAIL\n"); return 1;
    }

    int added = 0, key_down = 0, key_up = 0, motion = 0, button_down = 0, button_up = 0;
    for (int i = 0; i < 100 && added < 2; i++) {
        if (handle_events(li, &added, &key_down, &key_up, &motion, &button_down, &button_up) != 0) return 1;
        usleep(10000);
    }
    if (added < 2) { fprintf(stderr, "LIBINPUT_ENUM_FAIL added=%d\n", added); return 1; }
    printf("LIBINPUT_READY backend=%s devices=%d\n", direct_path ? "path" : "path-seatd", added);
    fflush(stdout);
    for (int i = 0; i < 400 && !(key_down && key_up && motion && button_down && button_up); i++) {
        struct pollfd fds[2] = {{.fd = libinput_get_fd(li), .events = POLLIN}};
        nfds_t count = 1;
        if (state.seat != NULL) { fds[1].fd = libseat_get_fd(state.seat); fds[1].events = POLLIN; count = 2; }
        (void)poll(fds, count, 25);
        if (state.seat != NULL && (fds[1].revents & POLLIN)) (void)libseat_dispatch(state.seat, 0);
        if (handle_events(li, &added, &key_down, &key_up, &motion, &button_down, &button_up) != 0) return 1;
    }
    libinput_unref(li);
    if (state.seat != NULL) libseat_close_seat(state.seat);
    if (!(key_down && key_up && motion && button_down && button_up)) {
        fprintf(stderr, "LIBINPUT_EVENT_FAIL key=%d/%d motion=%d button=%d/%d\n",
            key_down, key_up, motion, button_down, button_up);
        return 1;
    }
    printf("LIBINPUT_EVENT_PASS key=30 motion=7,-4 button=272 seat=%s\n",
        direct_path ? "direct" : "seat0");
    return 0;
}

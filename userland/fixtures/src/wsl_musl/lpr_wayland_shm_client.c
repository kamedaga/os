#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;
static struct wl_surface *client_surface;
static int configured;
static int frame_done;
static int input_mode;
static int surface_ready;
static int keymap_ready;
static int keyboard_focus;
static int pointer_focus;
static int input_ready;
static int input_finished;
static int key_down;
static int key_up;
static int button_down;
static int button_up;
static wl_fixed_t pointer_x;
static wl_fixed_t pointer_y;
static wl_fixed_t pointer_dx;
static wl_fixed_t pointer_dy;
static char seat_name[32] = "unknown";

static void maybe_input_ready(void) {
    if (!input_mode || input_ready || !surface_ready || !keymap_ready ||
        !keyboard_focus || !pointer_focus) return;
    input_ready = 1;
    pointer_dx = 0;
    pointer_dy = 0;
    printf("M57_INPUT_READY seat=%s keyboard=1 pointer=1\n", seat_name);
    fflush(stdout);
}

static void maybe_input_finished(void) {
    const int dx = wl_fixed_to_int(pointer_dx);
    const int dy = wl_fixed_to_int(pointer_dy);
    if (!input_ready || input_finished || !key_down || !key_up ||
        dx != 7 || dy != -4 || !button_down || !button_up) return;
    input_finished = 1;
    printf("M57_INPUT_PASS key=30/1/0 motion=7,-4 button=272/1/0\n");
    fflush(stdout);
}

static void frame_complete(void *data, struct wl_callback *callback, uint32_t time) {
    (void)data;
    (void)time;
    frame_done = 1;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = {.done = frame_complete};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 7 ? version : 7);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = {.ping = wm_ping};

static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(surface, serial);
    configured = 1;
}

static const struct xdg_surface_listener surface_listener = {.configure = surface_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height, struct wl_array *states) {
    (void)data; (void)toplevel; (void)width; (void)height; (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)data; (void)toplevel;
}

static void toplevel_bounds(void *data, struct xdg_toplevel *toplevel,
                            int32_t width, int32_t height) {
    (void)data; (void)toplevel; (void)width; (void)height;
}

static void toplevel_capabilities(void *data, struct xdg_toplevel *toplevel,
                                  struct wl_array *capabilities) {
    (void)data; (void)toplevel; (void)capabilities;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_bounds,
    .wm_capabilities = toplevel_capabilities,
};

static void keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
                            uint32_t format, int32_t fd, uint32_t size) {
    (void)data; (void)wl_keyboard;
    keymap_ready = format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && fd >= 0 && size > 0;
    if (fd >= 0) close(fd);
    if (keymap_ready)
        printf("M57_KEYMAP_OK format=%u bytes=%u\n", format, size);
    else
        printf("M57_KEYMAP_FAIL format=%u bytes=%u errno=%d\n", format, size, errno);
    fflush(stdout);
    maybe_input_ready();
}

static void keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys) {
    (void)data; (void)wl_keyboard; (void)keys;
    keyboard_focus = surface == client_surface;
    if (keyboard_focus) {
        printf("M57_KEYBOARD_ENTER serial=%u\n", serial);
        fflush(stdout);
    }
    maybe_input_ready();
}

static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
                           uint32_t serial, struct wl_surface *surface) {
    (void)data; (void)wl_keyboard; (void)serial; (void)surface;
    keyboard_focus = 0;
}

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state) {
    (void)data; (void)wl_keyboard; (void)time;
    printf("M57_KEY code=%u state=%u serial=%u\n", key, state, serial);
    fflush(stdout);
    if (key == 30 && state == WL_KEYBOARD_KEY_STATE_PRESSED) key_down = 1;
    if (key == 30 && state == WL_KEYBOARD_KEY_STATE_RELEASED) key_up = 1;
    maybe_input_finished();
}

static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
                               uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked,
                               uint32_t group) {
    (void)data; (void)wl_keyboard; (void)serial; (void)depressed;
    (void)latched; (void)locked; (void)group;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
                                 int32_t rate, int32_t delay) {
    (void)data; (void)wl_keyboard; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void pointer_enter(void *data, struct wl_pointer *wl_pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)wl_pointer;
    pointer_focus = surface == client_surface;
    pointer_x = x;
    pointer_y = y;
    if (pointer_focus) {
        printf("M57_POINTER_ENTER serial=%u x=%d y=%d\n",
               serial, wl_fixed_to_int(x), wl_fixed_to_int(y));
        fflush(stdout);
    }
    maybe_input_ready();
}

static void pointer_leave(void *data, struct wl_pointer *wl_pointer,
                          uint32_t serial, struct wl_surface *surface) {
    (void)data; (void)wl_pointer; (void)serial; (void)surface;
    pointer_focus = 0;
}

static void pointer_motion(void *data, struct wl_pointer *wl_pointer,
                           uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)wl_pointer; (void)time;
    if (input_ready && pointer_focus) {
        pointer_dx += x - pointer_x;
        pointer_dy += y - pointer_y;
        printf("M57_MOTION dx=%d dy=%d\n",
               wl_fixed_to_int(pointer_dx), wl_fixed_to_int(pointer_dy));
        fflush(stdout);
    }
    pointer_x = x;
    pointer_y = y;
    maybe_input_finished();
}

static void pointer_button(void *data, struct wl_pointer *wl_pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state) {
    (void)data; (void)wl_pointer; (void)time;
    printf("M57_BUTTON btn=%u state=%u serial=%u\n", button, state, serial);
    fflush(stdout);
    if (button == 272 && state == WL_POINTER_BUTTON_STATE_PRESSED) button_down = 1;
    if (button == 272 && state == WL_POINTER_BUTTON_STATE_RELEASED) button_up = 1;
    maybe_input_finished();
}

static void pointer_axis(void *data, struct wl_pointer *wl_pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)data; (void)wl_pointer; (void)time; (void)axis; (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *wl_pointer) {
    (void)data; (void)wl_pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
                                uint32_t source) {
    (void)data; (void)wl_pointer; (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
                              uint32_t time, uint32_t axis) {
    (void)data; (void)wl_pointer; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t axis, int32_t discrete) {
    (void)data; (void)wl_pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *wl_seat,
                              uint32_t capabilities) {
    (void)data;
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && keyboard == NULL) {
        keyboard = wl_seat_get_keyboard(wl_seat);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && pointer == NULL) {
        pointer = wl_seat_get_pointer(wl_seat);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
    printf("M57_SEAT_CAPS keyboard=%d pointer=%d\n",
           keyboard != NULL, pointer != NULL);
    fflush(stdout);
}

static void seat_name_event(void *data, struct wl_seat *wl_seat,
                            const char *name) {
    (void)data; (void)wl_seat;
    snprintf(seat_name, sizeof(seat_name), "%s", name);
    printf("M57_SEAT_NAME=%s\n", seat_name);
    fflush(stdout);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name_event,
};

int main(void) {
    input_mode = getenv("M57_INPUT") != NULL;
    struct wl_display *display = NULL;
    char display_name[32];
    for (int index = 0; index < 10 && display == NULL; ++index) {
        snprintf(display_name, sizeof(display_name), "wayland-%d", index);
        (void)setenv("WAYLAND_DISPLAY", display_name, 1);
        display = wl_display_connect(NULL);
    }
    if (display == NULL) {
        printf("M51_WL_CONNECT_FAIL errno=%d\n", errno);
        return 1;
    }
    printf("M51_WL_CONNECT_OK display=%s\n", display_name);
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0 || compositor == NULL || shm == NULL ||
        wm_base == NULL || seat == NULL) {
        printf("M51_WL_GLOBALS_FAIL compositor=%d shm=%d xdg=%d seat=%d errno=%d\n",
               compositor != NULL, shm != NULL, wm_base != NULL, seat != NULL, errno);
        return 2;
    }
    printf("M51_WL_GLOBALS_OK compositor=1 shm=1 xdg=1 seat=1\n");
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    wl_seat_add_listener(seat, &seat_listener, NULL);

    const int width = 256, height = 192, stride = width * 4;
    const size_t bytes = (size_t)stride * height;
    int fd = memfd_create("m51-wl-shm", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)bytes) < 0) {
        printf("M51_WL_MEMFD_FAIL errno=%d\n", errno);
        return 3;
    }
    uint32_t *pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        printf("M51_WL_MMAP_FAIL errno=%d\n", errno);
        return 4;
    }
    for (size_t i = 0; i < bytes / sizeof(*pixels); ++i) pixels[i] = 0xff336699u;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)bytes);
    close(fd);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                                         WL_SHM_FORMAT_XRGB8888);
    printf("M51_WL_SHM_BUFFER_OK bytes=%zu\n", bytes);
    printf("M56_WL_SHM_TRANSFER bytes=%zu\n", bytes);

    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    client_surface = surface;
    struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &surface_listener, NULL);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_app_id(toplevel, "m56-shm");
    xdg_toplevel_set_title(toplevel, "PachaOS M5.1 wl_shm probe");
    xdg_toplevel_set_min_size(toplevel, width, height);
    xdg_toplevel_set_max_size(toplevel, width, height);
    wl_surface_commit(surface);
    if (wl_display_roundtrip(display) < 0 || !configured) {
        printf("M51_WL_CONFIGURE_FAIL configured=%d errno=%d\n", configured, errno);
        return 5;
    }
    printf("M56_WL_XDG_CONFIGURE_OK\n");
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    struct wl_callback *frame = wl_surface_frame(surface);
    wl_callback_add_listener(frame, &frame_listener, NULL);
    wl_surface_commit(surface);
    while (!frame_done && wl_display_dispatch(display) >= 0) {}
    if (!frame_done || wl_display_roundtrip(display) < 0) {
        printf("M51_WL_COMMIT_FAIL errno=%d\n", errno);
        return 6;
    }
    frame_done = 0;
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    frame = wl_surface_frame(surface);
    wl_callback_add_listener(frame, &frame_listener, NULL);
    wl_surface_commit(surface);
    while (!frame_done && wl_display_dispatch(display) >= 0) {}
    if (!frame_done) {
        printf("M51_WL_COMMIT_FAIL errno=%d\n", errno);
        return 6;
    }
    usleep(500000);
    printf("M51_WL_SURFACE_COMMIT_OK color=#336699 size=%dx%d\n", width, height);
    printf("M56_WL_SURFACE_READY color=#336699 size=%dx%d\n", width, height);
    fflush(stdout);
    surface_ready = 1;
    maybe_input_ready();
    if (input_mode) {
        while (!input_finished && wl_display_dispatch(display) >= 0) {}
        if (!input_finished) return 7;
    } else {
        sleep(5);
    }
    wl_display_disconnect(display);
    return 0;
}

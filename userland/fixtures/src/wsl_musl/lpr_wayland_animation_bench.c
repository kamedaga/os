#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define SAMPLE_CAPACITY 2048u
#define INPUT_SAMPLE_CAPACITY 32u

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wp_presentation *presentation;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;
static struct wl_surface *surface;
static int configured;
static int surface_ready;
static int keymap_ready;
static int keyboard_focus;
static int pointer_focus;
static int input_ready;
static int input_done;
static int key_down;
static int key_up;
static int motion_seen;
static int button_down;
static int button_up;
static uint32_t presentation_clock_id;
struct input_sample {
    uint32_t event_time_ms;
    uint64_t receive_ns;
};

static uint64_t input_samples_ms[INPUT_SAMPLE_CAPACITY];
static struct input_sample input_samples[INPUT_SAMPLE_CAPACITY];
static size_t input_sample_count;

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static pid_t start_input_stress_markers(void)
{
    const char *enabled = getenv("P4_MOUSE_STRESS");
    if (enabled == NULL || strcmp(enabled, "1") != 0) return -1;
    const pid_t pid = fork();
    if (pid != 0) return pid;

    uint64_t deadline_ns = monotonic_ns() + 50000000ull;
    for (unsigned step = 1; step <= 60; step++, deadline_ns += 50000000ull) {
        const struct timespec deadline = {
            .tv_sec = (time_t)(deadline_ns / 1000000000ull),
            .tv_nsec = (long)(deadline_ns % 1000000000ull),
        };
        int status;
        do {
            status = clock_nanosleep(
                CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        } while (status == EINTR);
        if (status != 0 ||
            dprintf(STDOUT_FILENO, "P4_MOUSE_STEP_%u\n", step) < 0)
            _Exit(1);
    }
    _Exit(0);
}

static int compare_u64(const void *left, const void *right)
{
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : a > b;
}

static uint64_t percentile(const uint64_t *samples, size_t count, unsigned percent)
{
    uint64_t sorted[SAMPLE_CAPACITY];
    if (count == 0) return 0;
    if (count > SAMPLE_CAPACITY) count = SAMPLE_CAPACITY;
    memcpy(sorted, samples, count * sizeof(sorted[0]));
    qsort(sorted, count, sizeof(sorted[0]), compare_u64);
    return sorted[((count - 1u) * percent) / 100u];
}

static void record_input_latency(uint32_t event_time_ms)
{
    if (!input_ready || input_sample_count >= INPUT_SAMPLE_CAPACITY) return;
    const uint64_t receive_ns = monotonic_ns();
    const uint32_t now_ms = (uint32_t)(receive_ns / 1000000ull);
    input_samples_ms[input_sample_count++] = (uint32_t)(now_ms - event_time_ms);
    input_samples[input_sample_count - 1u] = (struct input_sample){
        .event_time_ms = event_time_ms,
        .receive_ns = receive_ns,
    };
}

static void maybe_input_ready(void)
{
    if (input_ready || !surface_ready || !keymap_ready ||
        !keyboard_focus || !pointer_focus) return;
    input_ready = 1;
    printf("P4_BENCH_INPUT_READY source=wayland-event-time\n");
    fflush(stdout);
}

static void maybe_input_done(void)
{
    if (!input_ready || input_done || !key_down || !key_up ||
        !motion_seen || !button_down || !button_up) return;
    input_done = 1;
    printf(
        "P4_BENCH_INPUT count=%zu p50_ms=%llu p99_ms=%llu max_ms=%llu source=irq-ready-to-wayland-client\n",
        input_sample_count,
        (unsigned long long)percentile(input_samples_ms, input_sample_count, 50),
        (unsigned long long)percentile(input_samples_ms, input_sample_count, 99),
        (unsigned long long)percentile(input_samples_ms, input_sample_count, 100));
    fflush(stdout);
}

static void print_input_breakdown(void)
{
    for (size_t i = 0; i < input_sample_count; i++) {
        printf(
            "P4_BENCH_INPUT_EVENT time_ms=%u receive_ns=%llu\n",
            input_samples[i].event_time_ms,
            (unsigned long long)input_samples[i].receive_ns);
    }
    printf("P4_BENCH_INPUT_BOUNDARY press_to_irq=unobserved start=irq-ready-monotonic\n");
    fflush(stdout);
}

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(
            registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    } else if (strcmp(interface, wp_presentation_interface.name) == 0) {
        presentation = wl_registry_bind(
            registry, name, &wp_presentation_interface, version < 2 ? version : 2);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(
            registry, name, &wl_seat_interface, version < 7 ? version : 7);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

struct buffer_slot {
    struct wl_buffer *object;
    uint32_t *pixels;
    int released;
};

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    struct buffer_slot *slot = data;
    slot->released = 1;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = {.ping = wm_ping};

static void presentation_clock(void *data, struct wp_presentation *object,
                               uint32_t clock_id)
{
    (void)data;
    (void)object;
    presentation_clock_id = clock_id;
}

static const struct wp_presentation_listener presentation_listener = {
    .clock_id = presentation_clock,
};

static void surface_configure(void *data, struct xdg_surface *xdg_surface,
                              uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(xdg_surface, serial);
    configured = 1;
}

static const struct xdg_surface_listener surface_listener = {
    .configure = surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)data;
    (void)toplevel;
}

static void toplevel_bounds(void *data, struct xdg_toplevel *toplevel,
                            int32_t width, int32_t height)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
}

static void toplevel_capabilities(void *data, struct xdg_toplevel *toplevel,
                                  struct wl_array *capabilities)
{
    (void)data;
    (void)toplevel;
    (void)capabilities;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_bounds,
    .wm_capabilities = toplevel_capabilities,
};

static void keyboard_keymap(void *data, struct wl_keyboard *object,
                            uint32_t format, int32_t fd, uint32_t size)
{
    (void)data;
    (void)object;
    keymap_ready = format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && fd >= 0 && size > 0;
    if (fd >= 0) close(fd);
    maybe_input_ready();
}

static void keyboard_enter(void *data, struct wl_keyboard *object,
                           uint32_t serial, struct wl_surface *entered,
                           struct wl_array *keys)
{
    (void)data;
    (void)object;
    (void)serial;
    (void)keys;
    keyboard_focus = entered == surface;
    maybe_input_ready();
}

static void keyboard_leave(void *data, struct wl_keyboard *object,
                           uint32_t serial, struct wl_surface *left)
{
    (void)data;
    (void)object;
    (void)serial;
    (void)left;
    keyboard_focus = 0;
}

static void keyboard_key(void *data, struct wl_keyboard *object,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state)
{
    (void)data;
    (void)object;
    (void)serial;
    record_input_latency(time);
    if (key == 30 && state == WL_KEYBOARD_KEY_STATE_PRESSED) key_down = 1;
    if (key == 30 && state == WL_KEYBOARD_KEY_STATE_RELEASED) key_up = 1;
    maybe_input_done();
}

static void keyboard_modifiers(void *data, struct wl_keyboard *object,
                               uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    (void)data;
    (void)object;
    (void)serial;
    (void)depressed;
    (void)latched;
    (void)locked;
    (void)group;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *object,
                                 int32_t rate, int32_t delay)
{
    (void)data;
    (void)object;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void pointer_enter(void *data, struct wl_pointer *object,
                          uint32_t serial, struct wl_surface *entered,
                          wl_fixed_t x, wl_fixed_t y)
{
    (void)data;
    (void)x;
    (void)y;
    const char *hide_cursor = getenv("P4_HIDE_CURSOR");
    if (hide_cursor != NULL && strcmp(hide_cursor, "1") == 0) {
        wl_pointer_set_cursor(object, serial, NULL, 0, 0);
    }
    pointer_focus = entered == surface;
    maybe_input_ready();
}

static void pointer_leave(void *data, struct wl_pointer *object,
                          uint32_t serial, struct wl_surface *left)
{
    (void)data;
    (void)object;
    (void)serial;
    (void)left;
    pointer_focus = 0;
}

static void pointer_motion(void *data, struct wl_pointer *object,
                           uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data;
    (void)object;
    (void)x;
    (void)y;
    record_input_latency(time);
    motion_seen = 1;
    maybe_input_done();
}

static void pointer_button(void *data, struct wl_pointer *object,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state)
{
    (void)data;
    (void)object;
    (void)serial;
    record_input_latency(time);
    if (button == 272 && state == WL_POINTER_BUTTON_STATE_PRESSED) button_down = 1;
    if (button == 272 && state == WL_POINTER_BUTTON_STATE_RELEASED) button_up = 1;
    maybe_input_done();
}

static void pointer_axis(void *data, struct wl_pointer *object,
                         uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data;
    (void)object;
    (void)time;
    (void)axis;
    (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *object)
{
    (void)data;
    (void)object;
}

static void pointer_axis_source(void *data, struct wl_pointer *object,
                                uint32_t source)
{
    (void)data;
    (void)object;
    (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *object,
                              uint32_t time, uint32_t axis)
{
    (void)data;
    (void)object;
    (void)time;
    (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *object,
                                  uint32_t axis, int32_t discrete)
{
    (void)data;
    (void)object;
    (void)axis;
    (void)discrete;
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

static void seat_capabilities(void *data, struct wl_seat *object,
                              uint32_t capabilities)
{
    (void)data;
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && keyboard == NULL) {
        keyboard = wl_seat_get_keyboard(object);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && pointer == NULL) {
        pointer = wl_seat_get_pointer(object);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}

static void seat_name(void *data, struct wl_seat *object, const char *name)
{
    (void)data;
    (void)object;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

struct frame_sample {
    int callback_done;
    int feedback_done;
    int presented;
    uint64_t timestamp_ns;
};

static void feedback_sync_output(void *data, struct wp_presentation_feedback *object,
                                 struct wl_output *output)
{
    (void)data;
    (void)object;
    (void)output;
}

static void feedback_presented(void *data, struct wp_presentation_feedback *object,
                               uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                               uint32_t tv_nsec, uint32_t refresh,
                               uint32_t seq_hi, uint32_t seq_lo,
                               uint32_t flags)
{
    (void)refresh;
    (void)seq_hi;
    (void)seq_lo;
    (void)flags;
    struct frame_sample *state = data;
    state->timestamp_ns = (((uint64_t)tv_sec_hi << 32) | tv_sec_lo) * 1000000000ull + tv_nsec;
    state->presented = 1;
    state->feedback_done = 1;
    wp_presentation_feedback_destroy(object);
}

static void feedback_discarded(void *data, struct wp_presentation_feedback *object)
{
    struct frame_sample *state = data;
    state->feedback_done = 1;
    wp_presentation_feedback_destroy(object);
}

static const struct wp_presentation_feedback_listener feedback_listener = {
    .sync_output = feedback_sync_output,
    .presented = feedback_presented,
    .discarded = feedback_discarded,
};

static int callback_done;

static void frame_complete(void *data, struct wl_callback *callback, uint32_t time)
{
    (void)time;
    struct frame_sample *sample = data;
    if (sample != NULL) {
        sample->callback_done = 1;
    } else {
        callback_done = 1;
    }
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = {.done = frame_complete};

static void fill_rect(uint32_t *pixels, int stride_pixels,
                      int x, int y, int width, int height, uint32_t color)
{
    for (int row = y; row < y + height; ++row) {
        for (int column = x; column < x + width; ++column)
            pixels[(size_t)row * (size_t)stride_pixels + (size_t)column] = color;
    }
}

int main(void)
{
    struct wl_display *display = NULL;
    char display_name[32];
    for (int index = 0; index < 10 && display == NULL; ++index) {
        snprintf(display_name, sizeof(display_name), "wayland-%d", index);
        (void)setenv("WAYLAND_DISPLAY", display_name, 1);
        display = wl_display_connect(NULL);
    }
    if (display == NULL) {
        fprintf(stderr, "P4_BENCH_FAIL stage=connect errno=%d\n", errno);
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0 || compositor == NULL || shm == NULL ||
        wm_base == NULL || seat == NULL) {
        fprintf(stderr, "P4_BENCH_FAIL stage=globals errno=%d\n", errno);
        return 2;
    }
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    wl_seat_add_listener(seat, &seat_listener, NULL);
    if (presentation != NULL)
        wp_presentation_add_listener(presentation, &presentation_listener, NULL);

    const int width = 640;
    const int height = 480;
    const int stride = width * 4;
    const size_t bytes = (size_t)stride * height;
    const char *damage_mode = getenv("P4_ANIMATION_DAMAGE");
    const int small_damage = damage_mode != NULL && strcmp(damage_mode, "small") == 0;
    int fd = memfd_create("p4-wayland-animation", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)(bytes * 2u)) < 0) return 3;
    uint32_t *pixels = mmap(NULL, bytes * 2u, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) return 4;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)(bytes * 2u));
    close(fd);
    struct buffer_slot buffers[2] = {0};
    for (size_t i = 0; i < 2u; ++i) {
        buffers[i].pixels = pixels + (bytes / sizeof(*pixels)) * i;
        buffers[i].released = 1;
        buffers[i].object = wl_shm_pool_create_buffer(
            pool, (int32_t)(bytes * i), width, height, stride, WL_SHM_FORMAT_XRGB8888);
        wl_buffer_add_listener(buffers[i].object, &buffer_listener, &buffers[i]);
    }

    surface = wl_compositor_create_surface(compositor);
    struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &surface_listener, NULL);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_app_id(toplevel, "p4-animation");
    xdg_toplevel_set_title(toplevel, "P4 animation benchmark");
    xdg_toplevel_set_min_size(toplevel, width, height);
    xdg_toplevel_set_max_size(toplevel, width, height);
    wl_surface_commit(surface);
    if (wl_display_roundtrip(display) < 0 || !configured) return 5;

    for (size_t slot = 0; slot < 2u; ++slot) {
        for (size_t i = 0; i < bytes / sizeof(*pixels); ++i)
            buffers[slot].pixels[i] = 0xff204060u;
    }
    buffers[0].released = 0;
    wl_surface_attach(surface, buffers[0].object, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    callback_done = 0;
    struct wl_callback *callback = wl_surface_frame(surface);
    wl_callback_add_listener(callback, &frame_listener, NULL);
    wl_surface_commit(surface);
    while (!callback_done && wl_display_dispatch(display) >= 0) {}
    if (!callback_done) return 6;
    surface_ready = 1;
    maybe_input_ready();
    while (!input_done && wl_display_dispatch(display) >= 0) {}
    if (!input_done) return 7;
    print_input_breakdown();

    uint64_t frame_samples_us[SAMPLE_CAPACITY];
    uint64_t present_samples_us[SAMPLE_CAPACITY];
    struct frame_sample frame_details[SAMPLE_CAPACITY];
    memset(frame_details, 0, sizeof(frame_details));
    size_t frame_count = 0;
    size_t present_count = 0;
    size_t frame_detail_count = 0;
    uint64_t previous_frame_ns = 0;
    uint64_t previous_present_ns = 0;
    unsigned discarded = 0;
    const uint64_t start_ns = monotonic_ns();
    const uint64_t deadline_ns = start_ns + 5000000000ull;
    uint64_t frame_index = 1;
    printf("P4_BENCH_ANIMATION_MODE damage=%s\n", small_damage ? "small" : "full");
    fflush(stdout);
    const pid_t stress_pid = start_input_stress_markers();
    if (stress_pid == 0) return 10;

    while (monotonic_ns() < deadline_ns && frame_detail_count < SAMPLE_CAPACITY) {
        struct buffer_slot *slot = &buffers[frame_index % 2u];
        while (!slot->released && wl_display_dispatch(display) >= 0) {}
        if (!slot->released) return 8;
        struct frame_sample *frame = &frame_details[frame_detail_count];
        const uint32_t background = 0xff204060u;
        const uint32_t color = (frame_index & 1u) == 0 ? 0xff285078u : 0xff704028u;
        if (small_damage) {
            const int rect_size = 64;
            const int current_x = (int)((frame_index * 37u) % (uint64_t)(width - rect_size));
            const int current_y = (int)((frame_index * 23u) % (uint64_t)(height - rect_size));
            if (frame_index >= 3u) {
                const uint64_t stale_index = frame_index - 2u;
                const int stale_x = (int)((stale_index * 37u) % (uint64_t)(width - rect_size));
                const int stale_y = (int)((stale_index * 23u) % (uint64_t)(height - rect_size));
                fill_rect(slot->pixels, width, stale_x, stale_y,
                          rect_size, rect_size, background);
            }
            fill_rect(slot->pixels, width, current_x, current_y,
                      rect_size, rect_size, color);
        } else {
            for (size_t i = 0; i < bytes / sizeof(*pixels); ++i)
                slot->pixels[i] = color;
        }
        slot->released = 0;
        wl_surface_attach(surface, slot->object, 0, 0);
        if (small_damage) {
            const int rect_size = 64;
            if (frame_index >= 2u) {
                const uint64_t previous_index = frame_index - 1u;
                wl_surface_damage_buffer(
                    surface,
                    (int32_t)((previous_index * 37u) % (uint64_t)(width - rect_size)),
                    (int32_t)((previous_index * 23u) % (uint64_t)(height - rect_size)),
                    rect_size, rect_size);
            }
            wl_surface_damage_buffer(
                surface,
                (int32_t)((frame_index * 37u) % (uint64_t)(width - rect_size)),
                (int32_t)((frame_index * 23u) % (uint64_t)(height - rect_size)),
                rect_size, rect_size);
        } else {
            wl_surface_damage_buffer(surface, 0, 0, width, height);
        }
        callback = wl_surface_frame(surface);
        wl_callback_add_listener(callback, &frame_listener, frame);
        if (presentation != NULL) {
            struct wp_presentation_feedback *object =
                wp_presentation_feedback(presentation, surface);
            wp_presentation_feedback_add_listener(object, &feedback_listener, frame);
        }
        wl_surface_commit(surface);
        while ((!frame->callback_done ||
                (presentation != NULL && !frame->feedback_done)) &&
               wl_display_dispatch(display) >= 0) {}
        if (!frame->callback_done) return 8;

        const uint64_t now_ns = monotonic_ns();
        if (previous_frame_ns != 0)
            frame_samples_us[frame_count++] = (now_ns - previous_frame_ns) / 1000ull;
        previous_frame_ns = now_ns;
        if (frame->presented) {
            if (previous_present_ns != 0 && present_count < SAMPLE_CAPACITY)
                present_samples_us[present_count++] =
                    (frame->timestamp_ns - previous_present_ns) / 1000ull;
            previous_present_ns = frame->timestamp_ns;
        } else if (presentation != NULL) {
            discarded++;
        }
        frame_detail_count++;
        frame_index++;
    }

    const uint64_t elapsed_ns = monotonic_ns() - start_ns;
    const uint64_t *samples = present_count != 0 ? present_samples_us : frame_samples_us;
    const size_t count = present_count != 0 ? present_count : frame_count;
    const char *source = present_count != 0 ? "wp_presentation" : "frame_callback";
    const uint64_t fps_milli = elapsed_ns == 0 ? 0 :
        (uint64_t)count * 1000000000000ull / elapsed_ns;
    printf(
        "P4_BENCH_FRAME source=%s clock_id=%u count=%zu p50_us=%llu p95_us=%llu p99_us=%llu max_us=%llu fps=%llu.%03llu discarded=%u\n",
        source, presentation_clock_id, count,
        (unsigned long long)percentile(samples, count, 50),
        (unsigned long long)percentile(samples, count, 95),
        (unsigned long long)percentile(samples, count, 99),
        (unsigned long long)percentile(samples, count, 100),
        (unsigned long long)(fps_milli / 1000ull),
        (unsigned long long)(fps_milli % 1000ull), discarded);
    printf("P4_BENCH_ANIMATION_DONE elapsed_ms=%llu\n",
           (unsigned long long)(elapsed_ns / 1000000ull));
    fflush(stdout);
    if (stress_pid > 0) {
        int stress_status = 0;
        if (waitpid(stress_pid, &stress_status, 0) != stress_pid ||
            !WIFEXITED(stress_status) || WEXITSTATUS(stress_status) != 0)
            return 10;
    }
    wl_display_disconnect(display);
    return count == 0 ? 9 : 0;
}

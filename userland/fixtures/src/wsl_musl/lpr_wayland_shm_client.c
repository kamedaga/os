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
static int configured;
static int frame_done;

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

int main(void) {
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
    if (wl_display_roundtrip(display) < 0 || compositor == NULL || shm == NULL || wm_base == NULL) {
        printf("M51_WL_GLOBALS_FAIL compositor=%d shm=%d xdg=%d errno=%d\n",
               compositor != NULL, shm != NULL, wm_base != NULL, errno);
        return 2;
    }
    printf("M51_WL_GLOBALS_OK compositor=1 shm=1 xdg=1\n");
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);

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
    sleep(5);
    wl_display_disconnect(display);
    return 0;
}

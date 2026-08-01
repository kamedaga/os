#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#if !defined(MAP_UNINITIALIZED)
#define MAP_UNINITIALIZED 0
#endif

#if !defined(MFD_NOEXEC_SEAL)
#define MFD_NOEXEC_SEAL 0
#endif

/* Minimal client bindings for protocols whose XML is available in the fixture
 * sysroot, but which the existing one-file build does not generate. */
struct wp_viewporter;
struct wp_viewport;
struct wp_fractional_scale_manager_v1;
struct wp_fractional_scale_v1;
struct zxdg_decoration_manager_v1;
struct zxdg_toplevel_decoration_v1;

static const struct wl_interface wp_viewport_interface;
static const struct wl_interface wp_fractional_scale_v1_interface;
static const struct wl_interface zxdg_toplevel_decoration_v1_interface;

static const struct wl_interface *viewporter_types[] = {
    NULL, NULL, NULL, NULL, &wp_viewport_interface, &wl_surface_interface,
};
static const struct wl_message wp_viewporter_requests[] = {
    {"destroy", "", viewporter_types},
    {"get_viewport", "no", viewporter_types + 4},
};
static const struct wl_message wp_viewport_requests[] = {
    {"destroy", "", viewporter_types},
    {"set_source", "ffff", viewporter_types},
    {"set_destination", "ii", viewporter_types},
};
static const struct wl_interface wp_viewporter_interface = {
    "wp_viewporter", 1, 2, wp_viewporter_requests, 0, NULL,
};
static const struct wl_interface wp_viewport_interface = {
    "wp_viewport", 1, 3, wp_viewport_requests, 0, NULL,
};

static const struct wl_interface *fractional_scale_types[] = {
    NULL, &wp_fractional_scale_v1_interface, &wl_surface_interface,
};
static const struct wl_message fractional_scale_manager_requests[] = {
    {"destroy", "", fractional_scale_types},
    {"get_fractional_scale", "no", fractional_scale_types + 1},
};
static const struct wl_message fractional_scale_requests[] = {
    {"destroy", "", fractional_scale_types},
};
static const struct wl_message fractional_scale_events[] = {
    {"preferred_scale", "u", fractional_scale_types},
};
static const struct wl_interface wp_fractional_scale_manager_v1_interface = {
    "wp_fractional_scale_manager_v1", 1,
    2, fractional_scale_manager_requests, 0, NULL,
};
static const struct wl_interface wp_fractional_scale_v1_interface = {
    "wp_fractional_scale_v1", 1,
    1, fractional_scale_requests, 1, fractional_scale_events,
};

static const struct wl_interface *decoration_types[] = {
    NULL, &zxdg_toplevel_decoration_v1_interface, &xdg_toplevel_interface,
};
static const struct wl_message decoration_manager_requests[] = {
    {"destroy", "", decoration_types},
    {"get_toplevel_decoration", "no", decoration_types + 1},
};
static const struct wl_message toplevel_decoration_requests[] = {
    {"destroy", "", decoration_types},
    {"set_mode", "u", decoration_types},
    {"unset_mode", "", decoration_types},
};
static const struct wl_message toplevel_decoration_events[] = {
    {"configure", "u", decoration_types},
};
static const struct wl_interface zxdg_decoration_manager_v1_interface = {
    "zxdg_decoration_manager_v1", 1,
    2, decoration_manager_requests, 0, NULL,
};
static const struct wl_interface zxdg_toplevel_decoration_v1_interface = {
    "zxdg_toplevel_decoration_v1", 1,
    3, toplevel_decoration_requests, 1, toplevel_decoration_events,
};

static struct wp_viewport *get_viewport(struct wp_viewporter *viewporter,
                                        struct wl_surface *surface) {
    return (struct wp_viewport *)wl_proxy_marshal_flags(
        (struct wl_proxy *)viewporter, 1, &wp_viewport_interface,
        wl_proxy_get_version((struct wl_proxy *)viewporter), 0, NULL, surface);
}

static struct wp_fractional_scale_v1 *get_fractional_scale(
    struct wp_fractional_scale_manager_v1 *manager, struct wl_surface *surface) {
    return (struct wp_fractional_scale_v1 *)wl_proxy_marshal_flags(
        (struct wl_proxy *)manager, 1, &wp_fractional_scale_v1_interface,
        wl_proxy_get_version((struct wl_proxy *)manager), 0, NULL, surface);
}

static struct zxdg_toplevel_decoration_v1 *get_toplevel_decoration(
    struct zxdg_decoration_manager_v1 *manager, struct xdg_toplevel *toplevel) {
    return (struct zxdg_toplevel_decoration_v1 *)wl_proxy_marshal_flags(
        (struct wl_proxy *)manager, 1, &zxdg_toplevel_decoration_v1_interface,
        wl_proxy_get_version((struct wl_proxy *)manager), 0, NULL, toplevel);
}

static void request_server_side_decoration(
    struct zxdg_toplevel_decoration_v1 *decoration) {
    wl_proxy_marshal_flags((struct wl_proxy *)decoration, 1, NULL,
                           wl_proxy_get_version((struct wl_proxy *)decoration),
                           0, 2u);
}

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;
static struct wl_subcompositor *subcompositor;
static struct wp_viewporter *viewporter;
static struct wp_fractional_scale_manager_v1 *fractional_scale_manager;
static struct zxdg_decoration_manager_v1 *decoration_manager;
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
static const char *m58_iteration;
static int foot_surface_mode;
static uint32_t pending_configure_serial;

static void maybe_input_ready(void) {
    if (!input_mode || input_ready || !surface_ready || !keymap_ready ||
        !keyboard_focus || !pointer_focus) return;
    input_ready = 1;
    pointer_dx = 0;
    pointer_dy = 0;
    printf("M57_INPUT_READY seat=%s keyboard=1 pointer=1\n", seat_name);
    if (m58_iteration != NULL)
        printf("M58_INPUT_READY iteration=%s seat=%s keyboard=1 pointer=1\n",
               m58_iteration, seat_name);
    fflush(stdout);
}

static void maybe_input_finished(void) {
    const int dx = wl_fixed_to_int(pointer_dx);
    const int dy = wl_fixed_to_int(pointer_dy);
    if (!input_ready || input_finished || !key_down || !key_up ||
        dx != 7 || dy != -4 || !button_down || !button_up) return;
    input_finished = 1;
    printf("M57_INPUT_PASS key=30/1/0 motion=7,-4 button=272/1/0\n");
    if (m58_iteration != NULL)
        printf("M58_INPUT_PASS iteration=%s key=30/1/0 motion=7,-4 button=272/1/0\n",
               m58_iteration);
    fflush(stdout);
}

static void frame_complete(void *data, struct wl_callback *callback, uint32_t time) {
    (void)data;
    (void)time;
    frame_done = 1;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = {.done = frame_complete};

struct foot_fractional_scale_listener {
    void (*preferred_scale)(void *data,
                            struct wp_fractional_scale_v1 *fractional_scale,
                            uint32_t scale);
};

static void fractional_preferred_scale(
    void *data, struct wp_fractional_scale_v1 *fractional_scale, uint32_t scale) {
    (void)data;
    (void)fractional_scale;
    printf("M56_FOOT_SURFACE_FRACTIONAL_SCALE preferred=%u denominator=120\n",
           scale);
    fflush(stdout);
}

static const struct foot_fractional_scale_listener fractional_scale_listener = {
    .preferred_scale = fractional_preferred_scale,
};

struct foot_decoration_listener {
    void (*configure)(void *data,
                      struct zxdg_toplevel_decoration_v1 *decoration,
                      uint32_t mode);
};

static void decoration_configure(
    void *data, struct zxdg_toplevel_decoration_v1 *decoration, uint32_t mode) {
    (void)data;
    (void)decoration;
    printf("M56_FOOT_SURFACE_DECORATION_CONFIGURE mode=%u\n", mode);
    fflush(stdout);
}

static const struct foot_decoration_listener decoration_listener = {
    .configure = decoration_configure,
};

static int create_shm_buffer(struct wl_shm *shm_global, int width, int height,
                             struct wl_buffer **buffer_out) {
    const int stride = width * 4;
    const size_t bytes = (size_t)stride * height;
    int fd = memfd_create("m51-wl-shm", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)bytes) < 0) {
        printf("M51_WL_MEMFD_FAIL errno=%d\n", errno);
        return 3;
    }
    uint32_t *pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        printf("M51_WL_MMAP_FAIL errno=%d\n", errno);
        close(fd);
        return 4;
    }
    for (size_t i = 0; i < bytes / sizeof(*pixels); ++i) pixels[i] = 0xff336699u;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm_global, fd, (int32_t)bytes);
    close(fd);
    *buffer_out = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                             WL_SHM_FORMAT_XRGB8888);
    printf("M51_WL_SHM_BUFFER_OK bytes=%zu\n", bytes);
    printf("M56_WL_SHM_TRANSFER bytes=%zu\n", bytes);
    return 0;
}

static int create_foot_shm_buffer(struct wl_shm *shm_global, int width, int height,
                                  struct wl_buffer **buffer_out) {
    const int stride = width * 4;
    const size_t bytes = (size_t)stride * height;
    const off_t maximum_pool_size = (off_t)512 * 1024 * 1024;
    const unsigned int first_flags =
        MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL;

    errno = 0;
    int fd = memfd_create("foot-wayland-shm-buffer-pool", first_flags);
    int create_errno = errno;
    int retried = 0;
    if (fd < 0 && create_errno == EINVAL && MFD_NOEXEC_SEAL != 0) {
        retried = 1;
        errno = 0;
        fd = memfd_create("foot-wayland-shm-buffer-pool",
                          MFD_CLOEXEC | MFD_ALLOW_SEALING);
        create_errno = errno;
    }
    printf("M56_FOOT_MEMFD_CREATE flags=0x%x noexec=0x%x retried=%d fd=%d errno=%d\n",
           first_flags, MFD_NOEXEC_SEAL, retried, fd, fd < 0 ? create_errno : 0);
    fflush(stdout);
    if (fd < 0) {
        errno = create_errno;
        printf("M51_WL_MEMFD_FAIL errno=%d\n", errno);
        return 3;
    }

    if (ftruncate(fd, maximum_pool_size) < 0) {
        const int saved_errno = errno;
        printf("M56_FOOT_MEMFD_FAIL stage=ftruncate_initial bytes=%lld errno=%d\n",
               (long long)maximum_pool_size, saved_errno);
        printf("M51_WL_MEMFD_FAIL errno=%d\n", saved_errno);
        fflush(stdout);
        close(fd);
        errno = saved_errno;
        return 3;
    }
    printf("M56_FOOT_MEMFD_TRUNCATE_INITIAL bytes=%lld\n",
           (long long)maximum_pool_size);
    fflush(stdout);

    errno = 0;
    const int punch_status = fallocate(
        fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 1);
    const int punch_errno = errno;
    printf("M56_FOOT_MEMFD_PUNCH_HOLE status=%d errno=%d\n",
           punch_status, punch_status < 0 ? punch_errno : 0);
    fflush(stdout);

    off_t mapping_size = maximum_pool_size;
    if (punch_status < 0) {
        if (ftruncate(fd, (off_t)bytes) < 0) {
            const int saved_errno = errno;
            printf("M56_FOOT_MEMFD_FAIL stage=ftruncate_actual bytes=%zu errno=%d\n",
                   bytes, saved_errno);
            printf("M51_WL_MEMFD_FAIL errno=%d\n", saved_errno);
            fflush(stdout);
            close(fd);
            errno = saved_errno;
            return 3;
        }
        mapping_size = (off_t)bytes;
        printf("M56_FOOT_MEMFD_TRUNCATE_ACTUAL bytes=%zu punch_errno=%d\n",
               bytes, punch_errno);
        fflush(stdout);
    }

    uint32_t *pixels = mmap(NULL, (size_t)mapping_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_UNINITIALIZED, fd, 0);
    if (pixels == MAP_FAILED) {
        const int saved_errno = errno;
        printf("M56_FOOT_MEMFD_FAIL stage=mmap bytes=%lld flags=0x%x errno=%d\n",
               (long long)mapping_size, MAP_SHARED | MAP_UNINITIALIZED,
               saved_errno);
        printf("M51_WL_MMAP_FAIL errno=%d\n", saved_errno);
        fflush(stdout);
        close(fd);
        errno = saved_errno;
        return 4;
    }
    printf("M56_FOOT_MEMFD_MMAP bytes=%lld flags=0x%x\n",
           (long long)mapping_size, MAP_SHARED | MAP_UNINITIALIZED);
    fflush(stdout);
    for (size_t i = 0; i < bytes / sizeof(*pixels); ++i) pixels[i] = 0xff336699u;

    errno = 0;
    const int seal_status = fcntl(
        fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);
    const int seal_errno = errno;
    printf("M56_FOOT_MEMFD_SEALS status=%d seals=0x%x errno=%d\n",
           seal_status, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL,
           seal_status < 0 ? seal_errno : 0);
    fflush(stdout);

    struct wl_shm_pool *pool =
        wl_shm_create_pool(shm_global, fd, (int32_t)mapping_size);
    close(fd);
    *buffer_out = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                             WL_SHM_FORMAT_XRGB8888);
    printf("M51_WL_SHM_BUFFER_OK bytes=%zu\n", bytes);
    printf("M56_WL_SHM_TRANSFER bytes=%zu\n", bytes);
    printf("M56_FOOT_MEMFD_READY buffer_bytes=%zu pool_bytes=%lld\n",
           bytes, (long long)mapping_size);
    fflush(stdout);
    return 0;
}

static int wait_for_frame_epoll(struct wl_display *display, int epoll_fd,
                                unsigned int frame_index) {
    const int display_fd = wl_display_get_fd(display);
    printf("M56_MIDSTREAM_EPOLL_WAIT frame=%u\n", frame_index);
    fflush(stdout);
    while (!frame_done) {
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) {
                printf("M56_MIDSTREAM_EPOLL_FAIL frame=%u stage=dispatch_pending errno=%d\n",
                       frame_index, errno);
                fflush(stdout);
                return -1;
            }
            if (frame_done) break;
        }
        if (frame_done) break;

        uint32_t events = EPOLLIN;
        if (wl_display_flush(display) < 0) {
            if (errno != EAGAIN) {
                const int saved_errno = errno;
                wl_display_cancel_read(display);
                errno = saved_errno;
                printf("M56_MIDSTREAM_EPOLL_FAIL frame=%u stage=flush errno=%d\n",
                       frame_index, errno);
                fflush(stdout);
                return -1;
            }
            events |= EPOLLOUT;
        }

        struct epoll_event interest = {
            .events = events,
            .data.fd = display_fd,
        };
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, display_fd, &interest) != 0) {
            const int saved_errno = errno;
            wl_display_cancel_read(display);
            errno = saved_errno;
            printf("M56_MIDSTREAM_EPOLL_FAIL frame=%u stage=epoll_ctl errno=%d\n",
                   frame_index, errno);
            fflush(stdout);
            return -1;
        }

        struct epoll_event event;
        int ready;
        do {
            ready = epoll_wait(epoll_fd, &event, 1, -1);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) {
            const int saved_errno = errno;
            wl_display_cancel_read(display);
            errno = saved_errno;
            printf("M56_MIDSTREAM_EPOLL_FAIL frame=%u stage=epoll_wait errno=%d\n",
                   frame_index, errno);
            fflush(stdout);
            return -1;
        }

        if ((event.events & EPOLLIN) != 0) {
            if (wl_display_read_events(display) < 0) {
                printf("M56_MIDSTREAM_EPOLL_FAIL frame=%u stage=read_events errno=%d\n",
                       frame_index, errno);
                fflush(stdout);
                return -1;
            }
        } else {
            wl_display_cancel_read(display);
        }
        if ((event.events & (EPOLLERR | EPOLLHUP)) != 0 &&
            (event.events & EPOLLIN) == 0) {
            errno = EPIPE;
            printf("M56_MIDSTREAM_EPOLL_FAIL frame=%u stage=epoll_hup errno=%d\n",
                   frame_index, errno);
            fflush(stdout);
            return -1;
        }
        if (wl_display_dispatch_pending(display) < 0) {
            printf("M56_MIDSTREAM_EPOLL_FAIL frame=%u stage=dispatch_pending errno=%d\n",
                   frame_index, errno);
            fflush(stdout);
            return -1;
        }
    }
    printf("M56_MIDSTREAM_FRAME_DONE frame=%u\n", frame_index);
    fflush(stdout);
    return 0;
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        const uint32_t bind_version = foot_surface_mode ? 6 : (version < 4 ? version : 4);
        if (version >= bind_version) {
            compositor = wl_registry_bind(
                registry, name, &wl_compositor_interface, bind_version);
            if (foot_surface_mode)
                printf("M56_FOOT_SURFACE_BIND interface=wl_compositor version=%u advertised=%u\n",
                       bind_version, version);
        } else if (foot_surface_mode) {
            printf("M56_FOOT_SURFACE_UNAVAILABLE interface=wl_compositor required=6 advertised=%u\n",
                   version);
        }
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        const uint32_t bind_version = foot_surface_mode ? 2 : 1;
        if (version >= bind_version) {
            shm = wl_registry_bind(registry, name, &wl_shm_interface, bind_version);
            if (foot_surface_mode)
                printf("M56_FOOT_SURFACE_BIND interface=wl_shm version=%u advertised=%u\n",
                       bind_version, version);
        } else if (foot_surface_mode) {
            printf("M56_FOOT_SURFACE_UNAVAILABLE interface=wl_shm required=2 advertised=%u\n",
                   version);
        }
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        const uint32_t bind_version = foot_surface_mode ? 5 : 1;
        if (version >= bind_version) {
            wm_base = wl_registry_bind(
                registry, name, &xdg_wm_base_interface, bind_version);
            if (foot_surface_mode)
                printf("M56_FOOT_SURFACE_BIND interface=xdg_wm_base version=%u advertised=%u\n",
                       bind_version, version);
        } else if (foot_surface_mode) {
            printf("M56_FOOT_SURFACE_UNAVAILABLE interface=xdg_wm_base required=5 advertised=%u\n",
                   version);
        }
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 7 ? version : 7);
    } else if (foot_surface_mode &&
               strcmp(interface, wl_subcompositor_interface.name) == 0) {
        subcompositor = wl_registry_bind(
            registry, name, &wl_subcompositor_interface, 1);
        printf("M56_FOOT_SURFACE_BIND interface=wl_subcompositor version=1 advertised=%u\n",
               version);
    } else if (foot_surface_mode &&
               strcmp(interface, wp_viewporter_interface.name) == 0) {
        viewporter = wl_registry_bind(
            registry, name, &wp_viewporter_interface, 1);
        printf("M56_FOOT_SURFACE_BIND interface=wp_viewporter version=1 advertised=%u\n",
               version);
    } else if (foot_surface_mode &&
               strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        fractional_scale_manager = wl_registry_bind(
            registry, name, &wp_fractional_scale_manager_v1_interface, 1);
        printf("M56_FOOT_SURFACE_BIND interface=wp_fractional_scale_manager_v1 version=1 advertised=%u\n",
               version);
    } else if (foot_surface_mode &&
               strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        decoration_manager = wl_registry_bind(
            registry, name, &zxdg_decoration_manager_v1_interface, 1);
        printf("M56_FOOT_SURFACE_BIND interface=zxdg_decoration_manager_v1 version=1 advertised=%u\n",
               version);
    }
    if (foot_surface_mode) fflush(stdout);
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
    if (foot_surface_mode) {
        pending_configure_serial = serial;
        printf("M56_FOOT_SURFACE_CONFIGURE_DEFERRED serial=%u\n", serial);
        fflush(stdout);
    } else {
        xdg_surface_ack_configure(surface, serial);
    }
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
    const char *midstream_value = getenv("M56_MIDSTREAM_EPOLL");
    const char *foot_memfd_value = getenv("M56_FOOT_MEMFD");
    const char *foot_surface_value = getenv("M56_FOOT_SURFACE");
    foot_surface_mode =
        foot_surface_value != NULL && strcmp(foot_surface_value, "1") == 0;
    const int foot_memfd =
        foot_surface_mode ||
        (foot_memfd_value != NULL && strcmp(foot_memfd_value, "1") == 0);
    const int midstream_epoll =
        foot_memfd ||
        (midstream_value != NULL && strcmp(midstream_value, "1") == 0);
    m58_iteration = getenv("M58_ITERATION");
    if (m58_iteration != NULL && m58_iteration[0] == '\0') m58_iteration = NULL;
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
        wm_base == NULL || seat == NULL ||
        (foot_surface_mode &&
         (subcompositor == NULL || viewporter == NULL ||
          fractional_scale_manager == NULL || decoration_manager == NULL))) {
        printf("M51_WL_GLOBALS_FAIL compositor=%d shm=%d xdg=%d seat=%d errno=%d\n",
               compositor != NULL, shm != NULL, wm_base != NULL, seat != NULL, errno);
        if (foot_surface_mode)
            printf("M56_FOOT_SURFACE_GLOBALS_FAIL subcompositor=%d viewporter=%d fractional=%d decoration=%d\n",
                   subcompositor != NULL, viewporter != NULL,
                   fractional_scale_manager != NULL, decoration_manager != NULL);
        return 2;
    }
    printf("M51_WL_GLOBALS_OK compositor=1 shm=1 xdg=1 seat=1\n");
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    wl_seat_add_listener(seat, &seat_listener, NULL);

    const int width = foot_surface_mode ? 60 : 256;
    const int height = foot_surface_mode ? 60 : 192;
    struct wl_buffer *buffer = NULL;
    if (midstream_epoll) {
        printf("M56_MIDSTREAM_MODE enabled=1 shm_after_configure=1 epoll_level=1\n");
        if (foot_memfd)
            printf("M56_FOOT_MEMFD_MODE enabled=1 max_pool_bytes=%lld\n",
                   (long long)512 * 1024 * 1024);
        if (foot_surface_mode)
            printf("M56_FOOT_SURFACE_MODE enabled=1 size=%dx%d\n", width, height);
        fflush(stdout);
    } else {
        const int status = create_shm_buffer(shm, width, height, &buffer);
        if (status != 0) return status;
    }

    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    client_surface = surface;
    if (foot_surface_mode) {
        struct wl_region *opaque_region = wl_compositor_create_region(compositor);
        if (surface == NULL || opaque_region == NULL) {
            printf("M56_FOOT_SURFACE_SETUP_FAIL surface=%d opaque_region=%d\n",
                   surface != NULL, opaque_region != NULL);
            fflush(stdout);
            return 5;
        }
        wl_region_add(opaque_region, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_set_opaque_region(surface, opaque_region);
        wl_region_destroy(opaque_region);
        printf("M56_FOOT_SURFACE_OPAQUE_REGION x=0 y=0 width=%d height=%d destroyed=1\n",
               INT32_MAX, INT32_MAX);

        struct wp_viewport *main_viewport = get_viewport(viewporter, surface);
        printf("M56_FOOT_SURFACE_VIEWPORT target=main created=%d\n",
               main_viewport != NULL);

        struct wp_fractional_scale_v1 *fractional_scale =
            get_fractional_scale(fractional_scale_manager, surface);
        const int listener_status = fractional_scale == NULL ? -1 :
            wl_proxy_add_listener(
                (struct wl_proxy *)fractional_scale,
                (void (**)(void))&fractional_scale_listener, NULL);
        printf("M56_FOOT_SURFACE_FRACTIONAL created=%d listener_status=%d\n",
               fractional_scale != NULL, listener_status);
        fflush(stdout);
        if (main_viewport == NULL || fractional_scale == NULL ||
            listener_status != 0)
            return 5;
    }
    struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &surface_listener, NULL);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_app_id(toplevel, "m56-shm");
    xdg_toplevel_set_title(toplevel, "PachaOS M5.1 wl_shm probe");
    if (foot_surface_mode) {
        struct zxdg_toplevel_decoration_v1 *decoration =
            get_toplevel_decoration(decoration_manager, toplevel);
        if (decoration == NULL) {
            printf("M56_FOOT_SURFACE_DECORATION_FAIL created=0 listener_status=-1\n");
            fflush(stdout);
            return 5;
        }
        request_server_side_decoration(decoration);
        printf("M56_FOOT_SURFACE_DECORATION_SSD_REQUEST mode=2\n");
        const int listener_status =
            wl_proxy_add_listener(
                (struct wl_proxy *)decoration,
                (void (**)(void))&decoration_listener, NULL);
        if (listener_status != 0) {
            printf("M56_FOOT_SURFACE_DECORATION_FAIL created=%d listener_status=%d\n",
                   decoration != NULL, listener_status);
            fflush(stdout);
            return 5;
        }
    } else {
        xdg_toplevel_set_min_size(toplevel, width, height);
        xdg_toplevel_set_max_size(toplevel, width, height);
    }
    wl_surface_commit(surface);
    if (foot_surface_mode) {
        printf("M56_FOOT_SURFACE_EMPTY_COMMIT target=main\n");
        fflush(stdout);
    }
    if (wl_display_roundtrip(display) < 0 || !configured) {
        printf("M51_WL_CONFIGURE_FAIL configured=%d errno=%d\n", configured, errno);
        return 5;
    }
    printf("M56_WL_XDG_CONFIGURE_OK\n");
    if (foot_surface_mode) {
        struct wl_surface *child = wl_compositor_create_surface(compositor);
        struct wl_subsurface *subsurface =
            child == NULL ? NULL :
            wl_subcompositor_get_subsurface(subcompositor, child, surface);
        struct wp_viewport *child_viewport =
            child == NULL ? NULL : get_viewport(viewporter, child);
        if (child == NULL || subsurface == NULL || child_viewport == NULL) {
            printf("M56_FOOT_SURFACE_SETUP_FAIL child=%d subsurface=%d viewport=%d\n",
                   child != NULL, subsurface != NULL, child_viewport != NULL);
            fflush(stdout);
            return 5;
        }
        wl_subsurface_set_sync(subsurface);
        printf("M56_FOOT_SURFACE_SUBSURFACE_SYNC created=%d\n",
               subsurface != NULL);

        struct wl_region *empty_input = wl_compositor_create_region(compositor);
        if (empty_input == NULL) {
            printf("M56_FOOT_SURFACE_SETUP_FAIL empty_input=0\n");
            fflush(stdout);
            return 5;
        }
        wl_surface_set_input_region(child, empty_input);
        printf("M56_FOOT_SURFACE_CHILD_EMPTY_INPUT region_created=1\n");
        wl_region_destroy(empty_input);

        printf("M56_FOOT_SURFACE_VIEWPORT target=child created=%d\n",
               child_viewport != NULL);

        if (pending_configure_serial == 0) {
            printf("M56_FOOT_SURFACE_SETUP_FAIL serial=%u\n",
                   pending_configure_serial);
            fflush(stdout);
            return 5;
        }
        xdg_surface_ack_configure(xdg_surface, pending_configure_serial);
        printf("M56_FOOT_SURFACE_ACK_CONFIGURE serial=%u\n",
               pending_configure_serial);
        xdg_toplevel_set_min_size(toplevel, width, height);
        printf("M56_FOOT_SURFACE_MIN_SIZE width=%d height=%d\n", width, height);
        xdg_surface_set_window_geometry(xdg_surface, 0, 0, width, height);
        printf("M56_FOOT_SURFACE_WINDOW_GEOMETRY x=0 y=0 width=%d height=%d\n",
               width, height);
        fflush(stdout);
    }
    int epoll_fd = -1;
    if (midstream_epoll) {
        printf("M56_MIDSTREAM_CONFIGURE_COMPLETE\n");
        fflush(stdout);
        const int status = foot_memfd
            ? create_foot_shm_buffer(shm, width, height, &buffer)
            : create_shm_buffer(shm, width, height, &buffer);
        if (status != 0) return status;
        if (foot_surface_mode)
            printf("M56_FOOT_SURFACE_BUFFER width=%d height=%d format=XRGB8888\n",
                   width, height);
        printf("M56_MIDSTREAM_SHM_AFTER_CONFIGURE bytes=%d\n", width * height * 4);
        fflush(stdout);

        const int display_fd = wl_display_get_fd(display);
        epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        struct epoll_event interest = {
            .events = EPOLLIN,
            .data.fd = display_fd,
        };
        if (epoll_fd < 0 ||
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, display_fd, &interest) != 0) {
            printf("M56_MIDSTREAM_EPOLL_SETUP_FAIL errno=%d\n", errno);
            fflush(stdout);
            if (epoll_fd >= 0) close(epoll_fd);
            return 8;
        }
        printf("M56_MIDSTREAM_EPOLL_READY display_fd=%d\n", display_fd);
        fflush(stdout);
    }
    struct wl_callback *frame;
    if (foot_surface_mode) {
        wl_surface_damage_buffer(surface, 0, 0, width, 0);
        wl_surface_damage_buffer(surface, width, 0, 0, height);
        wl_surface_damage_buffer(surface, 0, height, width, 0);
        wl_surface_damage_buffer(surface, 0, 0, 0, height);
        wl_surface_damage_buffer(surface, 0, 0, width, height);
        printf("M56_FOOT_SURFACE_DAMAGE border_zero=4 full=1 width=%d height=%d\n",
               width, height);
        frame = wl_surface_frame(surface);
        wl_callback_add_listener(frame, &frame_listener, NULL);
        printf("M56_FOOT_SURFACE_FRAME_REQUEST index=1\n");
        wl_surface_set_buffer_scale(surface, 1);
        printf("M56_FOOT_SURFACE_BUFFER_SCALE value=1\n");
        wl_surface_attach(surface, buffer, 0, 0);
        printf("M56_FOOT_SURFACE_ATTACH x=0 y=0\n");
        wl_surface_commit(surface);
        printf("M56_FOOT_SURFACE_CONTENT_COMMIT size=%dx%d\n", width, height);
        fflush(stdout);
    } else {
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, width, height);
        frame = wl_surface_frame(surface);
        wl_callback_add_listener(frame, &frame_listener, NULL);
        wl_surface_commit(surface);
    }
    if (midstream_epoll) {
        if (wait_for_frame_epoll(display, epoll_fd, 1) < 0 || !frame_done) {
            printf("M51_WL_COMMIT_FAIL errno=%d\n", errno);
            close(epoll_fd);
            return 6;
        }
    } else {
        while (!frame_done && wl_display_dispatch(display) >= 0) {}
    }
    if (!frame_done || (!midstream_epoll && wl_display_roundtrip(display) < 0)) {
        printf("M51_WL_COMMIT_FAIL errno=%d\n", errno);
        return 6;
    }
    frame_done = 0;
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    frame = wl_surface_frame(surface);
    wl_callback_add_listener(frame, &frame_listener, NULL);
    wl_surface_commit(surface);
    if (midstream_epoll) {
        if (wait_for_frame_epoll(display, epoll_fd, 2) < 0) {
            printf("M51_WL_COMMIT_FAIL errno=%d\n", errno);
            close(epoll_fd);
            return 6;
        }
        printf("M56_MIDSTREAM_COMPLETE frames=2\n");
        fflush(stdout);
        close(epoll_fd);
    } else {
        while (!frame_done && wl_display_dispatch(display) >= 0) {}
    }
    if (!frame_done) {
        printf("M51_WL_COMMIT_FAIL errno=%d\n", errno);
        return 6;
    }
    usleep(500000);
    printf("M51_WL_SURFACE_COMMIT_OK color=#336699 size=%dx%d\n", width, height);
    printf("M56_WL_SURFACE_READY color=#336699 size=%dx%d\n", width, height);
    if (m58_iteration != NULL)
        printf("M58_WL_SURFACE_READY iteration=%s color=#336699 size=%dx%d\n",
               m58_iteration, width, height);
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

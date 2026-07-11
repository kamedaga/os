#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct buffer {
    struct drm_mode_create_dumb dumb;
    uint32_t fb_id;
    uint32_t *pixels;
};

static int fail(unsigned frame, const char *operation)
{
    fprintf(stderr, "FLIP_EVENT_FAIL frame=%u op=%s errno=%d\n", frame, operation, errno);
    return 1;
}

static drmModeConnector *find_connector(
    int fd,
    drmModeRes *resources,
    drmModeModeInfo *mode,
    uint32_t *crtc_id)
{
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector *connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (connector == NULL) continue;
        if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
            *mode = connector->modes[0];
            if (connector->encoder_id != 0) {
                drmModeEncoder *encoder = drmModeGetEncoder(fd, connector->encoder_id);
                if (encoder != NULL) {
                    *crtc_id = encoder->crtc_id != 0 ? encoder->crtc_id : resources->crtcs[0];
                    drmModeFreeEncoder(encoder);
                }
            }
            if (*crtc_id == 0 && resources->count_crtcs > 0) *crtc_id = resources->crtcs[0];
            return connector;
        }
        drmModeFreeConnector(connector);
    }
    return NULL;
}

static int create_buffer(int fd, uint32_t width, uint32_t height, struct buffer *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->dumb.width = width;
    buffer->dumb.height = height;
    buffer->dumb.bpp = 32;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &buffer->dumb) != 0)
        return fail(0, "CREATE_DUMB");
    struct drm_mode_map_dumb map;
    memset(&map, 0, sizeof(map));
    map.handle = buffer->dumb.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0)
        return fail(0, "MAP_DUMB");
    buffer->pixels = mmap(NULL, buffer->dumb.size, PROT_READ | PROT_WRITE,
        MAP_SHARED, fd, map.offset);
    if (buffer->pixels == MAP_FAILED) return fail(0, "mmap");
    const uint32_t handles[4] = { buffer->dumb.handle, 0, 0, 0 };
    const uint32_t pitches[4] = { buffer->dumb.pitch, 0, 0, 0 };
    const uint32_t offsets[4] = { 0, 0, 0, 0 };
    if (drmModeAddFB2(fd, width, height, DRM_FORMAT_XRGB8888,
            handles, pitches, offsets, &buffer->fb_id, 0) != 0)
        return fail(0, "drmModeAddFB2");
    return 0;
}

static void fill_buffer(struct buffer *buffer, uint32_t color)
{
    const uint32_t stride = buffer->dumb.pitch / 4u;
    for (uint32_t y = 0; y < buffer->dumb.height; y++) {
        for (uint32_t x = 0; x < buffer->dumb.width; x++) {
            buffer->pixels[y * stride + x] = color;
        }
    }
}

static int wait_poll(int fd, unsigned frame)
{
    struct pollfd item = { .fd = fd, .events = POLLIN };
    const int status = poll(&item, 1, 2000);
    if (status != 1 || (item.revents & POLLIN) == 0) {
        fprintf(stderr, "FLIP_EVENT_FAIL frame=%u op=poll status=%d revents=0x%x errno=%d\n",
            frame, status, (unsigned)(uint16_t)item.revents, errno);
        return 1;
    }
    return 0;
}

static int wait_epoll(int epoll_fd, unsigned frame)
{
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    const int status = epoll_wait(epoll_fd, &event, 1, 2000);
    if (status != 1 || (event.events & EPOLLIN) == 0 || event.data.u64 != 0x44524d45564e5400ull) {
        fprintf(stderr, "FLIP_EVENT_FAIL frame=%u op=epoll status=%d events=0x%x data=%llx errno=%d\n",
            frame, status, event.events, (unsigned long long)event.data.u64, errno);
        return 1;
    }
    return 0;
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

int main(int argc, char **argv)
{
    unsigned iterations = 20;
    if (argc == 2) {
        const unsigned long parsed = strtoul(argv[1], NULL, 10);
        if (parsed >= 1 && parsed <= 1000) iterations = (unsigned)parsed;
    }
    const int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return fail(0, "open");
    if (drmSetMaster(fd) != 0) return fail(0, "drmSetMaster");
    drmModeRes *resources = drmModeGetResources(fd);
    if (resources == NULL) return fail(0, "drmModeGetResources");
    drmModeModeInfo mode;
    memset(&mode, 0, sizeof(mode));
    uint32_t crtc_id = 0;
    drmModeConnector *connector = find_connector(fd, resources, &mode, &crtc_id);
    if (connector == NULL || crtc_id == 0) return fail(0, "find-connector");
    struct buffer buffers[2];
    if (create_buffer(fd, mode.hdisplay, mode.vdisplay, &buffers[0]) != 0 ||
        create_buffer(fd, mode.hdisplay, mode.vdisplay, &buffers[1]) != 0)
        return 1;
    fill_buffer(&buffers[0], 0x00104080u);
    uint32_t connector_id = connector->connector_id;
    if (drmModeSetCrtc(fd, crtc_id, buffers[0].fb_id, 0, 0,
            &connector_id, 1, &mode) != 0)
        return fail(0, "drmModeSetCrtc");

    const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) return fail(0, "epoll_create1");
    struct epoll_event interest;
    memset(&interest, 0, sizeof(interest));
    interest.events = EPOLLIN;
    interest.data.u64 = 0x44524d45564e5400ull;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &interest) != 0)
        return fail(0, "epoll_ctl-DRM");

    printf("FLIP_EVENT_BEGIN iterations=%u mode=%ux%u wait=poll+epoll\n",
        iterations, mode.hdisplay, mode.vdisplay);
    fflush(stdout);
    uint32_t previous_sequence = 0;
    uint64_t previous_timestamp = 0;
    const uint64_t start_ns = monotonic_ns();
    for (unsigned frame = 1; frame <= iterations; frame++) {
        struct buffer *next = &buffers[frame & 1u];
        const uint32_t color = frame == iterations ? 0x00ff00ffu :
            (((frame * 37u) & 0xffu) << 16u) |
            (((frame * 67u) & 0xffu) << 8u) |
            ((frame * 97u) & 0xffu);
        fill_buffer(next, color);
        const uint64_t user_data = 0x4d33000000000000ull | frame;
        if (drmModePageFlip(fd, crtc_id, next->fb_id,
                DRM_MODE_PAGE_FLIP_EVENT, (void *)(uintptr_t)user_data) != 0)
            return fail(frame, "drmModePageFlip-EVENT");
        if ((frame & 1u) != 0 ? wait_epoll(epoll_fd, frame) != 0 : wait_poll(fd, frame) != 0)
            return 1;
        struct drm_event_vblank event;
        memset(&event, 0, sizeof(event));
        const ssize_t bytes = read(fd, &event, sizeof(event));
        if (bytes != (ssize_t)sizeof(event) || event.base.type != DRM_EVENT_FLIP_COMPLETE ||
            event.base.length != sizeof(event) || event.user_data != user_data ||
            event.crtc_id != crtc_id || event.tv_usec >= 1000000u ||
            (previous_sequence != 0 && event.sequence != previous_sequence + 1u)) {
            fprintf(stderr,
                "FLIP_EVENT_FAIL frame=%u op=event-fields bytes=%ld type=%u length=%u user=%llx sequence=%u previous=%u crtc=%u usec=%u\n",
                frame, (long)bytes, event.base.type, event.base.length,
                (unsigned long long)event.user_data, event.sequence,
                previous_sequence, event.crtc_id, event.tv_usec);
            return 1;
        }
        const uint64_t timestamp = (uint64_t)event.tv_sec * 1000000ull + event.tv_usec;
        if (previous_timestamp != 0 && timestamp < previous_timestamp) {
            errno = EPROTO;
            return fail(frame, "timestamp-regressed");
        }
        previous_sequence = event.sequence;
        previous_timestamp = timestamp;
        if (frame == 1 || frame == iterations) {
            printf("FLIP_EVENT_FRAME frame=%u sequence=%u user_data=%016llx timestamp=%u.%06u wait=%s color=%06x\n",
                frame, event.sequence, (unsigned long long)event.user_data,
                event.tv_sec, event.tv_usec, (frame & 1u) != 0 ? "epoll" : "poll",
                color & 0xffffffu);
            fflush(stdout);
        }
    }
    const uint64_t elapsed_ns = monotonic_ns() - start_ns;
    const uint64_t fps_milli = elapsed_ns == 0 ? 0 :
        (uint64_t)iterations * 1000000000000ull / elapsed_ns;
    printf("FLIP_EVENT_PASS iterations=%u final_sequence=%u elapsed_ms=%llu fps=%llu.%03llu final_color=ff00ff\n",
        iterations, previous_sequence, (unsigned long long)(elapsed_ns / 1000000ull),
        (unsigned long long)(fps_milli / 1000ull),
        (unsigned long long)(fps_milli % 1000ull));
    fflush(stdout);
    sleep(1);
    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    close(epoll_fd);
    close(fd);
    return 0;
}

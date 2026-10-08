/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DRM_IOCTL_SET_CLIENT_CAP 0x4010640du
#define DRM_IOCTL_MODE_GETRESOURCES 0xc04064a0u
#define DRM_IOCTL_MODE_GETENCODER 0xc01464a6u
#define DRM_IOCTL_MODE_GETCONNECTOR 0xc05064a7u
#define DRM_IOCTL_MODE_GETPROPERTY 0xc04064aau
#define DRM_IOCTL_MODE_CREATE_DUMB 0xc02064b2u
#define DRM_IOCTL_MODE_MAP_DUMB 0xc01064b3u
#define DRM_IOCTL_MODE_GETPLANERESOURCES 0xc01064b5u
#define DRM_IOCTL_MODE_GETPLANE 0xc02064b6u
#define DRM_IOCTL_MODE_ADDFB2 0xc06864b8u
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES 0xc02064b9u
#define DRM_IOCTL_MODE_ATOMIC 0xc03864bcu
#define DRM_IOCTL_MODE_CREATEPROPBLOB 0xc01064bdu

#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_OBJECT_CONNECTOR 0xc0c0c0c0u
#define DRM_OBJECT_CRTC 0xccccccccu
#define DRM_OBJECT_PLANE 0xeeeeeeeeu
#define DRM_ATOMIC_TEST_ONLY 0x100u
#define DRM_ATOMIC_ALLOW_MODESET 0x400u

struct modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
};
struct card_res {
    uint64_t fb_ptr, crtc_ptr, connector_ptr, encoder_ptr;
    uint32_t fb_count, crtc_count, connector_count, encoder_count;
    uint32_t min_width, max_width, min_height, max_height;
};
struct connector {
    uint64_t encoder_ptr, mode_ptr, prop_ptr, value_ptr;
    uint32_t mode_count, prop_count, encoder_count, encoder_id;
    uint32_t connector_id, type, type_id, connection;
    uint32_t width_mm, height_mm, subpixel, pad;
};
struct plane_res { uint64_t ids; uint32_t count, pad; };
struct encoder {
    uint32_t id, type, crtc_id, possible_crtcs, possible_clones;
};
struct plane {
    uint32_t id, crtc_id, fb_id, possible_crtcs, gamma_size, format_count;
    uint64_t formats;
};
struct obj_props {
    uint64_t ids, values;
    uint32_t count, object_id, type, pad;
};
struct property {
    uint64_t values, enums;
    uint32_t id, flags;
    char name[32];
    uint32_t value_count, enum_count;
};
struct client_cap { uint64_t id, value; };
struct create_dumb {
    uint32_t height, width, bpp, flags, handle, pitch;
    uint64_t size;
};
struct map_dumb { uint32_t handle, pad; uint64_t offset; };
struct fb2 {
    uint32_t id, width, height, format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifiers[4];
};
struct create_blob { uint64_t data; uint32_t length, id; };
struct atomic {
    uint32_t flags, object_count;
    uint64_t objects, counts, props, values, reserved, user_data;
};
_Static_assert(sizeof(struct modeinfo) == 68, "DRM mode ABI");
_Static_assert(sizeof(struct card_res) == 64, "DRM resources ABI");
_Static_assert(sizeof(struct connector) == 80, "DRM connector ABI");
_Static_assert(sizeof(struct plane) == 32, "DRM plane ABI");
_Static_assert(sizeof(struct property) == 64, "DRM property ABI");
_Static_assert(sizeof(struct atomic) == 56, "DRM atomic ABI");
struct property_names {
    uint32_t connector_crtc, crtc_mode, crtc_active;
    uint32_t plane_type, plane_fb, plane_crtc;
    uint32_t src_x, src_y, src_w, src_h, crtc_x, crtc_y, crtc_w, crtc_h;
};

static int fail(const char *stage)
{
    fprintf(stderr, "KMS_ATOMIC_FAIL stage=%s errno=%d\n", stage, errno);
    return 1;
}

static int property_id(int fd, uint32_t object, uint32_t type,
                       const char *name, uint64_t *current)
{
    struct obj_props props = {.object_id = object, .type = type};
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props) ||
        !props.count || props.count > 32) return 0;
    uint32_t ids[32] = {0};
    uint64_t values[32] = {0};
    props.ids = (uintptr_t)ids;
    props.values = (uintptr_t)values;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props)) return 0;
    for (uint32_t i = 0; i < props.count; ++i) {
        struct property property = {.id = ids[i]};
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &property)) return 0;
        if (!strncmp(property.name, name, sizeof(property.name))) {
            if (current) *current = values[i];
            return ids[i];
        }
    }
    return 0;
}

static int choose_connector(int fd, const struct card_res *resources,
                            const uint32_t *ids, uint32_t *chosen,
                            struct modeinfo *mode)
{
    for (uint32_t i = 0; i < resources->connector_count; ++i) {
        struct connector connector = {.connector_id = ids[i]};
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) ||
            connector.connection != 1 || !connector.mode_count ||
            connector.mode_count > 32) continue;
        struct modeinfo modes[32] = {0};
        connector.mode_ptr = (uintptr_t)modes;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector)) continue;
        for (uint32_t j = 0; j < connector.mode_count; ++j) {
            if (!modes[j].hdisplay || !modes[j].vdisplay) continue;
            if (!*chosen || (modes[j].type & 8u)) {
                *chosen = ids[i];
                *mode = modes[j];
            }
            if (modes[j].type & 8u) return 0;
        }
    }
    return *chosen ? 0 : -1;
}

static int choose_crtc_and_plane(int fd, const struct card_res *resources,
                                 const uint32_t *crtc_ids, uint32_t crtc_mask,
                                 uint32_t *crtc,
                                 uint32_t *plane_id, struct property_names *names)
{
    struct plane_res planes = {0};
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &planes) ||
        !planes.count || planes.count > 32) return -1;
    uint32_t plane_ids[32] = {0};
    planes.ids = (uintptr_t)plane_ids;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &planes)) return -1;
    for (uint32_t i = 0; i < planes.count; ++i) {
        struct plane plane = {.id = plane_ids[i]};
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) ||
            !plane.format_count || plane.format_count > 128) continue;
        uint32_t formats[128] = {0};
        plane.formats = (uintptr_t)formats;
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane)) continue;
        int xrgb = 0;
        for (uint32_t j = 0; j < plane.format_count; ++j)
            xrgb |= formats[j] == DRM_FORMAT_XRGB8888;
        uint64_t type = 0;
        uint32_t type_id = property_id(fd, plane.id, DRM_OBJECT_PLANE,
                                       "type", &type);
        if (!xrgb || !type_id || type != 1) continue;
        for (uint32_t j = 0; j < resources->crtc_count; ++j) {
            if (!(plane.possible_crtcs & crtc_mask & (1u << j))) continue;
            *crtc = crtc_ids[j];
            *plane_id = plane.id;
            names->plane_type = type_id;
            return 0;
        }
    }
    return -1;
}

int main(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return fail("card-open");
    struct client_cap cap = {.id = 2, .value = 1};
    if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap)) return fail("universal-planes");
    cap.id = 3;
    if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap)) return fail("atomic-cap");
    struct card_res resources = {0};
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) ||
        !resources.crtc_count || !resources.connector_count ||
        resources.crtc_count > 16 || resources.connector_count > 16)
        return fail("resources");
    uint32_t crtc_ids[16] = {0}, connector_ids[16] = {0};
    resources.crtc_ptr = (uintptr_t)crtc_ids;
    resources.connector_ptr = (uintptr_t)connector_ids;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources)) return fail("resource-ids");
    uint32_t connector = 0, crtc = 0, plane = 0;
    struct modeinfo mode = {0};
    struct property_names names = {0};
    if (choose_connector(fd, &resources, connector_ids, &connector, &mode))
        return fail("connected-mode");
    struct connector selected = {.connector_id = connector};
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &selected) ||
        !selected.encoder_count || selected.encoder_count > 16)
        return fail("connector-encoders");
    uint32_t encoder_ids[16] = {0}, crtc_mask = 0;
    selected.encoder_ptr = (uintptr_t)encoder_ids;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &selected))
        return fail("connector-encoder-ids");
    for (uint32_t i = 0; i < selected.encoder_count; ++i) {
        struct encoder encoder = {.id = encoder_ids[i]};
        if (!ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder))
            crtc_mask |= encoder.possible_crtcs;
    }
    if (!crtc_mask || choose_crtc_and_plane(fd, &resources, crtc_ids,
                                           crtc_mask, &crtc, &plane, &names))
        return fail("primary-plane");
#define PROP(field, object, type, name) \
    do { names.field = property_id(fd, object, type, name, NULL); \
         if (!names.field) return fail("property-" name); } while (0)
    PROP(connector_crtc, connector, DRM_OBJECT_CONNECTOR, "CRTC_ID");
    PROP(crtc_mode, crtc, DRM_OBJECT_CRTC, "MODE_ID");
    PROP(crtc_active, crtc, DRM_OBJECT_CRTC, "ACTIVE");
    PROP(plane_fb, plane, DRM_OBJECT_PLANE, "FB_ID");
    PROP(plane_crtc, plane, DRM_OBJECT_PLANE, "CRTC_ID");
    PROP(src_x, plane, DRM_OBJECT_PLANE, "SRC_X");
    PROP(src_y, plane, DRM_OBJECT_PLANE, "SRC_Y");
    PROP(src_w, plane, DRM_OBJECT_PLANE, "SRC_W");
    PROP(src_h, plane, DRM_OBJECT_PLANE, "SRC_H");
    PROP(crtc_x, plane, DRM_OBJECT_PLANE, "CRTC_X");
    PROP(crtc_y, plane, DRM_OBJECT_PLANE, "CRTC_Y");
    PROP(crtc_w, plane, DRM_OBJECT_PLANE, "CRTC_W");
    PROP(crtc_h, plane, DRM_OBJECT_PLANE, "CRTC_H");
#undef PROP
    printf("KMS_ATOMIC_ENUM connector=%u crtc=%u plane=%u mode=%.32s %ux%u@%u\n",
           connector, crtc, plane, mode.name, mode.hdisplay, mode.vdisplay,
           mode.vrefresh);
    printf("KMS_ATOMIC_PROPERTIES connector.CRTC_ID=%u crtc.MODE_ID=%u crtc.ACTIVE=%u plane.FB_ID=%u plane.CRTC_ID=%u plane.SRC_X=%u plane.SRC_Y=%u plane.SRC_W=%u plane.SRC_H=%u plane.CRTC_X=%u plane.CRTC_Y=%u plane.CRTC_W=%u plane.CRTC_H=%u\n",
           names.connector_crtc, names.crtc_mode, names.crtc_active,
           names.plane_fb, names.plane_crtc, names.src_x, names.src_y,
           names.src_w, names.src_h, names.crtc_x, names.crtc_y,
           names.crtc_w, names.crtc_h);
    fflush(stdout);

    struct create_dumb dumb = {.width = mode.hdisplay,
                               .height = mode.vdisplay, .bpp = 32};
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb)) return fail("create-dumb");
    struct map_dumb map = {.handle = dumb.handle};
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) return fail("map-dumb");
    uint32_t *pixels = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, map.offset);
    if (pixels == MAP_FAILED) return fail("mmap-dumb");
    for (uint32_t y = 0; y < dumb.height; ++y)
        for (uint32_t x = 0; x < dumb.width; ++x)
            pixels[(uint64_t)y * dumb.pitch / 4 + x] =
                ((x * 255 / dumb.width) << 16) |
                ((y * 255 / dumb.height) << 8) | 0x80;
    struct fb2 fb = {.width = dumb.width, .height = dumb.height,
                     .format = DRM_FORMAT_XRGB8888,
                     .handles = {dumb.handle}, .pitches = {dumb.pitch}};
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb)) return fail("addfb2");
    struct create_blob blob = {.data = (uintptr_t)&mode,
                               .length = sizeof(mode)};
    if (ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob))
        return fail("create-mode-blob");

    uint32_t objects[] = {connector, crtc, plane};
    uint32_t counts[] = {1, 2, 10};
    uint32_t props[] = {names.connector_crtc, names.crtc_mode,
        names.crtc_active, names.plane_fb, names.plane_crtc,
        names.src_x, names.src_y, names.src_w, names.src_h,
        names.crtc_x, names.crtc_y, names.crtc_w, names.crtc_h};
    uint64_t values[] = {crtc, blob.id, 1, fb.id, crtc,
        0, 0, (uint64_t)mode.hdisplay << 16,
        (uint64_t)mode.vdisplay << 16, 0, 0,
        mode.hdisplay, mode.vdisplay};
    struct atomic atomic = {
        .flags = DRM_ATOMIC_TEST_ONLY | DRM_ATOMIC_ALLOW_MODESET,
        .object_count = 3, .objects = (uintptr_t)objects,
        .counts = (uintptr_t)counts, .props = (uintptr_t)props,
        .values = (uintptr_t)values};
    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic)) return fail("test-only");
    puts("KMS_ATOMIC_TEST_PASS");
    fflush(stdout);
    atomic.flags &= ~DRM_ATOMIC_TEST_ONLY;
    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic)) return fail("commit");
    printf("KMS_ATOMIC_DISPLAY_PASS connector=%u crtc=%u plane=%u fb=%u\n",
           connector, crtc, plane, fb.id);
    fflush(stdout);
    sleep(2);
    close(fd);
    return 0;
}

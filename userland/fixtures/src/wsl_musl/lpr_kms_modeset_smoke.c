#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DRM_IOCTL_SET_MASTER 0x641eu
#define DRM_IOCTL_GET_CAP 0xc010640cu
#define DRM_IOCTL_SET_CLIENT_CAP 0x4010640du
#define DRM_IOCTL_MODE_GETRESOURCES 0xc04064a0u
#define DRM_IOCTL_MODE_GETCRTC 0xc06864a1u
#define DRM_IOCTL_MODE_SETCRTC 0xc06864a2u
#define DRM_IOCTL_MODE_GETENCODER 0xc01464a6u
#define DRM_IOCTL_MODE_GETCONNECTOR 0xc05064a7u
#define DRM_IOCTL_MODE_GETPROPERTY 0xc04064aau
#define DRM_IOCTL_MODE_GETPROPBLOB 0xc01064acu
#define DRM_IOCTL_MODE_PAGE_FLIP 0xc01864b0u
#define DRM_IOCTL_MODE_CREATE_DUMB 0xc02064b2u
#define DRM_IOCTL_MODE_MAP_DUMB 0xc01064b3u
#define DRM_IOCTL_MODE_GETPLANERESOURCES 0xc01064b5u
#define DRM_IOCTL_MODE_GETPLANE 0xc02064b6u
#define DRM_IOCTL_MODE_ADDFB2 0xc06864b8u
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES 0xc02064b9u
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_CAP_TIMESTAMP_MONOTONIC 0x6u
#define DRM_CAP_CRTC_IN_VBLANK_EVENT 0x12u
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2u
#define DRM_CLIENT_CAP_ATOMIC 3u
#define DRM_MODE_OBJECT_PLANE 0xeeeeeeeeu
#define DRM_MODE_PROP_BLOB (1u << 4)
#define DRM_PLANE_TYPE_PRIMARY 1u

struct modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
};

struct card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};

struct crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct modeinfo mode;
};

struct encoder { uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones; };

struct connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders, encoder_id;
    uint32_t connector_id, connector_type, connector_type_id, connection;
    uint32_t mm_width, mm_height, subpixel, pad;
};

struct create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct map_dumb { uint32_t handle, pad; uint64_t offset; };

struct fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifier[4];
};

struct page_flip { uint32_t crtc_id, fb_id, flags, reserved; uint64_t user_data; };
struct plane_res { uint64_t plane_id_ptr; uint32_t count_planes, pad; };
struct plane { uint32_t plane_id, crtc_id, fb_id, possible_crtcs, gamma_size, count_format_types; uint64_t format_type_ptr; };
struct cap { uint64_t capability, value; };
struct obj_properties {
    uint64_t props_ptr, prop_values_ptr;
    uint32_t count_props, obj_id, obj_type, pad;
};
struct property_enum { uint64_t value; char name[32]; };
struct get_property {
    uint64_t values_ptr, enum_blob_ptr;
    uint32_t prop_id, flags;
    char name[32];
    uint32_t count_values, count_enum_blobs;
};
struct get_blob { uint32_t blob_id, length; uint64_t data; };
struct format_modifier {
    uint64_t formats;
    uint32_t offset, pad;
    uint64_t modifier;
};
struct format_blob {
    uint32_t version, flags, count_formats, formats_offset;
    uint32_t count_modifiers, modifiers_offset, format, pad;
    struct format_modifier modifier;
};

struct buffer { struct create_dumb dumb; struct fb_cmd2 fb; uint32_t *pixels; };

static int fail(const char *operation)
{
    fprintf(stderr, "KMS_FAIL op=%s errno=%d\n", operation, errno);
    return 1;
}

static int create_buffer(int fd, uint32_t width, uint32_t height, struct buffer *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->dumb.width = width;
    buffer->dumb.height = height;
    buffer->dumb.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &buffer->dumb) != 0) return fail("CREATE_DUMB");
    struct map_dumb map = { .handle = buffer->dumb.handle };
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) return fail("MAP_DUMB");
    buffer->pixels = mmap(NULL, buffer->dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    if (buffer->pixels == MAP_FAILED) return fail("mmap");
    buffer->fb.width = width;
    buffer->fb.height = height;
    buffer->fb.pixel_format = DRM_FORMAT_XRGB8888;
    buffer->fb.handles[0] = buffer->dumb.handle;
    buffer->fb.pitches[0] = buffer->dumb.pitch;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &buffer->fb) != 0) return fail("ADDFB2");
    return 0;
}

static void fill_gradient(struct buffer *buffer, int second)
{
    const uint32_t width = buffer->dumb.width, height = buffer->dumb.height;
    const uint32_t stride = buffer->dumb.pitch / 4u;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t red = second ? 255u - x * 255u / (width - 1u) : x * 255u / (width - 1u);
            const uint32_t green = second ? 255u - y * 255u / (height - 1u) : y * 255u / (height - 1u);
            buffer->pixels[y * stride + x] = (red << 16) | (green << 8) | (second ? 0xc0u : 0x40u);
        }
    }
    const uint32_t colors[4] = {
        second ? 0x00ffffu : 0xff0000u, second ? 0xff00ffu : 0x00ff00u,
        second ? 0xffff00u : 0x0000ffu, second ? 0x000000u : 0xffffffu,
    };
    for (uint32_t y = 0; y < 32; y++) for (uint32_t x = 0; x < 32; x++) {
        buffer->pixels[y * stride + x] = colors[0];
        buffer->pixels[y * stride + width - 32u + x] = colors[1];
        buffer->pixels[(height - 32u + y) * stride + x] = colors[2];
        buffer->pixels[(height - 32u + y) * stride + width - 32u + x] = colors[3];
    }
}

int main(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return fail("open");
    if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0) != 0) return fail("SET_MASTER");
    struct cap cap = { .capability = DRM_CAP_TIMESTAMP_MONOTONIC };
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) != 0 || cap.value != 1) return fail("GET_CAP-monotonic");
    cap.capability = DRM_CAP_CRTC_IN_VBLANK_EVENT;
    cap.value = 0;
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) != 0 || cap.value != 1) return fail("GET_CAP-crtc-event");
    cap.capability = DRM_CLIENT_CAP_ATOMIC;
    cap.value = 1;
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) == 0 || errno != EOPNOTSUPP) return fail("SET_CLIENT_CAP-atomic");
    cap.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
    if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) != 0) return fail("SET_CLIENT_CAP-universal");
    struct card_res resources;
    memset(&resources, 0, sizeof(resources));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) != 0 || resources.count_crtcs != 1 || resources.count_connectors != 1 || resources.count_encoders != 1) return fail("GETRESOURCES-count");
    uint32_t crtc_id = 0, connector_id = 0, encoder_id = 0;
    resources.crtc_id_ptr = (uint64_t)(uintptr_t)&crtc_id;
    resources.connector_id_ptr = (uint64_t)(uintptr_t)&connector_id;
    resources.encoder_id_ptr = (uint64_t)(uintptr_t)&encoder_id;
    resources.count_crtcs = resources.count_connectors = resources.count_encoders = 1;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) != 0 || crtc_id == 0 || connector_id == 0 || encoder_id == 0) return fail("GETRESOURCES-data");
    struct connector connector;
    memset(&connector, 0, sizeof(connector));
    connector.connector_id = connector_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) != 0 || connector.count_modes != 1 || connector.connection != 1) return fail("GETCONNECTOR-count");
    struct modeinfo mode;
    uint32_t connector_encoder = 0;
    memset(&mode, 0, sizeof(mode));
    connector.modes_ptr = (uint64_t)(uintptr_t)&mode;
    connector.encoders_ptr = (uint64_t)(uintptr_t)&connector_encoder;
    connector.count_modes = connector.count_encoders = 1;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) != 0 || mode.hdisplay == 0 || mode.vdisplay == 0 || connector_encoder != encoder_id) return fail("GETCONNECTOR-data");
    struct encoder encoder = { .encoder_id = encoder_id };
    if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) != 0 || encoder.possible_crtcs != 1) return fail("GETENCODER");
    struct crtc get_crtc = { .crtc_id = crtc_id };
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &get_crtc) != 0) return fail("GETCRTC");
    struct plane_res plane_resources;
    memset(&plane_resources, 0, sizeof(plane_resources));
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_resources) != 0 || plane_resources.count_planes != 1) return fail("GETPLANERESOURCES-count");
    uint32_t plane_id = 0;
    plane_resources.plane_id_ptr = (uint64_t)(uintptr_t)&plane_id;
    plane_resources.count_planes = 1;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_resources) != 0 || plane_id == 0) return fail("GETPLANERESOURCES-data");
    uint32_t plane_format = 0;
    struct plane plane = { .plane_id = plane_id, .count_format_types = 1, .format_type_ptr = (uint64_t)(uintptr_t)&plane_format };
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) != 0 || plane_format != DRM_FORMAT_XRGB8888) return fail("GETPLANE");
    struct obj_properties object_props = { .obj_id = plane_id, .obj_type = DRM_MODE_OBJECT_PLANE };
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &object_props) != 0 || object_props.count_props != 2) return fail("OBJ_GETPROPERTIES-count");
    uint32_t prop_ids[2] = {0};
    uint64_t prop_values[2] = {0};
    object_props.props_ptr = (uint64_t)(uintptr_t)prop_ids;
    object_props.prop_values_ptr = (uint64_t)(uintptr_t)prop_values;
    object_props.count_props = 2;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &object_props) != 0 ||
        prop_values[0] != DRM_PLANE_TYPE_PRIMARY || prop_values[1] == 0) return fail("OBJ_GETPROPERTIES-data");
    struct property_enum enums[3];
    memset(enums, 0, sizeof(enums));
    struct get_property property = {
        .enum_blob_ptr = (uint64_t)(uintptr_t)enums,
        .prop_id = prop_ids[0],
        .count_enum_blobs = 3,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &property) != 0 ||
        strcmp(property.name, "type") != 0 || property.count_enum_blobs != 3) return fail("GETPROPERTY-type");
    memset(&property, 0, sizeof(property));
    property.prop_id = prop_ids[1];
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &property) != 0 ||
        strcmp(property.name, "IN_FORMATS") != 0 || (property.flags & DRM_MODE_PROP_BLOB) == 0) return fail("GETPROPERTY-formats");
    uint8_t blob_data[128];
    memset(blob_data, 0, sizeof(blob_data));
    struct get_blob blob = {
        .blob_id = (uint32_t)prop_values[1],
        .length = sizeof(blob_data),
        .data = (uint64_t)(uintptr_t)blob_data,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) != 0 || blob.length != sizeof(struct format_blob)) return fail("GETPROPBLOB");
    const struct format_blob *formats = (const struct format_blob *)blob_data;
    if (formats->version != 1 || formats->count_formats != 1 ||
        formats->format != DRM_FORMAT_XRGB8888 || formats->count_modifiers != 1 ||
        formats->modifier.formats != 1 || formats->modifier.modifier != 0) return fail("GETPROPBLOB-data");
    struct buffer first, second;
    if (create_buffer(fd, mode.hdisplay, mode.vdisplay, &first) != 0 || create_buffer(fd, mode.hdisplay, mode.vdisplay, &second) != 0) return 2;
    fill_gradient(&first, 0);
    fill_gradient(&second, 1);
    struct crtc set_crtc;
    memset(&set_crtc, 0, sizeof(set_crtc));
    set_crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&connector_id;
    set_crtc.count_connectors = 1;
    set_crtc.crtc_id = crtc_id;
    set_crtc.fb_id = first.fb.fb_id;
    set_crtc.mode_valid = 1;
    set_crtc.mode = mode;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set_crtc) != 0) return fail("SETCRTC");
    printf("KMS_FRAME1_READY color=ff0000 mode=%ux%u\n", mode.hdisplay, mode.vdisplay);
    fflush(stdout);
    sleep(2);
    struct page_flip flip = { .crtc_id = crtc_id, .fb_id = second.fb.fb_id };
    if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) != 0) return fail("PAGE_FLIP");
    printf("KMS_FRAME2_READY color=00ffff\n");
    fflush(stdout);
    sleep(1);
    printf("KMS_MODESET_OK connector=%u crtc=%u plane=%u first_fb=%u second_fb=%u\n", connector_id, crtc_id, plane_id, first.fb.fb_id, second.fb.fb_id);
    close(fd);
    return 0;
}

/* SPDX-License-Identifier: MIT */
#include "../kobox2/linux-sandbox/kobox/boot/drm_query.h"
#include "../userland/gpud/drm_reply.h"
#include "../userland/gpud/drm_translate.h"
#include "../userland/kobox2_adapter/gpu_query.h"
#include "../userland/kobox2_adapter/gpu_queue.h"
#include <kobox2/gpu_session.h>
#include <kobox2/virtqueue_x86_64.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned int calls;
static int cursor(struct kobox_linux_drm_file *,
                  const struct kobox_linux_drm_cursor *);

static int version(struct kobox_linux_drm_file *file,
                   const size_t *capacity,
                   struct kobox_linux_drm_version *out) {
    assert(file);
    ++calls;
    if (capacity[0] > sizeof(out->name) || capacity[1] > sizeof(out->date) ||
        capacity[2] > sizeof(out->description))
        return -EINVAL;
    struct kobox_linux_drm_version result = {.major = 5,
                                             .minor = 6,
                                             .patchlevel = 7,
                                             .name_length = 17,
                                             .date_length = 0,
                                             .description_length = 4};
    memcpy(result.name, "query-unit-driver", capacity[0] < 17 ? capacity[0] : 17);
    memcpy(result.description, "test", capacity[2] < 4 ? capacity[2] : 4);
    *out = result;
    return 0;
}

static int get_cap(struct kobox_linux_drm_file *file, uint64_t capability, uint64_t *value) {
    assert(file);
    ++calls;
    if (capability == UINT64_MAX)
        return -EINVAL;
    if (capability == 6)
        return -ENODEV;
    *value = UINT64_C(0x1234567887654321);
    return 0;
}

static int set_client_cap(struct kobox_linux_drm_file *file,
    uint64_t capability, uint64_t value) {
    (void)file;
    ++calls;
    return capability && value <= 1 ? 0 : -EINVAL;
}

static int get_magic(struct kobox_linux_drm_file *file, uint32_t *magic) {
    assert(file && magic);
    *magic = 0x1234;
    return 0;
}

static int auth_magic(struct kobox_linux_drm_file *file, uint32_t magic) {
    assert(file);
    return magic == 0x1234 ? 0 : -EACCES;
}

static int master(struct kobox_linux_drm_file *file, bool acquire) {
    assert(file);
    ++calls;
    return acquire ? 0 : -EPERM;
}

static int resources(struct kobox_linux_drm_file *file,
    uint32_t *fbs, size_t fb_capacity,
    uint32_t *crtcs, size_t crtc_capacity,
    uint32_t *connectors, size_t connector_capacity,
    uint32_t *encoders, size_t encoder_capacity,
    struct kobox_linux_drm_resources *out) {
    assert(file && out);
    ++calls;
    if (fb_capacity) fbs[0] = 11;
    if (crtc_capacity) crtcs[0] = 12;
    if (connector_capacity) connectors[0] = 13;
    if (encoder_capacity) encoders[0] = 14;
    *out = (struct kobox_linux_drm_resources){
        .fb_count = 1,
        .crtc_count = 1,
        .connector_count = 1,
        .encoder_count = 1,
        .min_width = 0,
        .max_width = 8192,
        .min_height = 0,
        .max_height = 8192,
    };
    return 0;
}

static int connector(struct kobox_linux_drm_file *file, uint32_t connector_id,
    struct kobox_linux_drm_mode *modes, size_t mode_capacity,
    struct kobox_linux_drm_property_value *properties, size_t property_capacity,
    uint32_t *encoders, size_t encoder_capacity,
    struct kobox_linux_drm_connector *out) {
    assert(file && connector_id && out);
    if (mode_capacity)
        modes[0] = (struct kobox_linux_drm_mode){.hdisplay = 1024,
            .vdisplay = 768};
    if (property_capacity)
        properties[0] = (struct kobox_linux_drm_property_value){
            .property_id = 3, .value = 4};
    if (encoder_capacity)
        encoders[0] = 5;
    *out = (struct kobox_linux_drm_connector){
        .encoder_id = 5, .connector_id = connector_id,
        .connection = 1, .mode_count = 1,
        .property_count = 1, .encoder_count = 1};
    return 0;
}

static int encoder(struct kobox_linux_drm_file *file, uint32_t encoder_id,
    struct kobox_linux_drm_encoder *out) {
    assert(file && encoder_id && out);
    *out = (struct kobox_linux_drm_encoder){
        .encoder_id = encoder_id, .crtc_id = 6, .possible_crtcs = 1};
    return 0;
}

static int get_crtc(struct kobox_linux_drm_file *file, uint32_t crtc_id,
    struct kobox_linux_drm_crtc *out) {
    assert(file && crtc_id && out);
    *out = (struct kobox_linux_drm_crtc){
        .crtc_id = crtc_id,
        .fb_id = 29,
        .gamma_size = 256,
        .mode_valid = 1,
        .mode = {.clock = 25175, .hdisplay = 640, .vdisplay = 480},
    };
    memcpy(out->mode.name, "640x480", 8);
    return 0;
}

static int set_crtc(struct kobox_linux_drm_file *file, uint32_t crtc_id,
    uint32_t fb_id, uint32_t x, uint32_t y, const uint32_t *connectors,
    size_t connector_count, const struct kobox_linux_drm_mode *mode) {
    assert(file && crtc_id && fb_id && !x && !y && connectors &&
        connector_count == 1 && mode);
    return 0;
}

static int page_flip(struct kobox_linux_drm_file *file, uint32_t crtc_id,
    uint32_t fb_id, uint32_t flags, uint32_t sequence, uint64_t event_token) {
    assert(file && crtc_id && fb_id && flags && !sequence && event_token);
    return 0;
}

static int dirty_fb(struct kobox_linux_drm_file *file, uint32_t fb_id,
    uint32_t flags, uint32_t color,
    const struct kobox_linux_drm_rectangle *rectangles,
    size_t rectangle_count) {
    if (!fb_id) {
        assert(file && !flags && !color && !rectangles && !rectangle_count);
        ++calls;
        return -ENOENT;
    }
    assert(file && fb_id == 29 && !flags && color == UINT32_C(0x12345678) &&
        rectangles && rectangle_count == 1 && rectangles[0].x1 == 3 &&
        rectangles[0].y1 == 5 && rectangles[0].x2 == 73 &&
        rectangles[0].y2 == 59);
    ++calls;
    return 0;
}

static int create_dumb(struct kobox_linux_drm_file *file,
    struct kobox_linux_drm_dumb_buffer *buffer) {
    assert(file && buffer && buffer->height && buffer->width &&
        buffer->bits_per_pixel == 32 && !buffer->handle && !buffer->pitch &&
        !buffer->size);
    ++calls;
    buffer->handle = 27;
    buffer->pitch = buffer->width * 4;
    buffer->size = 4096;
    return 0;
}

static int add_fb(struct kobox_linux_drm_file *file,
    struct kobox_linux_drm_fb *framebuffer) {
    assert(file && framebuffer && !framebuffer->fb_id &&
        framebuffer->width == 79 && framebuffer->height == 61 &&
        framebuffer->pitch == 79 * 4 && framebuffer->bits_per_pixel == 32 &&
        framebuffer->depth == 24 && framebuffer->handle == 27);
    framebuffer->fb_id = 29;
    return 0;
}

static int remove_fb(struct kobox_linux_drm_file *file, uint32_t fb_id) {
    assert(file && fb_id == 29);
    return 0;
}

static int add_fb2(struct kobox_linux_drm_file *file,
    struct kobox_linux_drm_fb2 *framebuffer) {
    assert(file && framebuffer && !framebuffer->fb_id &&
        framebuffer->handles[0]);
    framebuffer->fb_id = 31;
    return 0;
}

static int object_properties(struct kobox_linux_drm_file *file,
    uint32_t object_id, uint32_t object_type,
    struct kobox_linux_drm_property_value *properties,
    size_t property_capacity, uint32_t *property_count) {
    assert(file && object_id == 13 &&
        object_type == GPUD_DRM_MODE_OBJECT_CONNECTOR && property_count &&
        (!property_capacity || properties));
    for (size_t index = 0; index < property_capacity && index < 3; ++index)
        properties[index] = (struct kobox_linux_drm_property_value) {
            .property_id = 101 + index,
            .value = 201 + index,
        };
    *property_count = 3;
    return 0;
}

static int poll_events(struct kobox_linux_drm_file *file,
    uint32_t requested, uint32_t *ready) {
    assert(file && ready && !(requested & ~1u));
    *ready = 0;
    return 0;
}

static int read_events(struct kobox_linux_drm_file *file,
    void *output, size_t capacity, size_t *bytes) {
    assert(file && output && capacity && bytes);
    *bytes = 0;
    return 0;
}

static int gem_close(struct kobox_linux_drm_file *file, uint32_t handle) {
    assert(file && handle);
    ++calls;
    return 0;
}

static int virtgpu_getparam(
    struct kobox_linux_drm_file *file, uint64_t parameter, uint64_t *value) {
    assert(file && value);
    ++calls;
    *value = parameter + 100;
    return 0;
}

static int virtgpu_get_caps(struct kobox_linux_drm_file *file,
    uint32_t capset_id, uint32_t capset_version, void *output, size_t capacity) {
    assert(file && output && capacity);
    ++calls;
    memset(output, 0, capacity);
    ((unsigned char *)output)[0] = (unsigned char)capset_id;
    ((unsigned char *)output)[1] = (unsigned char)capset_version;
    return 0;
}

static int virtgpu_context_init(struct kobox_linux_drm_file *file,
    uint32_t mask, uint32_t capset_id, uint32_t ring_count,
    uint64_t poll_ring_mask, const void *debug_name, size_t debug_name_size) {
    assert(file && mask && capset_id <= 63 && ring_count <= 64);
    assert(!poll_ring_mask && !debug_name && !debug_name_size);
    ++calls;
    return 0;
}

static int syncobj_create(struct kobox_linux_drm_file *file, uint32_t flags,
    uint32_t *handle) {
    assert(file && !flags && handle);
    *handle = 1;
    return 0;
}

static int syncobj_destroy(struct kobox_linux_drm_file *file, uint32_t handle) {
    assert(file && handle);
    return 0;
}

static int syncobj_wait(struct kobox_linux_drm_file *file, const void *handles,
    size_t count, int64_t timeout, uint32_t flags, uint64_t deadline,
    uint32_t *first) {
    assert(file && handles && count && timeout >= -1 && !(flags & ~15u) &&
        !deadline && first);
    *first = 0;
    return 0;
}

static int syncobj_array(struct kobox_linux_drm_file *file, const void *handles,
    size_t count, bool signal) {
    assert(file && handles && count);
    (void)signal;
    return 0;
}

static size_t fence_results, fence_taken;
static int take_fence(struct kobox_linux_drm_service *service,
    struct kobox_drm_fence_result *result) {
    assert(service && result);
    if (fence_taken == fence_results)
        return 0;
    *result = (struct kobox_drm_fence_result) {
        .session = 71, .correlation = 100 + fence_taken,
        .status = fence_taken % 2 ? -EIO : 1,
    };
    ++fence_taken;
    return 1;
}

static int virtgpu_execbuffer(struct kobox_linux_drm_service *service,
    uint64_t cookie, uint64_t session, uint64_t correlation,
    uint32_t flags, uint32_t ring_index,
    const void *command, size_t command_size,
    const void *handles, size_t handle_count,
    const void *input_syncobjs, size_t input_count,
    const void *output_syncobjs, size_t output_count) {
    (void)service; (void)cookie; (void)correlation;
    assert(session && command && command_size);
    assert(!flags && !ring_index);
    assert((handles && handle_count) || (!handles && !handle_count));
    assert(!input_syncobjs && !input_count && !output_syncobjs && !output_count);
    assert(command_size != 4 || !memcmp(command, "\0\0\0\0", 4));
    assert(handle_count != 1 || ((const unsigned char *)handles)[0] == 7);
    ++calls;
    return 0;
}

static int virtgpu_resource_create(struct kobox_linux_drm_file *file,
    struct kobox_linux_virtgpu_resource_create *resource) {
    assert(file && resource);
    ++calls;
    resource->bo_handle = 21;
    resource->resource_handle = 22;
    return 0;
}

static int virtgpu_resource_info(struct kobox_linux_drm_file *file,
    struct kobox_linux_virtgpu_resource_info *resource) {
    assert(file && resource);
    ++calls;
    resource->resource_handle = 31;
    resource->size = 4096;
    resource->blob_memory = 1;
    return 0;
}

static int virtgpu_transfer(struct kobox_linux_drm_file *file, bool from_host,
    const struct kobox_linux_virtgpu_transfer *transfer) {
    assert(file && transfer);
    (void)from_host;
    ++calls;
    return 0;
}

static int virtgpu_wait(
    struct kobox_linux_drm_file *file, uint32_t handle, uint32_t flags) {
    assert(file && handle && flags);
    ++calls;
    return 0;
}

static int virtgpu_map(struct kobox_linux_drm_service *service,
    uint64_t cookie, uint32_t handle, uint32_t rights,
    uint64_t *pages, size_t capacity,
    struct kobox_linux_drm_service_mapping *result) {
    assert(service && cookie == 3 && handle == 27 &&
        rights == (KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE) &&
        pages && capacity && result);
    pages[0] = 11;
    *result = (struct kobox_linux_drm_service_mapping) {
        .mapping_id = 4096,
        .length = 4096,
        .page_count = 1,
    };
    return 0;
}

static int prime_export(struct kobox_linux_drm_service *service,
    uint64_t cookie, uint32_t handle, uint32_t flags,
    uint64_t *pages, size_t capacity,
    struct kobox_linux_drm_service_prime *result) {
    (void)service;
    (void)cookie;
    (void)handle;
    (void)flags;
    (void)pages;
    (void)capacity;
    (void)result;
    return -EOPNOTSUPP;
}

static int prime_import(struct kobox_linux_drm_service *service,
    uint64_t cookie, uint64_t token, uint32_t *handle) {
    (void)service;
    (void)cookie;
    (void)token;
    (void)handle;
    return -EOPNOTSUPP;
}

struct ph_image ph_core;
static unsigned char native_vmo[PH_GPU_QUERY_VMO_SIZE];
static _Alignas(16) unsigned char queue_vmo[GPUD_GPU_CHANNEL_SIZE];
static _Alignas(4096) unsigned char private_aux[PH_GPU_QUERY_AUX_REUSE_BYTES];
static _Alignas(4096) unsigned char large_aux[2 * PH_GPU_QUERY_AUX_REUSE_BYTES];
static unsigned int aux_mapped;
static int aux_map_failure, aux_unmap_failure;
static void *active_vmo = native_vmo;
static size_t active_size = sizeof(native_vmo);
static struct ph_ipc_packet sent;
static unsigned int mapped, unmapped, aux_unmapped, sends;
static uint64_t mock_cookie;
static int mock_open_error, mock_close_error;
static int mock_send_error;
static unsigned int mock_opens, mock_closes, mock_unmaps;
static uint64_t mock_unmapped_id;
static uint32_t mock_open_node;

static int file_open(
    struct kobox_linux_drm_service *owner, uint32_t node_type, uint64_t *cookie) {
    assert(owner && (node_type == KB2_GPU_NODE_PRIMARY || node_type == KB2_GPU_NODE_RENDER));
    ++mock_opens;
    mock_open_node = node_type;
    if (mock_open_error)
        return mock_open_error;
    *cookie = ++mock_cookie;
    return 0;
}

static int file_lookup(struct kobox_linux_drm_service *owner,
                       uint64_t cookie,
                       struct kobox_linux_drm_file **file) {
    assert(owner && cookie && cookie <= mock_cookie);
    *file = (struct kobox_linux_drm_file *)(void *)&calls;
    return 0;
}

static int file_close(struct kobox_linux_drm_service *owner, uint64_t cookie) {
    assert(owner && cookie && cookie <= mock_cookie);
    ++mock_closes;
    return mock_close_error;
}

static int file_unmap(struct kobox_linux_drm_service *owner, uint64_t mapping_id) {
    assert(owner && mapping_id);
    ++mock_unmaps;
    mock_unmapped_id = mapping_id;
    return 0;
}

void *ph_image_lookup(void *image, const char *name) {
    assert(image == &ph_core);
    struct kobox_drm_query_api api = {
        .version = version,
        .get_cap = get_cap,
        .set_client_cap = set_client_cap,
        .master = master,
        .get_magic = get_magic,
        .auth_magic = auth_magic,
        .resources = resources,
        .connector = connector,
        .encoder = encoder,
        .get_crtc = get_crtc,
        .set_crtc = set_crtc,
        .page_flip = page_flip,
        .cursor = cursor,
        .dirty_fb = dirty_fb,
        .create_dumb = create_dumb,
        .add_fb = add_fb,
        .remove_fb = remove_fb,
        .add_fb2 = add_fb2,
        .object_properties = object_properties,
        .poll_events = poll_events,
        .read_events = read_events,
        .gem_close = gem_close,
        .prime_export = prime_export,
        .prime_import = prime_import,
        .syncobj_create = syncobj_create,
        .syncobj_destroy = syncobj_destroy,
        .syncobj_wait = syncobj_wait,
        .syncobj_array = syncobj_array,
        .virtgpu_getparam = virtgpu_getparam,
        .virtgpu_get_caps = virtgpu_get_caps,
        .virtgpu_context_init = virtgpu_context_init,
        .virtgpu_execbuffer = virtgpu_execbuffer,
        .virtgpu_resource_create = virtgpu_resource_create,
        .virtgpu_resource_info = virtgpu_resource_info,
        .virtgpu_transfer = virtgpu_transfer,
        .virtgpu_wait = virtgpu_wait,
        .virtgpu_map = virtgpu_map,
    };
    void *symbol = NULL;
    if (!strcmp(name, "kobox_linux_drm_version"))
        memcpy(&symbol, &api.version, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_get_cap"))
        memcpy(&symbol, &api.get_cap, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_set_client_cap"))
        memcpy(&symbol, &api.set_client_cap, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_master"))
        memcpy(&symbol, &api.master, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_get_magic"))
        memcpy(&symbol, &api.get_magic, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_auth_magic"))
        memcpy(&symbol, &api.auth_magic, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_resources"))
        memcpy(&symbol, &api.resources, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_connector"))
        memcpy(&symbol, &api.connector, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_encoder"))
        memcpy(&symbol, &api.encoder, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_get_crtc"))
        memcpy(&symbol, &api.get_crtc, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_set_crtc"))
        memcpy(&symbol, &api.set_crtc, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_page_flip"))
        memcpy(&symbol, &api.page_flip, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_cursor"))
        memcpy(&symbol, &api.cursor, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_dirty_fb"))
        memcpy(&symbol, &api.dirty_fb, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_create_dumb"))
        memcpy(&symbol, &api.create_dumb, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_add_fb"))
        memcpy(&symbol, &api.add_fb, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_remove_fb"))
        memcpy(&symbol, &api.remove_fb, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_add_fb2"))
        memcpy(&symbol, &api.add_fb2, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_object_properties"))
        memcpy(&symbol, &api.object_properties, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_poll_events"))
        memcpy(&symbol, &api.poll_events, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_read_events"))
        memcpy(&symbol, &api.read_events, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_gem_close"))
        memcpy(&symbol, &api.gem_close, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_service_prime_export"))
        memcpy(&symbol, &api.prime_export, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_service_prime_import"))
        memcpy(&symbol, &api.prime_import, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_syncobj_create"))
        memcpy(&symbol, &api.syncobj_create, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_syncobj_destroy"))
        memcpy(&symbol, &api.syncobj_destroy, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_syncobj_wait"))
        memcpy(&symbol, &api.syncobj_wait, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_syncobj_array"))
        memcpy(&symbol, &api.syncobj_array, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_virtgpu_getparam"))
        memcpy(&symbol, &api.virtgpu_getparam, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_virtgpu_get_caps"))
        memcpy(&symbol, &api.virtgpu_get_caps, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_virtgpu_context_init"))
        memcpy(&symbol, &api.virtgpu_context_init, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_service_execbuffer"))
        memcpy(&symbol, &api.virtgpu_execbuffer, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_virtgpu_resource_create"))
        memcpy(&symbol, &api.virtgpu_resource_create, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_virtgpu_resource_info"))
        memcpy(&symbol, &api.virtgpu_resource_info, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_virtgpu_transfer"))
        memcpy(&symbol, &api.virtgpu_transfer, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_virtgpu_wait"))
        memcpy(&symbol, &api.virtgpu_wait, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_service_map"))
        memcpy(&symbol, &api.virtgpu_map, sizeof(symbol));
    struct ph_gpu_session_service files = {
        .open = file_open, .file = file_lookup, .close = file_close,
        .unmap = file_unmap, .take_fence = take_fence};
    if (!strcmp(name, "kobox_linux_drm_service_open"))
        memcpy(&symbol, &files.open, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_service_file"))
        memcpy(&symbol, &files.file, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_service_close"))
        memcpy(&symbol, &files.close, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_service_unmap"))
        memcpy(&symbol, &files.unmap, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_service_take_fence"))
        memcpy(&symbol, &files.take_fence, sizeof(symbol));
    return symbol;
}

long pacha_syscall2(uint64_t number, uint64_t a0, uint64_t a1) {
    if (number == PACHA_FD_SYSCALL_GET_INFO) {
        assert(a0 == 16);
        *(struct pacha_fd_info *)(uintptr_t)a1 =
            (struct pacha_fd_info){.kind = PACHA_FD_KIND_VMO,
                                   .size = active_size,
                                   .rights = PH_GPU_QUERY_RIGHTS,
                                   .flags = PACHA_FD_FLAG_CLOEXEC};
    } else {
        assert(number == PACHA_VM_SYSCALL_MUNMAP);
        if (a0 == (uintptr_t)private_aux || a0 == (uintptr_t)large_aux) {
            assert(a1 <= (a0 == (uintptr_t)private_aux ? sizeof(private_aux) : sizeof(large_aux)));
            if (aux_unmap_failure) return PACHA_SYSCALL_ERR_INVALID;
            ++aux_unmapped;
        } else {
            assert(a0 == (uintptr_t)active_vmo && a1 == active_size);
            ++unmapped;
        }
    }
    return 0;
}

long pacha_syscall3(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2) {
    if (number == PACHA_FD_SYSCALL_EVENTFD_CREATE) {
        assert(!a0 && a1 && !a2);
        return 17;
    }
    if (number == PACHA_FD_SYSCALL_READ) {
        assert(a0 == 17 && a1 && a2 == sizeof(uint64_t));
        return PACHA_SYSCALL_ERR_NOT_READY;
    }
    assert(number == PACHA_FD_SYSCALL_WRITE);
    assert(a0 == 17 && a1 && a2 == sizeof(uint64_t));
    return (long)a2;
}

long pacha_syscall1(uint64_t number, uint64_t a0) {
    (void)number;
    (void)a0;
    return 0;
}

long pacha_syscall5(uint64_t number, uint64_t a0, uint64_t a1,
    uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)number;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    return -1;
}

long pacha_syscall6(
    uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    assert(number == PACHA_VM_SYSCALL_MMAP && !a1 &&
           a3 == (PACHA_PROT_READ | PACHA_PROT_WRITE) && !a5);
    if (!a0) {
        assert(a2 <= sizeof(large_aux) &&
               a4 == (PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS));
        if (aux_map_failure) return PACHA_SYSCALL_ERR_ALLOC;
        ++aux_mapped;
        return (long)(uintptr_t)(a2 <= sizeof(private_aux) ? private_aux : large_aux);
    }
    assert(a0 == 16 && a2 == active_size && a4 == PACHA_MMAP_SHARED);
    ++mapped;
    return (long)(uintptr_t)active_vmo;
}

int ph_ipc_send(struct ph_ipc *ipc, struct ph_ipc_packet *packet) {
    assert(ipc->generation == packet->generation);
    sent = *packet;
    ++sends;
    return mock_send_error;
}

static void native_transport_case(uint64_t capability, int expected_error) {
    static struct ph_gpu_query query;
    memset(&query, 0, sizeof(query));
    memset(native_vmo, 0xa5, sizeof(native_vmo));
    mapped = unmapped = aux_unmapped = sends = 0;
    struct ph_lifecycle_service service;
    struct gpud_drm_binding binding = {.generation = 9, .frontend_handle = 8, .session_id = 7};
    gpud_drm_ioctl_request_t request = {
        .handle = 8, .request = UINT64_C(0xc010640c), .arg_size = 16, .data_size = 16};
    memcpy(request.data, &capability, sizeof(capability));
    struct gpud_drm_translation encoded;
    assert(!gpud_drm_ioctl_encode(&encoded, &binding, &request, 1));
    kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
                                                .opcode = KB2_GPU_OPCODE_COMMAND,
                                                .generation = 9,
                                                .correlation_id = 99,
                                                .payload_length = encoded.command_size};
    size_t size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + encoded.command_size;
    assert(!kb2_protocol_message_envelope_encode(native_vmo, size, &envelope));
    memcpy(native_vmo + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, encoded.command, encoded.command_size);
    struct ph_ipc_packet packet = {
        .operation = PH_GPU_QUERY_OPERATION,
        .generation = 9,
        .correlation = 99,
        .value = size,
        .fd_count = 1,
        .fds = {{.fd = 16, .rights = PH_GPU_QUERY_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC}}};
    assert(!ph_gpu_query_init(&query, 9, 7, &service));
    packet.fds[0].rights ^= PACHA_FD_RIGHT_MAP_WRITE;
    assert(service.prepare(service.context, &packet) == -EACCES && !mapped);
    packet.fds[0].rights ^= PACHA_FD_RIGHT_MAP_WRITE;
    assert(!service.prepare(service.context, &packet) && mapped == 1);
    memset(native_vmo, 0xcc, PH_GPU_QUERY_PAGE); /* Peer mutation cannot alter the private plan. */
    assert(!service.dispatch(service.context, &calls));
    struct ph_ipc ipc = {.generation = 9};
    assert(service.complete(service.context, &ipc) == (capability == 6 ? -ENODEV : 0));
    assert(sends == 1 && sent.generation == 9 && sent.correlation == 99 && !sent.fd_count);
    assert(gpud_drm_ioctl_reply(&request,
                                &encoded,
                                99,
                                native_vmo + PH_GPU_QUERY_REPLY_OFFSET,
                                sent.value,
                                native_vmo + PH_GPU_QUERY_OUTPUT_OFFSET,
                                PH_GPU_QUERY_PAGE) == expected_error);
    assert(!service.release(service.context) && unmapped == 1 && !query.mapping);
    assert(service.prepare(service.context, &packet) == -EPROTO && mapped == 1); /* No replay. */
    assert(!service.stop(service.context));
}

static int prepare_caps_snapshot(struct ph_gpu_query *query, uint64_t correlation, uint32_t bytes) {
    struct gpud_drm_binding binding = {.generation = 9, .frontend_handle = 8, .session_id = 7};
    gpud_drm_virtgpu_get_caps_t caps = {.cap_set_id = 1, .cap_set_ver = 2, .size = bytes};
    gpud_drm_ioctl_request_t request = {.handle = 8,
        .request = GPUD_DRM_IOCTL_VIRTGPU_GET_CAPS, .arg_size = sizeof(caps),
        .data_size = sizeof(caps), .aux_size = bytes};
    memcpy(request.data, &caps, sizeof(caps));
    struct gpud_drm_translation translation;
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, PH_GPU_QUERY_OUTPUT_REGION));
    kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
        .opcode = KB2_GPU_OPCODE_COMMAND, .generation = 9, .correlation_id = correlation,
        .payload_length = translation.command_size};
    size_t size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + translation.command_size;
    assert(!kb2_protocol_message_envelope_encode(query->request, size, &envelope));
    memcpy(query->request + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
        translation.command, translation.command_size);
    return ph_gpu_query_prepare_snapshot(query, size, correlation, KB2_GPU_QUEUE_EXECUTION);
}

static void private_aux_lifetime(void) {
    struct ph_gpu_query query = {0};
    struct ph_lifecycle_service service;
    assert(!ph_gpu_query_init(&query, 9, 7, &service));
    assert(ph_gpu_query_destroy_aux(NULL) == -EINVAL);
    aux_mapped = aux_unmapped = 0;
    aux_map_failure = 1;
    assert(prepare_caps_snapshot(&query, 1, 5284) == -ENOMEM);
    assert(!query.aux && !query.aux_cache && !query.correlation);
    aux_map_failure = 0;
    for (uint64_t i = 1; i <= 8; ++i) {
        assert(!prepare_caps_snapshot(&query, i, i % 2 ? 5284 : 7276));
        assert(query.aux == private_aux && query.aux_cache == private_aux);
        assert(query.aux_mapping_size == sizeof(private_aux));
        for (size_t j = 0; j < 8192; ++j) assert(!private_aux[j]);
        memset(query.aux, 0xa5, 8192);
        assert(prepare_caps_snapshot(&query, i + 1, 5284) == -EPROTO);
        assert(!ph_gpu_query_release_aux(&query) && !query.aux && !query.aux_size);
        assert(aux_mapped == 1 && !aux_unmapped);
    }
    assert(!prepare_caps_snapshot(&query, 9, sizeof(private_aux) + 1));
    assert(query.aux == large_aux && query.aux_mapping_size == sizeof(private_aux) + 4096);
    assert(query.aux_cache == private_aux && aux_mapped == 2);
    aux_unmap_failure = 1;
    assert(ph_gpu_query_release_aux(&query) == -EIO && query.aux == large_aux);
    assert(prepare_caps_snapshot(&query, 10, 5284) == -EPROTO);
    aux_unmap_failure = 0;
    assert(!ph_gpu_query_release_aux(&query) && aux_unmapped == 1);
    assert(!prepare_caps_snapshot(&query, 10, 5284) && query.aux == private_aux && aux_mapped == 2);
    aux_unmap_failure = 1;
    assert(service.stop(service.context) == -EIO && query.aux_cache == private_aux);
    aux_unmap_failure = 0;
    assert(!service.stop(service.context) && !query.aux && !query.aux_cache && aux_unmapped == 2);
    assert(!service.stop(service.context) && aux_unmapped == 2);
}

static void codec_errors(const unsigned char *bytes, size_t size, uint64_t session) {
    unsigned char mutated[512];
    kb2_gpu_inline_completion_t saved = {.session_id = 77, .status = 88}, decoded = saved;
    for (size_t length = 0; length < size; ++length) {
        assert(kb2_gpu_inline_completion_decode(bytes, length, session, &decoded));
        assert(!memcmp(&saved, &decoded, sizeof(saved)));
    }
    assert(kb2_gpu_inline_completion_decode(bytes, size, session + 1, &decoded));
    const size_t offsets[] = {KB2_GPU_COMPLETION_HEADER_TOTAL_SIZE_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_ABI_IDENTITY_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_SCHEMA_DIGEST_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_SPAN_COUNT_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_ATTACHMENT_COUNT_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_ARGUMENT_TABLE_OFFSET_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_INLINE_OFFSET_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_RESERVED_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_DETAIL_CODE_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_DETAIL_SET_ID_OFFSET};
    for (size_t index = 0; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
        memcpy(mutated, bytes, size);
        mutated[offsets[index]] ^= 1;
        assert(kb2_gpu_inline_completion_decode(mutated, size, session, &decoded));
        assert(!memcmp(&saved, &decoded, sizeof(saved)));
    }
}

static void virtgpu_input_execution(void) {
    const kb2_gpu_region_t region = {
        .region_id = 1,
        .rights = KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE,
        .length = 12};
    const struct gpud_drm_binding binding = {
        .generation = 9, .frontend_handle = 8, .session_id = 7};
    const struct kobox_drm_query_api api = {
        .virtgpu_context_init = virtgpu_context_init,
        .virtgpu_execbuffer = virtgpu_execbuffer,
    };
    gpud_drm_ioctl_request_t request = {
        .handle = 8, .request = GPUD_DRM_IOCTL_VIRTGPU_CONTEXT_INIT,
        .arg_size = sizeof(gpud_drm_virtgpu_context_init_t),
        .data_size = sizeof(gpud_drm_virtgpu_context_wire_t)};
    gpud_drm_virtgpu_context_wire_t context = {
        .parameter_mask = GPUD_DRM_VIRTGPU_CONTEXT_HAS_CAPSET_ID,
        .capset_id = 1,
    };
    memcpy(request.data, &context, sizeof(context));
    struct gpud_drm_translation translation;
    struct kobox_drm_query query;
    unsigned char output[224] = {0}, completion[512], reply[1024];
    size_t completion_size;
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, region.region_id));
    assert(!kobox_drm_query_prepare(&query, binding.generation, binding.session_id,
        KB2_GPU_QUEUE_EXECUTION, translation.command,
        translation.command_size, &region));
    assert(query.aux_input && !query.aux_size);
    assert(!kobox_drm_query_execute(&query, &api, (void *)&calls,
        output, sizeof(output), NULL, 0, completion, sizeof(completion), &completion_size));
    kb2_gpu_inline_completion_t decoded;
    assert(!kb2_gpu_inline_completion_decode(
        completion, completion_size, binding.session_id, &decoded));
    assert(decoded.status == KB2_GPU_STATUS_OK && !decoded.record_schema_id && !decoded.length);

    gpud_drm_virtgpu_execbuffer_t exec = {
        .size = 4, .bo_handles = 8, .num_bo_handles = 1, .fence_fd = -1};
    request = (gpud_drm_ioctl_request_t) {
        .handle = 8, .request = GPUD_DRM_IOCTL_VIRTGPU_EXECBUFFER,
        .arg_size = sizeof(exec), .data_size = sizeof(exec), .aux_size = 12};
    memcpy(request.data, &exec, sizeof(exec));
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, region.region_id));
    assert(!kobox_drm_query_prepare(&query, binding.generation, binding.session_id,
        KB2_GPU_QUEUE_EXECUTION, translation.command,
        translation.command_size, &region));
    assert(query.aux_input && query.command_size == 4 &&
        query.handles_offset == 8 && query.handle_count == 1 && query.aux_size == 12);
    unsigned char aux[12] = {0};
    aux[8] = 7;
    assert(!kobox_drm_query_execute(&query, &api, (void *)&calls,
        output, sizeof(output), aux, sizeof(aux), completion, sizeof(completion), &completion_size));
    assert(!kb2_gpu_inline_completion_decode(
        completion, completion_size, binding.session_id, &decoded));
    assert(decoded.status == KB2_GPU_STATUS_OK &&
        decoded.record_schema_id == KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_RESULT &&
        decoded.length == KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_RESULT_SIZE);
    kb2_protocol_message_envelope_t envelope = {
        .protocol_id = KB2_GPU_PROTOCOL_ID, .opcode = KB2_GPU_OPCODE_COMMAND,
        .generation = binding.generation, .correlation_id = 91,
        .payload_length = completion_size};
    size_t reply_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + completion_size;
    assert(!kb2_protocol_message_envelope_encode(reply, reply_size, &envelope));
    memcpy(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, completion, completion_size);
    assert(!gpud_drm_ioctl_reply(
        &request, &translation, 91, reply, reply_size, output, sizeof(output)));
    reply[KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + KB2_GPU_COMPLETION_HEADER_SIZE +
          KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE] = 1;
    assert(gpud_drm_ioctl_reply(
        &request, &translation, 91, reply, reply_size, output, sizeof(output)) == -EPROTO);
}

static struct kobox_linux_drm_cursor observed_cursor;
static int cursor_status;

static int cursor(struct kobox_linux_drm_file *file,
                  const struct kobox_linux_drm_cursor *value) {
    assert(file);
    observed_cursor = *value;
    return cursor_status;
}

static void cursor_execution(void) {
    const struct gpud_drm_binding binding = {
        .generation = 9, .frontend_handle = 8, .session_id = 7};
    const struct kobox_drm_query_api api = {.cursor = cursor};
    const kb2_gpu_region_t region = {.region_id = 1,
        .rights = KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE,
        .length = 4096};
    /* Legacy move (negative coordinates), CURSOR2 show with hotspot, hide,
     * and a real backend authorization failure, not synthetic success. */
    for (unsigned int test = 0; test < 4; ++test) {
        gpud_drm_mode_cursor2_t value = {.cursor = {
            .flags = test ? GPUD_DRM_MODE_CURSOR_BO : GPUD_DRM_MODE_CURSOR_MOVE,
            .crtc_id = 17, .x = -23, .y = -41,
            .width = 64, .height = 64, .handle = test == 2 ? 0 : 27},
            .hot_x = 3, .hot_y = 5};
        gpud_drm_ioctl_request_t request = {.handle = binding.frontend_handle,
            .request = test ? GPUD_DRM_IOCTL_MODE_CURSOR2 : GPUD_DRM_IOCTL_MODE_CURSOR,
            .arg_size = test ? sizeof(value) : sizeof(value.cursor),
            .data_size = test ? sizeof(value) : sizeof(value.cursor)};
        memcpy(request.data, &value, request.data_size);
        struct gpud_drm_translation translation;
        struct kobox_drm_query query;
        unsigned char completion[512], reply[1024];
        size_t completion_size;
        assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 0));
        assert(translation.queue_class == KB2_GPU_QUEUE_DISPLAY);
        assert(!kobox_drm_query_prepare(&query, binding.generation,
            binding.session_id, translation.queue_class, translation.command,
            translation.command_size, &region));
        cursor_status = test == 3 ? -EACCES : 0;
        assert(!kobox_drm_query_execute(&query, &api, (void *)&calls,
            NULL, 0, NULL, 0, completion, sizeof(completion), &completion_size));
        assert(observed_cursor.flags == value.cursor.flags &&
            observed_cursor.crtc_id == 17 && observed_cursor.x == -23 &&
            observed_cursor.y == -41 && observed_cursor.width == 64 &&
            observed_cursor.height == 64 && observed_cursor.handle == value.cursor.handle &&
            observed_cursor.hot_x == (test ? 3 : 0) &&
            observed_cursor.hot_y == (test ? 5 : 0));
        kb2_protocol_message_envelope_t envelope = {
            .protocol_id = KB2_GPU_PROTOCOL_ID, .opcode = KB2_GPU_OPCODE_COMMAND,
            .generation = binding.generation, .correlation_id = 41,
            .payload_length = completion_size};
        size_t reply_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + completion_size;
        assert(!kb2_protocol_message_envelope_encode(reply, reply_size, &envelope));
        memcpy(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, completion, completion_size);
        assert(gpud_drm_ioctl_reply(&request, &translation, 41, reply,
            reply_size, NULL, 0) == cursor_status);
        --request.data_size;
        assert(gpud_drm_ioctl_encode(&translation, &binding, &request, 0) == -EINVAL);
        ++request.data_size;
        value.cursor.flags = 4;
        memcpy(request.data, &value, request.data_size);
        assert(gpud_drm_ioctl_encode(&translation, &binding, &request, 0) == -EINVAL);
    }
}

static void display_execution(void) {
    const struct gpud_drm_binding binding = {
        .generation = 9, .frontend_handle = 8, .session_id = 7};
    const kb2_gpu_region_t region = {
        .region_id = 1,
        .rights = KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE,
        .length = 4096,
    };
    const struct kobox_drm_query_api api = {
        .create_dumb = create_dumb,
        .add_fb = add_fb,
        .remove_fb = remove_fb,
        .object_properties = object_properties,
        .get_crtc = get_crtc,
        .dirty_fb = dirty_fb,
        .virtgpu_map = virtgpu_map,
    };
    gpud_drm_ioctl_request_t request = {
        .handle = binding.frontend_handle,
        .request = GPUD_DRM_IOCTL_MODE_CREATE_DUMB,
        .arg_size = sizeof(gpud_drm_mode_create_dumb_t),
        .data_size = sizeof(gpud_drm_mode_create_dumb_t),
    };
    gpud_drm_mode_create_dumb_t create = {
        .height = 61, .width = 79, .bpp = 32};
    memcpy(request.data, &create, sizeof(create));

    struct gpud_drm_translation translation;
    struct kobox_drm_query query;
    unsigned char output[224] = {0}, completion[512], reply[1024];
    size_t completion_size;
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 0));
    assert(translation.queue_class == KB2_GPU_QUEUE_DISPLAY);
    assert(!kobox_drm_query_prepare(&query, binding.generation,
        binding.session_id, KB2_GPU_QUEUE_DISPLAY, translation.command,
        translation.command_size, &region));
    assert(!kobox_drm_query_execute(&query, &api, (void *)&calls,
        output, sizeof(output), NULL, 0, completion, sizeof(completion),
        &completion_size));
    kb2_protocol_message_envelope_t envelope = {
        .protocol_id = KB2_GPU_PROTOCOL_ID,
        .opcode = KB2_GPU_OPCODE_COMMAND,
        .generation = binding.generation,
        .correlation_id = 41,
        .payload_length = completion_size,
    };
    size_t reply_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + completion_size;
    assert(!kb2_protocol_message_envelope_encode(reply, reply_size, &envelope));
    memcpy(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, completion,
        completion_size);
    assert(!gpud_drm_ioctl_reply(&request, &translation, 41, reply,
        reply_size, output, sizeof(output)));
    memcpy(&create, request.data, sizeof(create));
    assert(create.handle == 27 && create.pitch == 79 * 4 &&
        create.size == 4096);

    gpud_drm_mode_fb_cmd_t framebuffer = {
        .width = 79,
        .height = 61,
        .pitch = create.pitch,
        .bpp = 32,
        .depth = 24,
        .handle = create.handle,
    };
    request = (gpud_drm_ioctl_request_t) {
        .handle = binding.frontend_handle,
        .request = GPUD_DRM_IOCTL_MODE_ADDFB,
        .arg_size = sizeof(framebuffer),
        .data_size = sizeof(framebuffer),
    };
    memcpy(request.data, &framebuffer, sizeof(framebuffer));
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 0));
    assert(!kobox_drm_query_prepare(&query, binding.generation,
        binding.session_id, KB2_GPU_QUEUE_DISPLAY, translation.command,
        translation.command_size, &region));
    assert(!kobox_drm_query_execute(&query, &api, (void *)&calls,
        output, sizeof(output), NULL, 0, completion, sizeof(completion),
        &completion_size));
    envelope.correlation_id = 42;
    envelope.payload_length = completion_size;
    reply_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + completion_size;
    assert(!kb2_protocol_message_envelope_encode(reply, reply_size, &envelope));
    memcpy(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, completion,
        completion_size);
    assert(!gpud_drm_ioctl_reply(&request, &translation, 42, reply,
        reply_size, output, sizeof(output)));
    memcpy(&framebuffer, request.data, sizeof(framebuffer));
    assert(framebuffer.fb_id == 29);

    gpud_drm_kms_crtc_wire_t crtc = {
        .value = {.crtc_id = 12},
    };
    request = (gpud_drm_ioctl_request_t) {
        .handle = binding.frontend_handle,
        .request = GPUD_DRM_IOCTL_MODE_GETCRTC,
        .arg_size = sizeof(crtc.value),
        .data_size = sizeof(crtc),
    };
    memcpy(request.data, &crtc, sizeof(crtc));
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request,
        region.region_id));
    assert(!kobox_drm_query_prepare(&query, binding.generation,
        binding.session_id, KB2_GPU_QUEUE_DISPLAY, translation.command,
        translation.command_size, &region));
    assert(!kobox_drm_query_execute(&query, &api, (void *)&calls,
        output, sizeof(output), NULL, 0, completion, sizeof(completion),
        &completion_size));
    envelope.correlation_id = 43;
    envelope.payload_length = completion_size;
    reply_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + completion_size;
    assert(!kb2_protocol_message_envelope_encode(reply, reply_size, &envelope));
    memcpy(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, completion,
        completion_size);
    assert(!gpud_drm_ioctl_reply(&request, &translation, 43, reply,
        reply_size, output, sizeof(output)));
    memcpy(&crtc, request.data, sizeof(crtc));
    assert(crtc.value.crtc_id == 12 && crtc.value.fb_id == 29 &&
        crtc.value.gamma_size == 256 && crtc.value.mode_valid == 1 &&
        crtc.value.mode.hdisplay == 640 && crtc.value.mode.vdisplay == 480);

    gpud_drm_mode_fb_dirty_t dirty = {
        .fb_id = framebuffer.fb_id,
        .color = UINT32_C(0x12345678),
        .num_clips = 1,
    };
    gpud_drm_mode_rectangle_t rectangle = {
        .x1 = 3, .y1 = 5, .x2 = 73, .y2 = 59,
    };
    request = (gpud_drm_ioctl_request_t) {
        .handle = binding.frontend_handle,
        .request = GPUD_DRM_IOCTL_MODE_DIRTYFB,
        .arg_size = sizeof(dirty),
        .data_size = sizeof(dirty),
        .aux_size = sizeof(rectangle),
    };
    memcpy(request.data, &dirty, sizeof(dirty));
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request,
        region.region_id));
    assert(!kobox_drm_query_prepare(&query, binding.generation,
        binding.session_id, KB2_GPU_QUEUE_DISPLAY, translation.command,
        translation.command_size, &region));
    unsigned int calls_before_dirty = calls;
    assert(!kobox_drm_query_execute(&query, &api, (void *)&calls,
        output, sizeof(output), (unsigned char *)&rectangle, sizeof(rectangle),
        completion, sizeof(completion), &completion_size));
    assert(calls == calls_before_dirty + 1);
    kb2_gpu_inline_completion_t dirty_completion;
    assert(!kb2_gpu_inline_completion_decode(completion, completion_size,
        binding.session_id, &dirty_completion));
    assert(dirty_completion.status == KB2_GPU_STATUS_OK &&
        !dirty_completion.record_schema_id && !dirty_completion.length);

    dirty = (gpud_drm_mode_fb_dirty_t){0};
    request = (gpud_drm_ioctl_request_t) {
        .handle = binding.frontend_handle,
        .request = GPUD_DRM_IOCTL_MODE_DIRTYFB,
        .arg_size = sizeof(dirty),
        .data_size = sizeof(dirty),
    };
    memcpy(request.data, &dirty, sizeof(dirty));
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 0));
    assert(!kobox_drm_query_prepare(&query, binding.generation,
        binding.session_id, KB2_GPU_QUEUE_DISPLAY, translation.command,
        translation.command_size, &region));
    calls_before_dirty = calls;
    assert(!kobox_drm_query_execute(&query, &api, (void *)&calls,
        output, sizeof(output), NULL, 0, completion, sizeof(completion),
        &completion_size));
    assert(calls == calls_before_dirty + 1);
    assert(!kb2_gpu_inline_completion_decode(completion, completion_size,
        binding.session_id, &dirty_completion));
    assert(dirty_completion.status == KB2_GPU_STATUS_NOT_FOUND &&
        !dirty_completion.record_schema_id && !dirty_completion.length);
    envelope.correlation_id = 44;
    envelope.payload_length = completion_size;
    reply_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + completion_size;
    assert(!kb2_protocol_message_envelope_encode(reply, reply_size, &envelope));
    memcpy(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, completion,
        completion_size);
    assert(gpud_drm_ioctl_reply(&request, &translation, 44, reply,
        reply_size, output, sizeof(output)) == -ENOENT);

    request = (gpud_drm_ioctl_request_t) {
        .handle = binding.frontend_handle,
        .request = GPUD_DRM_IOCTL_MODE_RMFB,
        .arg_size = sizeof(framebuffer.fb_id),
        .data_size = sizeof(framebuffer.fb_id),
    };
    memcpy(request.data, &framebuffer.fb_id, sizeof(framebuffer.fb_id));
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 0));
    assert(!kobox_drm_query_prepare(&query, binding.generation,
        binding.session_id, KB2_GPU_QUEUE_DISPLAY, translation.command,
        translation.command_size, &region));
    assert(!kobox_drm_query_execute(&query, &api, (void *)&calls,
        output, sizeof(output), NULL, 0, completion, sizeof(completion),
        &completion_size));
    envelope.correlation_id = 45;
    envelope.payload_length = completion_size;
    reply_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + completion_size;
    assert(!kb2_protocol_message_envelope_encode(reply, reply_size, &envelope));
    memcpy(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, completion,
        completion_size);
    assert(!gpud_drm_ioctl_reply(&request, &translation, 45, reply,
        reply_size, output, sizeof(output)));

    gpud_drm_mode_map_dumb_t map = {.handle = create.handle};
    request = (gpud_drm_ioctl_request_t) {
        .handle = binding.frontend_handle,
        .request = GPUD_DRM_IOCTL_MODE_MAP_DUMB,
        .arg_size = sizeof(map),
        .data_size = sizeof(map),
    };
    memcpy(request.data, &map, sizeof(map));
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 0));
    assert(!kobox_drm_query_prepare(&query, binding.generation,
        binding.session_id, KB2_GPU_QUEUE_DISPLAY, translation.command,
        translation.command_size, &region));
    uint64_t pages[1] = {0};
    query.mapping_page_capacity = 1;
    query.aux_size = sizeof(pages);
    assert(!kobox_drm_query_execute_service(&query, &api,
        (struct kobox_linux_drm_service *)(void *)&calls, 3,
        (void *)&calls, output, sizeof(output), (unsigned char *)pages,
        sizeof(pages), completion, sizeof(completion), &completion_size,
        &(struct kobox_drm_query_result){0}));
    kb2_gpu_mode_map_completion_t mapped;
    assert(!kb2_gpu_mode_map_completion_decode(completion, completion_size,
        binding.session_id, binding.generation, &mapped));
    assert(mapped.mapping_id == 4096 && mapped.length == 4096 &&
        mapped.exchange_id == 4096 && pages[0] == 11);
    envelope.correlation_id = 46;
    envelope.payload_length = completion_size;
    reply_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + completion_size;
    assert(!kb2_protocol_message_envelope_encode(reply, reply_size, &envelope));
    memcpy(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, completion,
        completion_size);
    assert(!gpud_drm_ioctl_reply(&request, &translation, 46, reply,
        reply_size, output, sizeof(output)));
    memcpy(&map, request.data, sizeof(map));
    assert(map.handle == create.handle && map.offset == 4096 &&
        translation.mapping_id == 4096 &&
        translation.mapping_exchange == 4096 &&
        translation.mapping_length == 4096);

    gpud_drm_kms_object_properties_wire_t properties = {
        .value = {
            .count_props = 2,
            .obj_id = 13,
            .obj_type = GPUD_DRM_MODE_OBJECT_CONNECTOR,
        },
    };
    request = (gpud_drm_ioctl_request_t) {
        .handle = binding.frontend_handle,
        .request = GPUD_DRM_IOCTL_MODE_OBJ_GETPROPERTIES,
        .arg_size = sizeof(properties.value),
        .data_size = sizeof(properties),
    };
    memcpy(request.data, &properties, sizeof(properties));
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request,
        region.region_id));
    assert(!kobox_drm_query_prepare(&query, binding.generation,
        binding.session_id, KB2_GPU_QUEUE_DISPLAY, translation.command,
        translation.command_size, &region));
    assert(!kobox_drm_query_execute(&query, &api, (void *)&calls,
        output, sizeof(output), NULL, 0, completion, sizeof(completion),
        &completion_size));
    envelope.correlation_id = 47;
    envelope.payload_length = completion_size;
    reply_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + completion_size;
    assert(!kb2_protocol_message_envelope_encode(reply, reply_size, &envelope));
    memcpy(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, completion,
        completion_size);
    assert(!gpud_drm_ioctl_reply(&request, &translation, 47, reply,
        reply_size, output, sizeof(output)));
    memcpy(&properties, request.data, sizeof(properties));
    assert(properties.value.count_props == 3 &&
        properties.props[0] == 101 && properties.prop_values[0] == 201 &&
        properties.props[1] == 102 && properties.prop_values[1] == 202);
}

static void native_fence_events(void) {
    struct ph_gpu_queue queue = {0};
    struct gpud_gpu_sessions sessions = {0};
    struct gpud_gpu_channel frontend = {0};
    struct ph_lifecycle_service service;
    memset(queue_vmo, 0, sizeof(queue_vmo));
    active_vmo = queue_vmo;
    active_size = sizeof(queue_vmo);
    mapped = unmapped = aux_unmapped = sends = 0;
    assert(!ph_gpu_channel_bind(&frontend, queue_vmo, 9, 27,
        KB2_VQ_DRIVER, &kb2_vq_x86_64_atomics));
    assert(!ph_gpu_sessions_init(&sessions, 9, 2));
    assert(!ph_gpu_queue_init(&queue, &sessions, 7, 27,
        &kb2_vq_x86_64_atomics, &service));
    struct ph_ipc_packet packet = {
        .operation = PH_GPU_QUEUE_BIND, .generation = 9, .value = 27,
        .fd_count = 1,
        .fds = {{.fd = 16, .rights = PH_GPU_QUERY_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC}},
    };
    assert(service.prepare(service.context, &packet) == PH_LIFECYCLE_SERVICE_IDLE);
    const size_t batch = PH_GPU_DRM_EVENT_BYTES / PH_GPU_FENCE_RECORD_BYTES;
    /* No open file/session remains. Completions must survive that close,
     * and a temporarily missing posted event buffer must not lose them. */
    fence_taken = 0;
    fence_results = 2 * batch;
    assert(!queue.event_host.notify_fences(queue.event_host.context));
    assert(!queue.event_host.notify_fences(queue.event_host.context));
    assert(service.next(service.context) == PH_LIFECYCLE_SERVICE_IDLE);
    assert(atomic_load(&queue.fence_pending) && !fence_taken);
    kb2_vq_t *lane = &frontend.lanes[GPUD_GPU_QUEUE_EVENT];
    size_t verified = 0;
    for (unsigned int pass = 0; pass < 3; ++pass) {
        kb2_vq_segment_t segment = {
            .address = GPUD_GPU_EVENT_OFFSET,
            .length = GPUD_GPU_CHANNEL_PAGE, .writable = 1,
        };
        kb2_vq_chain_t chain = {.segments = &segment, .capacity = 1, .count = 1};
        int ready, notify;
        assert(!kb2_vq_arm(lane, 9, &ready) && !ready);
        assert(!kb2_vq_publish(lane, 9, &chain, &notify));
        packet = (struct ph_ipc_packet) {
            .operation = PH_GPU_QUEUE_NOTIFY, .generation = 9,
            .value = lane->queue.available_notification_id,
        };
        assert(!service.prepare(service.context, &packet));
        assert(queue.event_fences && !sessions.occupied);
        assert(!service.dispatch(service.context, &calls));
        struct ph_ipc ipc = {.generation = 9};
        assert(!service.complete(service.context, &ipc));
        assert(!service.release(service.context));
        kb2_vq_chain_t *done = NULL;
        assert(!kb2_vq_take_used(lane, 9, &done) && done == &chain);
        unsigned char bytes[GPUD_GPU_CHANNEL_PAGE];
        assert(!kb2_vq_copy_response(lane, 9, done, 0, bytes, done->used_length));
        struct ph_gpu_drm_event_message event;
        assert(!ph_gpu_drm_event_decode(bytes, done->used_length, 9, &event));
        assert(event.fences && !event.session_id && event.sequence == pass + 1);
        assert(event.data_size == (pass < 2 ? batch * PH_GPU_FENCE_RECORD_BYTES : 0));
        for (size_t offset = 0; offset < event.data_size; offset += PH_GPU_FENCE_RECORD_BYTES) {
            const unsigned char *record = event.data + offset;
            assert(ph_gpu_event_load_u64(record) == 71);
            assert(ph_gpu_event_load_u64(record + 8) == 100 + verified);
            assert((int32_t)ph_gpu_event_load_u32(record + 16) == (verified % 2 ? -EIO : 1));
            assert(!ph_gpu_event_load_u32(record + 20));
            ++verified;
        }
        assert(!kb2_vq_release(lane, 9, done));
    }
    assert(verified == fence_results && fence_taken == fence_results);
    assert(!atomic_load(&queue.fence_pending));
    assert(service.next(service.context) == PH_LIFECYCLE_SERVICE_IDLE);
    assert(!service.stop(service.context) && !queue.mapping && unmapped == 1);
    fence_results = fence_taken = 0;
}

static void native_queue_case(unsigned int defect) {
    struct ph_gpu_queue queue = {0};
    struct gpud_gpu_sessions sessions = {0};
    struct gpud_gpu_channel frontend = {0};
    struct ph_lifecycle_service service;
    memset(queue_vmo, 0, sizeof(queue_vmo));
    active_vmo = queue_vmo;
    active_size = sizeof(queue_vmo);
    mapped = unmapped = aux_unmapped = sends = 0;
    assert(
        !ph_gpu_channel_bind(&frontend, queue_vmo, 9, 27, KB2_VQ_DRIVER, &kb2_vq_x86_64_atomics));
    assert(!ph_gpu_sessions_init(&sessions, 9, 2));
    assert(!ph_gpu_queue_init(&queue, &sessions, 7, 27, &kb2_vq_x86_64_atomics, &service));
    assert(service.next(service.context) == PH_LIFECYCLE_SERVICE_IDLE);
    struct ph_ipc_packet packet = {
        .operation = PH_GPU_QUEUE_BIND,
        .generation = 9,
        .value = 27,
        .fd_count = 1,
        .fds = {{.fd = 16, .rights = PH_GPU_QUERY_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC}}};
    if (defect == 1)
        queue_vmo[0] ^= 1;
    if (defect == 2)
        packet.value = 28;
    if (defect == 3)
        packet.fds[0].rights ^= PACHA_FD_RIGHT_MAP_WRITE;
    int result = service.prepare(service.context, &packet);
    if (defect >= 1 && defect <= 3) {
        assert(result < 0);
        assert(!service.stop(service.context) && !queue.mapping && mapped == unmapped);
        assert(mapped == (defect == 1));
        return;
    }
    assert(result == PH_LIFECYCLE_SERVICE_IDLE && mapped == 1);
    /* Open through the control queue, not a preinstalled session fixture. */
    unsigned char *control = queue_vmo + GPUD_GPU_CONTROL_REQUEST_OFFSET;
    size_t control_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + KB2_GPU_SESSION_OPEN_REQUEST_SIZE;
    kb2_protocol_message_envelope_t open_envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
                                                     .opcode = KB2_GPU_OPCODE_SESSION_OPEN,
                                                     .generation = 9,
                                                     .correlation_id = 1,
                                                     .payload_length =
                                                         KB2_GPU_SESSION_OPEN_REQUEST_SIZE};
    kb2_gpu_session_open_t open = {.node_type = KB2_GPU_NODE_RENDER, .client_id = 7};
    assert(!kb2_protocol_message_envelope_encode(control, control_size, &open_envelope));
    assert(!kb2_gpu_session_open_encode(
        control + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, KB2_GPU_SESSION_OPEN_REQUEST_SIZE, &open));
    kb2_vq_segment_t control_segments[] = {
        {GPUD_GPU_CONTROL_REQUEST_OFFSET, control_size, 0, 0},
        {GPUD_GPU_CONTROL_REPLY_OFFSET, GPUD_GPU_CONTROL_REPLY_CAPACITY, 1, 1}};
    kb2_vq_chain_t control_chain = {.segments = control_segments, .capacity = 2, .count = 2};
    kb2_vq_t *control_lane = &frontend.lanes[GPUD_GPU_QUEUE_CONTROL];
    int control_ready, control_notify;
    assert(!kb2_vq_arm(control_lane, 9, &control_ready) && !control_ready);
    assert(!kb2_vq_publish(control_lane, 9, &control_chain, &control_notify) && control_notify);
    packet = (struct ph_ipc_packet){.operation = PH_GPU_QUEUE_NOTIFY, .generation = 9, .value = 3};
    assert(!service.prepare(service.context, &packet));
    assert(!service.dispatch(service.context, &calls));
    struct ph_ipc control_ipc = {.generation = 9};
    if (defect == 7) {
        /* Failed delivery must not drop the successfully opened file's
         * ownership. Channel retirement leaves it for GPL device close-all. */
        unsigned int closes = mock_closes;
        mock_send_error = -EIO;
        assert(service.complete(service.context, &control_ipc) == -EIO);
        mock_send_error = 0;
        assert(!service.release(service.context));
        assert(!service.stop(service.context) && unmapped == 1 && !queue.mapping);
        assert(sessions.occupied == 1 && mock_closes == closes);
        assert(sessions.entries[0].state == GPUD_GPU_SESSION_OPEN &&
               sessions.entries[0].file_cookie);
        return;
    }
    assert(!service.complete(service.context, &control_ipc));
    assert(!service.release(service.context));
    kb2_vq_chain_t *control_done = NULL;
    assert(!kb2_vq_take_used(control_lane, 9, &control_done) && control_done == &control_chain);
    unsigned char control_reply[GPUD_GPU_CONTROL_REPLY_CAPACITY];
    assert(!kb2_vq_copy_response(
        control_lane, 9, control_done, 0, control_reply, control_done->used_length));
    kb2_gpu_session_completion_t opened;
    assert(!kb2_gpu_session_completion_decode(control_reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                              control_done->used_length -
                                                  KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                              KB2_GPU_OPCODE_SESSION_OPEN,
                                              &opened));
    assert(!opened.status && opened.session_id && sends == 1 && sent.value == 4);
    assert(!kb2_vq_release(control_lane, 9, control_done));
    sends = 0;
    packet = (struct ph_ipc_packet){.operation = PH_GPU_QUEUE_NOTIFY, .generation = 9, .value = 7};
    assert(service.prepare(service.context, &packet) == PH_LIFECYCLE_SERVICE_IDLE);
    const uint64_t command_count = defect ? 2 : 5;
    for (uint64_t correlation = 1; correlation <= command_count; ++correlation) {
        struct gpud_drm_binding binding = {
            .generation = 9, .frontend_handle = 8, .session_id = opened.session_id};
        bool input = !defect && (correlation == 2 || correlation >= 4);
        bool display = !defect && correlation == 3;
        gpud_drm_ioctl_request_t request = {.handle = 8};
        if (display) {
            request.request = GPUD_DRM_IOCTL_SET_MASTER;
        } else if (input) {
            gpud_drm_virtgpu_execbuffer_t exec = {
                .size = 4, .bo_handles = 8, .num_bo_handles = 1, .fence_fd = -1};
            request.request = GPUD_DRM_IOCTL_VIRTGPU_EXECBUFFER;
            request.arg_size = request.data_size = sizeof(exec);
            request.aux_size = 12;
            memcpy(request.data, &exec, sizeof(exec));
            memset(queue_vmo + GPUD_GPU_AUX_OFFSET, 0, request.aux_size);
            queue_vmo[GPUD_GPU_AUX_OFFSET + 8] = 7;
        } else {
            uint64_t capability = defect == 4 ? 6 : 5;
            request.request = UINT64_C(0xc010640c);
            request.arg_size = request.data_size = 16;
            memcpy(request.data, &capability, sizeof(capability));
        }
        struct gpud_drm_translation encoded;
        assert(!gpud_drm_ioctl_encode(&encoded, &binding, &request, 1));
        kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
                                                    .opcode = KB2_GPU_OPCODE_COMMAND,
                                                    .generation = 9,
                                                    .correlation_id = correlation,
                                                    .payload_length = encoded.command_size};
        size_t request_offset = display ? GPUD_GPU_DISPLAY_REQUEST_OFFSET :
            GPUD_GPU_REQUEST_OFFSET;
        size_t reply_offset = display ? GPUD_GPU_DISPLAY_REPLY_OFFSET :
            GPUD_GPU_REPLY_OFFSET;
        size_t reply_capacity = display ? GPUD_GPU_DISPLAY_REPLY_CAPACITY :
            GPUD_GPU_CHANNEL_PAGE;
        unsigned char *bytes = queue_vmo + request_offset;
        size_t size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + encoded.command_size;
        assert(!kb2_protocol_message_envelope_encode(bytes, size, &envelope));
        memcpy(bytes + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, encoded.command, encoded.command_size);
        kb2_vq_segment_t segments[] = {{request_offset, size, 0, 0},
                                       {reply_offset, reply_capacity, 1, 1}};
        kb2_vq_chain_t send = {.segments = segments, .capacity = 2, .count = 2};
        kb2_vq_t *lane = &frontend.lanes[display ? GPUD_GPU_QUEUE_DISPLAY :
            GPUD_GPU_QUEUE_EXECUTION];
        int ready, notify;
        assert(kb2_vq_arm(lane, 9, &ready) == KB2_VQ_OK && !ready);
        assert(kb2_vq_publish(lane, 9, &send, &notify) == KB2_VQ_OK && notify);
        unsigned int before = calls;
        if (defect == 5)
            bytes[KB2_PROTOCOL_MESSAGE_ENVELOPE_GENERATION_OFFSET] ^= 1;
        if (defect == 6) {
            /* Output page cannot be used as a response descriptor. */
            uint64_t address = GPUD_GPU_OUTPUT_OFFSET;
            memcpy(queue_vmo + lane->queue.descriptor_address + 16, &address, sizeof(address));
        }
        result = service.next(service.context); /* Drain without another packet. */
        if (defect == 5 || defect == 6) {
            assert(result == -EPROTO && calls == before);
            assert(!service.stop(service.context) && unmapped == 1);
            return;
        }
        assert(!result);
        memset(bytes, 0xcc, size); /* Private plan survives hostile source changes. */
        if (input)
            memset(queue_vmo + GPUD_GPU_AUX_OFFSET, 0xcc, request.aux_size);
        assert(!service.dispatch(service.context, &calls) && calls == before + 1);
        struct ph_ipc ipc = {.generation = 9};
        assert(service.complete(service.context, &ipc) == (defect == 4 ? -ENODEV : 0));
        assert(!service.release(service.context) && !unmapped && mapped == 1);
        assert(!aux_unmapped); /* Published requests relinquish ownership, not reusable backing. */
        assert(sends == correlation && sent.operation == PH_GPU_QUEUE_NOTIFY &&
               sent.value == (display ? 6 : 8));
        kb2_vq_chain_t *done = NULL;
        assert(kb2_vq_take_used(lane, 9, &done) == KB2_VQ_OK && done == &send);
        unsigned char reply[GPUD_GPU_CHANNEL_PAGE];
        assert(kb2_vq_copy_response(lane, 9, done, 0, reply, done->used_length) == KB2_VQ_OK);
        assert(gpud_drm_ioctl_reply(&request,
                                    &encoded,
                                    correlation,
                                    reply,
                                    done->used_length,
                                    queue_vmo + GPUD_GPU_OUTPUT_OFFSET,
                                    GPUD_GPU_CHANNEL_PAGE) == (defect == 4 ? -EIO : 0));
        assert(kb2_vq_release(lane, 9, done) == KB2_VQ_OK);
        if (defect == 4) {
            assert(service.next(service.context) == -ENODEV && calls == before + 1);
            break;
        }
        assert(service.prepare(service.context, &packet) == PH_LIFECYCLE_SERVICE_IDLE);
    }
    assert(!service.stop(service.context) && unmapped == 1 && !queue.mapping);
    assert(aux_unmapped == (!defect ? 1u : 0u) && !queue.service.query.aux_cache);
    assert(!service.stop(service.context) && unmapped == 1);
}

static kb2_gpu_session_completion_t session_request(struct ph_gpu_session_service *service,
                                                    uint64_t client,
                                                    uint64_t session,
                                                    uint32_t node) {
    uint32_t opcode = session ? KB2_GPU_OPCODE_SESSION_CLOSE : KB2_GPU_OPCODE_SESSION_OPEN;
    size_t payload =
        session ? KB2_GPU_SESSION_CLOSE_REQUEST_SIZE : KB2_GPU_SESSION_OPEN_REQUEST_SIZE;
    size_t size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + payload;
    kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
                                                .opcode = opcode,
                                                .generation = service->query.generation,
                                                .correlation_id = service->last_control + 1,
                                                .payload_length = payload};
    assert(!kb2_protocol_message_envelope_encode(service->query.request, size, &envelope));
    unsigned char *bytes = service->query.request + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE;
    if (session) {
        assert(!kb2_gpu_session_close_encode(bytes, payload, session));
    } else {
        kb2_gpu_session_open_t request = {.client_id = client, .node_type = node};
        assert(!kb2_gpu_session_open_encode(bytes, payload, &request));
    }
    assert(!ph_gpu_session_prepare(service, KB2_GPU_QUEUE_CONTROL, size));
    memset(service->query.request, 0xcc, size);
    assert(!ph_gpu_session_dispatch(service, &calls));
    kb2_gpu_session_completion_t completion;
    assert(!kb2_protocol_message_envelope_decode(
        service->query.reply, service->query.reply_size, &envelope));
    assert(envelope.opcode == opcode && envelope.correlation_id == service->last_control &&
           envelope.generation == service->query.generation);
    assert(!kb2_gpu_session_completion_decode(service->query.reply +
                                                  KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                              envelope.payload_length,
                                              opcode,
                                              &completion));
    assert(!ph_gpu_session_service_release(service));
    return completion;
}

static void session_service_cases(void) {
    struct gpud_gpu_sessions sessions = {0};
    struct ph_gpu_session_service first = {0}, other = {0};
    assert(!ph_gpu_sessions_init(&sessions, 9, 2));
    assert(!ph_gpu_session_service_init(&first, &sessions, 7));
    assert(!ph_gpu_session_service_init(&other, &sessions, 8));
    mock_opens = mock_closes = 0;
    kb2_gpu_session_completion_t reply = session_request(&first, 8, 0, KB2_GPU_NODE_RENDER);
    assert(reply.status == KB2_GPU_STATUS_DENIED && !mock_opens && !sessions.occupied);
    mock_open_error = -ENOMEM;
    reply = session_request(&first, 7, 0, KB2_GPU_NODE_PRIMARY);
    assert(reply.status == KB2_GPU_STATUS_NO_MEMORY && !reply.session_id &&
           !sessions.occupied && mock_opens == 1 && mock_open_node == KB2_GPU_NODE_PRIMARY);
    mock_open_error = 0;
    reply = session_request(&first, 7, 0, KB2_GPU_NODE_RENDER);
    assert(!reply.status && reply.session_id > 1 && mock_opens == 2 &&
           mock_open_node == KB2_GPU_NODE_RENDER);
    uint64_t a = reply.session_id;
    reply = session_request(&other, 8, a, 0);
    assert(reply.status == KB2_GPU_STATUS_DENIED && !mock_closes);
    reply = session_request(&other, 8, 0, KB2_GPU_NODE_RENDER);
    assert(!reply.status && reply.session_id != a && sessions.occupied == 2);
    uint64_t b = reply.session_id, cookie;
    reply = session_request(&first, 7, 0, KB2_GPU_NODE_RENDER);
    assert(reply.status == KB2_GPU_STATUS_LIMIT && mock_opens == 3);
    /* Model an already admitted request on another lane. Never close its file
     * or report CLOSE success while it is still in dispatch. */
    assert(!ph_gpu_session_acquire(&sessions, 9, 7, a, &cookie));
    reply = session_request(&first, 7, a, 0);
    assert(reply.status == KB2_GPU_STATUS_BUSY && !mock_closes);
    assert(ph_gpu_session_acquire(&sessions, 9, 7, a, &cookie) == KB2_GPU_STATUS_SESSION_LOST);
    assert(!ph_gpu_session_release(&sessions, 9, 7, a));
    reply = session_request(&first, 7, a, 0);
    assert(!reply.status && mock_closes == 1 && sessions.occupied == 1);
    reply = session_request(&first, 7, a, 0);
    assert(reply.status == KB2_GPU_STATUS_NOT_FOUND && mock_closes == 1);
    mock_close_error = -EIO;
    reply = session_request(&other, 8, b, 0);
    assert(reply.status == KB2_GPU_STATUS_DEVICE_LOST && mock_closes == 2 &&
           sessions.occupied == 1);
    assert(other.query.terminal_after_completion == -ENODEV);
    assert(ph_gpu_session_prepare(&other, KB2_GPU_QUEUE_CONTROL, 0) == -ENODEV);
    assert(ph_gpu_session_close_begin(&sessions, 9, 8, b) == KB2_GPU_STATUS_SESSION_LOST);
    mock_close_error = 0;
}

static void magic_execution(void) {
    const struct gpud_drm_binding binding = {.generation = 9,
        .frontend_handle = 8, .session_id = 7};
    const struct kobox_drm_query_api api = {.get_magic = get_magic,
        .auth_magic = auth_magic};
    const kb2_gpu_region_t region = {.region_id = 1,
        .rights = KB2_GPU_SPAN_RIGHT_WRITE, .length = 4};
    for (unsigned i = 0; i < 3; ++i) {
        gpud_drm_ioctl_request_t request = {.handle = binding.frontend_handle,
            .request = i ? GPUD_DRM_IOCTL_AUTH_MAGIC : GPUD_DRM_IOCTL_GET_MAGIC,
            .arg_size = 4, .data_size = 4};
        uint32_t magic = i == 1 ? 0x1234 : 0;
        memcpy(request.data, &magic, 4);
        struct gpud_drm_translation translation;
        struct kobox_drm_query query;
        unsigned char completion[512], reply[1024];
        size_t length;
        assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 0));
        assert(!kobox_drm_query_prepare(&query, binding.generation,
            binding.session_id, KB2_GPU_QUEUE_DISPLAY, translation.command,
            translation.command_size, &region));
        assert(!kobox_drm_query_execute(&query, &api, (void *)&calls,
            NULL, 0, NULL, 0, completion, sizeof(completion), &length));
        kb2_protocol_message_envelope_t envelope = {
            .protocol_id = KB2_GPU_PROTOCOL_ID, .opcode = KB2_GPU_OPCODE_COMMAND,
            .generation = binding.generation, .correlation_id = 1,
            .payload_length = length};
        size_t size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + length;
        assert(!kb2_protocol_message_envelope_encode(reply, size, &envelope));
        memcpy(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, completion, length);
        assert(gpud_drm_ioctl_reply(&request, &translation, 1, reply, size,
            NULL, 0) == (i == 2 ? -EACCES : 0));
        memcpy(&magic, request.data, 4);
        if (!i) assert(magic == 0x1234);
    }
}

int main(void) {
    magic_execution();
    struct gpud_drm_binding binding = {.generation = 9, .frontend_handle = 8, .session_id = 7};
    struct kobox_drm_query_api api = {
        .version = version,
        .get_cap = get_cap,
        .set_client_cap = set_client_cap,
        .virtgpu_get_caps = virtgpu_get_caps,
        .virtgpu_context_init = virtgpu_context_init,
        .virtgpu_execbuffer = virtgpu_execbuffer,
    };
    const kb2_gpu_region_t region = {
        .region_id = 1, .rights = KB2_GPU_SPAN_RIGHT_WRITE, .length = 224};
    for (unsigned int test = 0; test < 7; ++test) {
        gpud_drm_ioctl_request_t request = {.handle = binding.frontend_handle};
        if (test < 3) {
            gpud_drm_version_wire_t wire = {.name_capacity = test == 0 ? 64 : test == 1 ? 1 : 0};
            request.request = UINT64_C(0xc0406400);
            request.arg_size = 64;
            request.data_size = sizeof(wire);
            memcpy(request.data, &wire, sizeof(wire));
        } else if (test < 6) {
            uint64_t data[2] = {test == 4 ? UINT64_MAX : 5, 0};
            request.request = test == 5 ? UINT64_C(0x4010640d) : UINT64_C(0xc010640c);
            request.arg_size = request.data_size = sizeof(data);
            memcpy(request.data, data, sizeof(data));
        } else {
            gpud_drm_virtgpu_get_caps_t caps = {
                .cap_set_id = 1, .cap_set_ver = 2, .size = 64};
            request.request = GPUD_DRM_IOCTL_VIRTGPU_GET_CAPS;
            request.arg_size = request.data_size = sizeof(caps);
            request.aux_size = caps.size;
            memcpy(request.data, &caps, sizeof(caps));
        }
        struct gpud_drm_translation translation;
        assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, region.region_id));
        struct kobox_drm_query query, sentinel;
        memset(&query, 0xa5, sizeof(query));
        sentinel = query;
        assert(kobox_drm_query_prepare(&query,
                                       binding.generation + 1,
                                       binding.session_id,
                                       KB2_GPU_QUEUE_EXECUTION,
                                       translation.command,
                                       translation.command_size,
                                       &region) == -EPROTO);
        assert(!memcmp(&query, &sentinel, sizeof(query)));
        assert(kobox_drm_query_prepare(&query,
                                       binding.generation,
                                       binding.session_id + 1,
                                       KB2_GPU_QUEUE_EXECUTION,
                                       translation.command,
                                       translation.command_size,
                                       &region) == -EPROTO);
        assert(!kobox_drm_query_prepare(&query,
                                        binding.generation,
                                        binding.session_id,
                                        KB2_GPU_QUEUE_EXECUTION,
                                        translation.command,
                                        translation.command_size,
                                        &region));
        struct gpud_drm_translation admitted = translation;
        memset(translation.command, 0xcc, sizeof(translation.command));
        unsigned char output[224] = {0}, aux[64] = {0}, completion[512];
        unsigned char *aux_pointer = test == 6 ? aux : NULL;
        size_t aux_size = test == 6 ? sizeof(aux) : 0;
        size_t size = 0;
        unsigned int before = calls;
        assert(kobox_drm_query_execute(
                   &query, &api, (void *)&calls, output, sizeof(output),
                   aux_pointer, aux_size, completion, 1, &size) ==
               -EINVAL);
        assert(calls == before);
        assert(!kobox_drm_query_execute(&query,
                                        &api,
                                        (void *)&calls,
                                        output,
                                        sizeof(output),
                                        aux_pointer,
                                        aux_size,
                                        completion,
                                        sizeof(completion),
                                        &size));
        assert(calls == before + 1);
        kb2_gpu_inline_completion_t decoded;
        assert(!kb2_gpu_inline_completion_decode(completion, size, binding.session_id, &decoded));
        if (test < 3) {
            assert(decoded.status == KB2_GPU_STATUS_OK && decoded.length == 32 &&
                   decoded.record_schema_id == KB2_GPU_DRM_CORE_RECORD_VERSION_RESULT);
            assert(decoded.data[0] == 5 && decoded.data[16] == 17);
            assert(output[0] == (test == 2 ? 0 : 'q'));
            assert(output[1] == (test == 0 ? 'u' : 0));
        } else if (test == 3) {
            assert(decoded.status == KB2_GPU_STATUS_OK && decoded.length == 8);
            assert(decoded.data[0] == 0x21 && decoded.data[7] == 0x12);
        } else if (test < 6) {
            assert(decoded.status ==
                   (test == 4 ? KB2_GPU_STATUS_INVALID : KB2_GPU_STATUS_OK));
            assert(!decoded.length && !decoded.record_schema_id);
        } else {
            assert(decoded.status == KB2_GPU_STATUS_OK &&
                   decoded.record_schema_id == KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_RESULT &&
                   decoded.length == KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_RESULT_SIZE &&
                   decoded.data[0] == sizeof(aux) && decoded.data[4] == sizeof(aux) &&
                   aux[0] == 1 && aux[1] == 2);
        }
        codec_errors(completion, size, binding.session_id);
        unsigned char reply[1024];
        kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
                                                    .opcode = KB2_GPU_OPCODE_COMMAND,
                                                    .generation = binding.generation + 1,
                                                    .correlation_id = 99,
                                                    .payload_length = size};
        size_t reply_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + size;
        assert(!kb2_protocol_message_envelope_encode(reply, reply_size, &envelope));
        memcpy(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, completion, size);
        gpud_drm_ioctl_request_t original = request;
        assert(gpud_drm_ioctl_reply(
                   &request, &admitted, 99, reply, reply_size, output, sizeof(output)) == -EPROTO);
        assert(!memcmp(&request, &original, sizeof(request)));
        envelope.generation = binding.generation;
        assert(!kb2_protocol_message_envelope_encode(reply, reply_size, &envelope));
        assert(gpud_drm_ioctl_reply(
                   &request, &admitted, 100, reply, reply_size, output, sizeof(output)) == -EPROTO);
        int result = gpud_drm_ioctl_reply(
            &request, &admitted, 99, reply, reply_size, output, sizeof(output));
        if (test == 4) {
            assert(result == -EINVAL);
            assert(!memcmp(&request, &original, sizeof(request)));
        } else {
            assert(!result);
            if (test < 3) {
                gpud_drm_version_wire_t version;
                memcpy(&version, request.data, sizeof(version));
                assert(version.major == 5 && version.name_length == 17);
                assert(version.name[0] == (test == 2 ? 0 : 'q'));
                gpud_drm_ioctl_request_t unchanged = request;
                size_t record_offset = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE +
                                       KB2_GPU_COMPLETION_HEADER_SIZE +
                                       KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE;
                reply[record_offset + 12] = 1;
                assert(gpud_drm_ioctl_reply(
                           &request, &admitted, 99, reply, reply_size, output, sizeof(output)) ==
                       -EPROTO);
                assert(!memcmp(&request, &unchanged, sizeof(request)));
                reply[record_offset + 12] = 0;
                size_t id_offset = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE +
                                   KB2_GPU_COMPLETION_HEADER_SIZE +
                                   KB2_GPU_ARGUMENT_DESCRIPTOR_RECORD_SCHEMA_ID_OFFSET;
                reply[id_offset] ^= 1;
                assert(gpud_drm_ioctl_reply(
                           &request, &admitted, 99, reply, reply_size, output, sizeof(output)) ==
                       -EPROTO);
                assert(!memcmp(&request, &unchanged, sizeof(request)));
            } else if (test == 3) {
                uint64_t value;
                memcpy(&value, request.data + 8, sizeof(value));
                assert(value == UINT64_C(0x1234567887654321));
            } else {
                assert(!memcmp(&request, &original, sizeof(request)));
            }
        }
    }
    native_transport_case(5, 0);
    native_transport_case(UINT64_MAX, -EINVAL);
    native_transport_case(6, -EIO);
    private_aux_lifetime();
    virtgpu_input_execution();
    display_execution();
    cursor_execution();
    for (unsigned int defect = 0; defect <= 7; ++defect)
        native_queue_case(defect);
    session_service_cases();
    native_fence_events();
    puts("gpud GPU query pipeline: PASS encode, private plan, typed dispatch, canonical "
         "completion, loss admission");
    return 0;
}

#include "drm_kms.h"

#include <kobox/module.h>
#include <kobox/shim.h>
#include "linux_subsystem/dma/dma.h"
#include "loader/module_context.h"

#include <pacha/abi.h>
#include <pacha/ipc.h>

#include <stdio.h>
#include <string.h>

enum {
    DRMD_KMS_DUMB_MAX = 16,
    DRMD_KMS_FB_MAX = 16,
    DRMD_KMS_WIDTH = 1024,
    DRMD_KMS_HEIGHT = 768,
    DRMD_KMS_MAX_WIDTH = 8192,
    DRMD_KMS_MAX_HEIGHT = 8192,
    DRMD_KMS_CRTC_ID = 51,
    DRMD_KMS_CONNECTOR_ID = 31,
    DRMD_KMS_ENCODER_ID = 41,
    DRMD_KMS_PLANE_ID = 61,
    DRMD_VIRTIO_GPU_OBJECT_BYTES = 0x2c0,
    DRMD_GEM_DEV_OFFSET = 0x08,
};

typedef struct drmd_virtio_gpu_object_params {
    unsigned long size;
    unsigned char dumb;
    unsigned char virgl;
    unsigned char blob;
    unsigned char pad0;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t target;
    uint32_t bind;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t ctx_id;
    uint32_t blob_mem;
    uint32_t blob_flags;
    uint32_t pad1;
    uint64_t blob_id;
} drmd_virtio_gpu_object_params_t;

typedef struct drmd_virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} drmd_virtio_gpu_mem_entry_t;

typedef int (*drmd_resource_id_get_fn)(void *, uint32_t *);
typedef void (*drmd_create_resource_fn)(void *, void *, void *, void *, void *);
typedef void (*drmd_object_attach_fn)(void *, void *, void *, unsigned int);
typedef void (*drmd_set_scanout_fn)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (*drmd_transfer_2d_fn)(void *, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, void *, void *);
typedef void (*drmd_flush_fn)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void *, void *);
typedef void (*drmd_notify_fn)(void *);
typedef void (*drmd_unref_resource_fn)(void *, void *);
typedef void *(*drmd_array_alloc_fn)(uint32_t);
typedef void (*drmd_array_add_fn)(void *, void *);

typedef struct drmd_kms_dumb {
    int active;
    uint64_t owner;
    uint32_t handle;
    uint32_t resource_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t fb_refs;
    uint64_t size;
    uint64_t mmap_offset;
    int vmo_fd;
    void *mapping;
    uint64_t dma_addr;
    void *object;
} drmd_kms_dumb_t;

typedef struct drmd_kms_fb {
    int active;
    uint64_t owner;
    uint32_t id;
    uint32_t handle;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
} drmd_kms_fb_t;

typedef struct drmd_kms_state {
    int ready;
    uint64_t master_handle;
    uint32_t next_handle;
    uint32_t next_fb;
    uint32_t current_fb;
    drmd_modeinfo_t current_mode;
    void *drm_device;
    void *vgdev;
    kb_module_t *module;
    drmd_resource_id_get_fn resource_id_get;
    drmd_create_resource_fn create_resource;
    drmd_object_attach_fn object_attach;
    drmd_set_scanout_fn set_scanout;
    drmd_transfer_2d_fn transfer_2d;
    drmd_flush_fn flush;
    drmd_notify_fn notify;
    drmd_unref_resource_fn unref_resource;
    drmd_array_alloc_fn array_alloc;
    drmd_array_add_fn array_add;
    drmd_kms_dumb_t dumb[DRMD_KMS_DUMB_MAX];
    drmd_kms_fb_t fb[DRMD_KMS_FB_MAX];
} drmd_kms_state_t;

typedef struct drmd_kms_owner_context {
    unsigned long old_gs;
    kb_module_t *previous_owner;
    int active;
} drmd_kms_owner_context_t;

static drmd_kms_state_t kms;

static int enter_module(drmd_kms_owner_context_t *context)
{
    memset(context, 0, sizeof(*context));
    context->previous_owner = kb_loader_active_module();
    if (kms.module == NULL || kb_loader_enter_module_context(kms.module, &context->old_gs) != KB_OK) {
        return 0;
    }
    kb_loader_set_active_module(kms.module);
    context->active = 1;
    return 1;
}

static void leave_module(const drmd_kms_owner_context_t *context)
{
    if (context != NULL && context->active) {
        kb_loader_leave_module_context(context->old_gs);
        kb_loader_set_active_module(context->previous_owner);
    }
}

static int find_symbol(const char *name, void **out)
{
    return kb_module_find_symbol(kms.module, name, out) == KB_OK && *out != NULL ? 0 : -2;
}

static void pump_device(void)
{
    for (unsigned i = 0; i < 32; i++) {
        (void)kb_handle_any_irq(0);
        kb_run_deferred_work();
    }
}

static void fill_mode(drmd_modeinfo_t *mode)
{
    memset(mode, 0, sizeof(*mode));
    mode->clock = 65000;
    mode->hdisplay = DRMD_KMS_WIDTH;
    mode->hsync_start = 1048;
    mode->hsync_end = 1184;
    mode->htotal = 1344;
    mode->vdisplay = DRMD_KMS_HEIGHT;
    mode->vsync_start = 771;
    mode->vsync_end = 777;
    mode->vtotal = 806;
    mode->vrefresh = 60;
    mode->type = DRMD_MODE_TYPE_DRIVER | DRMD_MODE_TYPE_PREFERRED;
    memcpy(mode->name, "1024x768", 9);
}

int drmd_kms_init(struct drmd_drm_island *island)
{
    if (island == NULL || island->loaded_module_count < 2) {
        return -22;
    }
    memset(&kms, 0, sizeof(kms));
    kms.next_handle = 1;
    kms.next_fb = 71;
    kms.drm_device = kb_drm_primary_device();
    kms.vgdev = kb_drm_device_private(kms.drm_device);
    fill_mode(&kms.current_mode);
    for (uint32_t i = 0; i < island->loaded_module_count; i++) {
        kb_module_t *candidate = (kb_module_t *)island->modules[i];
        void *symbol = NULL;
        if (kb_module_find_symbol(candidate, "virtio_gpu_resource_id_get", &symbol) == KB_OK && symbol != NULL) {
            kms.module = candidate;
            break;
        }
    }
    if (kms.drm_device == NULL || kms.vgdev == NULL || kms.module == NULL ||
        find_symbol("virtio_gpu_resource_id_get", (void **)&kms.resource_id_get) != 0 ||
        find_symbol("virtio_gpu_cmd_create_resource", (void **)&kms.create_resource) != 0 ||
        find_symbol("virtio_gpu_object_attach", (void **)&kms.object_attach) != 0 ||
        find_symbol("virtio_gpu_cmd_set_scanout", (void **)&kms.set_scanout) != 0 ||
        find_symbol("virtio_gpu_cmd_transfer_to_host_2d", (void **)&kms.transfer_2d) != 0 ||
        find_symbol("virtio_gpu_cmd_resource_flush", (void **)&kms.flush) != 0 ||
        find_symbol("virtio_gpu_notify", (void **)&kms.notify) != 0 ||
        find_symbol("virtio_gpu_cmd_unref_resource", (void **)&kms.unref_resource) != 0 ||
        find_symbol("virtio_gpu_array_alloc", (void **)&kms.array_alloc) != 0 ||
        find_symbol("virtio_gpu_array_add_obj", (void **)&kms.array_add) != 0) {
        return -19;
    }
    kms.ready = 1;
    printf("[drmd] kms ready mode=%ux%u connector=%u crtc=%u plane=%u\n",
        DRMD_KMS_WIDTH, DRMD_KMS_HEIGHT,
        DRMD_KMS_CONNECTOR_ID, DRMD_KMS_CRTC_ID, DRMD_KMS_PLANE_ID);
    return 0;
}

static drmd_kms_dumb_t *find_dumb(uint64_t owner, uint32_t handle)
{
    for (size_t i = 0; i < DRMD_KMS_DUMB_MAX; i++) {
        if (kms.dumb[i].active && kms.dumb[i].owner == owner && kms.dumb[i].handle == handle) {
            return &kms.dumb[i];
        }
    }
    return NULL;
}

static drmd_kms_dumb_t *find_dumb_offset(uint64_t owner, uint64_t offset, uint64_t length)
{
    for (size_t i = 0; i < DRMD_KMS_DUMB_MAX; i++) {
        drmd_kms_dumb_t *dumb = &kms.dumb[i];
        if (dumb->active && dumb->owner == owner && dumb->mmap_offset == offset && length <= dumb->size) {
            return dumb;
        }
    }
    return NULL;
}

static drmd_kms_fb_t *find_fb(uint64_t owner, uint32_t id)
{
    for (size_t i = 0; i < DRMD_KMS_FB_MAX; i++) {
        if (kms.fb[i].active && kms.fb[i].owner == owner && kms.fb[i].id == id) {
            return &kms.fb[i];
        }
    }
    return NULL;
}

static int create_dumb(struct drmd_drm_island *island, uint64_t owner, drmd_mode_create_dumb_t *args)
{
    if (args == NULL || args->flags != 0 || args->bpp != 32 || args->width == 0 || args->height == 0 ||
        args->width > DRMD_KMS_MAX_WIDTH || args->height > DRMD_KMS_MAX_HEIGHT) {
        return -22;
    }
    const uint64_t pitch = (uint64_t)args->width * 4u;
    uint64_t size = pitch * args->height;
    size = (size + 4095u) & ~4095ull;
    if (size == 0 || size > 256u * 1024u * 1024u) {
        return -12;
    }
    drmd_kms_dumb_t *dumb = NULL;
    for (size_t i = 0; i < DRMD_KMS_DUMB_MAX; i++) {
        if (!kms.dumb[i].active) { dumb = &kms.dumb[i]; break; }
    }
    if (dumb == NULL) {
        return -24;
    }
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const int vmo_fd = pacha_vmo_create(size, rights, 0);
    if (vmo_fd < 16) {
        return -12;
    }
    void *mapping = pacha_mmap(vmo_fd, size, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (mapping == NULL) {
        (void)pacha_fd_close(vmo_fd);
        return -12;
    }
    kb_status_t dma_status = KB_ERR_INVALID;
    kb_device_backend_t *backend = (kb_device_backend_t *)island->device_backend;
    const uint64_t mapped_dma = kb_subsystem_dma_map(backend, NULL, mapping, size, KB_DMA_TO_DEVICE, &dma_status);
    if (dma_status != KB_OK || mapped_dma == 0) {
        (void)pacha_munmap(mapping, size);
        (void)pacha_fd_close(vmo_fd);
        return -5;
    }
    void *object = kb_kzalloc(DRMD_VIRTIO_GPU_OBJECT_BYTES, 0);
    if (object == NULL) {
        kb_subsystem_dma_unmap(backend, NULL, mapped_dma, size, KB_DMA_TO_DEVICE);
        (void)pacha_munmap(mapping, size);
        (void)pacha_fd_close(vmo_fd);
        return -12;
    }
    *(uint32_t *)object = 1;
    memcpy((uint8_t *)object + DRMD_GEM_DEV_OFFSET, &kms.drm_device, sizeof(kms.drm_device));
    uint32_t resource_id = 0;
    drmd_kms_owner_context_t context;
    const int entered = enter_module(&context);
    int status = kms.resource_id_get(kms.vgdev, &resource_id);
    if (status == 0) {
        drmd_virtio_gpu_object_params_t params;
        memset(&params, 0, sizeof(params));
        params.size = size;
        params.dumb = 1;
        params.format = 2;
        params.width = args->width;
        params.height = args->height;
        memcpy((uint8_t *)object + 0x198, &resource_id, sizeof(resource_id));
        kms.create_resource(kms.vgdev, object, &params, NULL, NULL);
        drmd_virtio_gpu_mem_entry_t *entry = kb_kzalloc(sizeof(*entry), 0);
        if (entry == NULL) {
            status = -12;
        } else {
            entry->addr = mapped_dma;
            entry->length = (uint32_t)size;
            kms.object_attach(kms.vgdev, object, entry, 1);
            kms.notify(kms.vgdev);
        }
    }
    if (entered) leave_module(&context);
    pump_device();
    if (status != 0) {
        kb_kfree(object);
        kb_subsystem_dma_unmap(backend, NULL, mapped_dma, size, KB_DMA_TO_DEVICE);
        (void)pacha_munmap(mapping, size);
        (void)pacha_fd_close(vmo_fd);
        return status;
    }
    memset(dumb, 0, sizeof(*dumb));
    dumb->active = 1;
    dumb->owner = owner;
    dumb->handle = kms.next_handle++;
    dumb->resource_id = resource_id;
    dumb->width = args->width;
    dumb->height = args->height;
    dumb->pitch = (uint32_t)pitch;
    dumb->size = size;
    dumb->mmap_offset = (uint64_t)dumb->handle << 32u;
    dumb->vmo_fd = vmo_fd;
    dumb->mapping = mapping;
    dumb->dma_addr = mapped_dma;
    dumb->object = object;
    args->handle = dumb->handle;
    args->pitch = dumb->pitch;
    args->size = dumb->size;
    return 0;
}

static int add_fb(uint64_t owner, uint32_t handle, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format, uint32_t *out_id)
{
    drmd_kms_dumb_t *dumb = find_dumb(owner, handle);
    if (dumb == NULL || width == 0 || height == 0 || width > dumb->width || height > dumb->height ||
        pitch != dumb->pitch || format != DRMD_FORMAT_XRGB8888 || out_id == NULL) {
        return -22;
    }
    for (size_t i = 0; i < DRMD_KMS_FB_MAX; i++) {
        if (!kms.fb[i].active) {
            drmd_kms_fb_t *fb = &kms.fb[i];
            memset(fb, 0, sizeof(*fb));
            fb->active = 1;
            fb->owner = owner;
            fb->id = kms.next_fb++;
            fb->handle = handle;
            fb->width = width;
            fb->height = height;
            fb->pitch = pitch;
            fb->format = format;
            dumb->fb_refs++;
            *out_id = fb->id;
            return 0;
        }
    }
    return -24;
}

static int scanout_fb(uint64_t owner, drmd_kms_fb_t *fb)
{
    drmd_kms_dumb_t *dumb = fb == NULL ? NULL : find_dumb(owner, fb->handle);
    if (dumb == NULL) {
        return -2;
    }
    drmd_kms_owner_context_t context;
    const int entered = enter_module(&context);
    void *objects = kms.array_alloc(1);
    if (objects == NULL) {
        if (entered) leave_module(&context);
        return -12;
    }
    kms.array_add(objects, dumb->object);
    kms.transfer_2d(kms.vgdev, 0, fb->width, fb->height, 0, 0, objects, NULL);
    kms.set_scanout(kms.vgdev, 0, dumb->resource_id, fb->width, fb->height, 0, 0);
    kms.flush(kms.vgdev, dumb->resource_id, 0, 0, fb->width, fb->height, NULL, NULL);
    kms.notify(kms.vgdev);
    if (entered) leave_module(&context);
    pump_device();
    kms.current_fb = fb->id;
    return 0;
}

static int require_master(uint64_t handle)
{
    return kms.master_handle == handle ? 0 : -13;
}

static int ioctl_resources(drmd_kms_resources_wire_t *wire)
{
    if (wire == NULL) return -22;
    const uint32_t fb_capacity = wire->value.count_fbs;
    const uint32_t crtc_capacity = wire->value.count_crtcs;
    const uint32_t connector_capacity = wire->value.count_connectors;
    const uint32_t encoder_capacity = wire->value.count_encoders;
    uint32_t fb_count = 0;
    for (size_t i = 0; i < DRMD_KMS_FB_MAX; i++) {
        if (!kms.fb[i].active) continue;
        if (fb_count < fb_capacity && fb_count < DRMD_KMS_FB_CAPACITY) wire->fbs[fb_count] = kms.fb[i].id;
        fb_count++;
    }
    if (crtc_capacity != 0) wire->crtcs[0] = DRMD_KMS_CRTC_ID;
    if (connector_capacity != 0) wire->connectors[0] = DRMD_KMS_CONNECTOR_ID;
    if (encoder_capacity != 0) wire->encoders[0] = DRMD_KMS_ENCODER_ID;
    wire->value.count_fbs = fb_count;
    wire->value.count_crtcs = 1;
    wire->value.count_connectors = 1;
    wire->value.count_encoders = 1;
    wire->value.min_width = 32;
    wire->value.max_width = DRMD_KMS_MAX_WIDTH;
    wire->value.min_height = 32;
    wire->value.max_height = DRMD_KMS_MAX_HEIGHT;
    return 0;
}

static int ioctl_connector(drmd_kms_connector_wire_t *wire)
{
    if (wire == NULL || wire->value.connector_id != DRMD_KMS_CONNECTOR_ID) return -2;
    const uint32_t mode_capacity = wire->value.count_modes;
    const uint32_t encoder_capacity = wire->value.count_encoders;
    if (mode_capacity != 0) wire->modes[0] = kms.current_mode;
    if (encoder_capacity != 0) wire->encoders[0] = DRMD_KMS_ENCODER_ID;
    wire->value.count_modes = 1;
    wire->value.count_props = 0;
    wire->value.count_encoders = 1;
    wire->value.encoder_id = DRMD_KMS_ENCODER_ID;
    wire->value.connector_type = DRMD_MODE_CONNECTOR_VIRTUAL;
    wire->value.connector_type_id = 1;
    wire->value.connection = DRMD_MODE_CONNECTED;
    wire->value.mm_width = 271;
    wire->value.mm_height = 203;
    wire->value.subpixel = DRMD_MODE_SUBPIXEL_UNKNOWN;
    return 0;
}

int drmd_kms_ioctl(struct drmd_drm_island *island, drmd_ioctl_request_t *request, int *out_handled)
{
    if (out_handled == NULL || request == NULL || !kms.ready) return -22;
    *out_handled = 1;
    switch ((uint32_t)request->request) {
    case DRMD_IOCTL_SET_MASTER:
        if (kms.master_handle != 0 && kms.master_handle != request->handle) return -16;
        kms.master_handle = request->handle;
        return 0;
    case DRMD_IOCTL_DROP_MASTER:
        if (kms.master_handle != request->handle) return -22;
        kms.master_handle = 0;
        return 0;
    case DRMD_IOCTL_MODE_GETRESOURCES:
        return request->data_size >= sizeof(drmd_kms_resources_wire_t) ? ioctl_resources((void *)request->data) : -22;
    case DRMD_IOCTL_MODE_GETCONNECTOR:
        return request->data_size >= sizeof(drmd_kms_connector_wire_t) ? ioctl_connector((void *)request->data) : -22;
    case DRMD_IOCTL_MODE_GETENCODER: {
        if (request->data_size < sizeof(drmd_mode_get_encoder_t)) return -22;
        drmd_mode_get_encoder_t *encoder = (void *)request->data;
        if (encoder->encoder_id != DRMD_KMS_ENCODER_ID) return -2;
        encoder->encoder_type = DRMD_MODE_ENCODER_VIRTUAL;
        encoder->crtc_id = kms.current_fb != 0 ? DRMD_KMS_CRTC_ID : 0;
        encoder->possible_crtcs = 1;
        encoder->possible_clones = 0;
        return 0;
    }
    case DRMD_IOCTL_MODE_GETCRTC: {
        if (request->data_size < sizeof(drmd_kms_crtc_wire_t)) return -22;
        drmd_kms_crtc_wire_t *wire = (void *)request->data;
        if (wire->value.crtc_id != DRMD_KMS_CRTC_ID) return -2;
        wire->value.fb_id = kms.current_fb;
        wire->value.x = 0;
        wire->value.y = 0;
        wire->value.gamma_size = 0;
        wire->value.mode_valid = kms.current_fb != 0;
        wire->value.mode = kms.current_mode;
        return 0;
    }
    case DRMD_IOCTL_MODE_SETCRTC: {
        if (require_master(request->handle) != 0 || request->data_size < sizeof(drmd_kms_crtc_wire_t)) return -13;
        drmd_kms_crtc_wire_t *wire = (void *)request->data;
        if (wire->value.crtc_id != DRMD_KMS_CRTC_ID || wire->value.count_connectors != 1 ||
            wire->connectors[0] != DRMD_KMS_CONNECTOR_ID || !wire->value.mode_valid ||
            wire->value.mode.hdisplay == 0 || wire->value.mode.vdisplay == 0) return -22;
        drmd_kms_fb_t *fb = find_fb(request->handle, wire->value.fb_id);
        if (fb == NULL || fb->width != wire->value.mode.hdisplay || fb->height != wire->value.mode.vdisplay) return -22;
        kms.current_mode = wire->value.mode;
        return scanout_fb(request->handle, fb);
    }
    case DRMD_IOCTL_MODE_CREATE_DUMB:
        return request->data_size >= sizeof(drmd_mode_create_dumb_t) ? create_dumb(island, request->handle, (void *)request->data) : -22;
    case DRMD_IOCTL_MODE_MAP_DUMB: {
        if (request->data_size < sizeof(drmd_mode_map_dumb_t)) return -22;
        drmd_mode_map_dumb_t *map = (void *)request->data;
        drmd_kms_dumb_t *dumb = find_dumb(request->handle, map->handle);
        if (dumb == NULL) return -2;
        map->offset = dumb->mmap_offset;
        return 0;
    }
    case DRMD_IOCTL_MODE_ADDFB: {
        if (request->data_size < sizeof(drmd_mode_fb_cmd_t)) return -22;
        drmd_mode_fb_cmd_t *fb = (void *)request->data;
        return add_fb(request->handle, fb->handle, fb->width, fb->height, fb->pitch,
            fb->bpp == 32 && fb->depth == 24 ? DRMD_FORMAT_XRGB8888 : 0, &fb->fb_id);
    }
    case DRMD_IOCTL_MODE_ADDFB2: {
        if (request->data_size < sizeof(drmd_mode_fb_cmd2_t)) return -22;
        drmd_mode_fb_cmd2_t *fb = (void *)request->data;
        if (fb->flags != 0 || fb->offsets[0] != 0 || fb->handles[1] != 0) return -22;
        return add_fb(request->handle, fb->handles[0], fb->width, fb->height, fb->pitches[0], fb->pixel_format, &fb->fb_id);
    }
    case DRMD_IOCTL_MODE_RMFB: {
        if (request->data_size < sizeof(uint32_t)) return -22;
        const uint32_t id = *(uint32_t *)request->data;
        drmd_kms_fb_t *fb = find_fb(request->handle, id);
        if (fb == NULL) return -2;
        if (kms.current_fb == id) return -16;
        drmd_kms_dumb_t *dumb = find_dumb(request->handle, fb->handle);
        if (dumb != NULL && dumb->fb_refs != 0) dumb->fb_refs--;
        memset(fb, 0, sizeof(*fb));
        return 0;
    }
    case DRMD_IOCTL_MODE_PAGE_FLIP: {
        if (require_master(request->handle) != 0 || request->data_size < sizeof(drmd_mode_crtc_page_flip_t)) return -13;
        drmd_mode_crtc_page_flip_t *flip = (void *)request->data;
        if (flip->crtc_id != DRMD_KMS_CRTC_ID || flip->flags != 0 || flip->reserved != 0) return -22;
        drmd_kms_fb_t *fb = find_fb(request->handle, flip->fb_id);
        return fb == NULL ? -2 : scanout_fb(request->handle, fb);
    }
    case DRMD_IOCTL_MODE_GETPLANERESOURCES: {
        if (request->data_size < sizeof(drmd_kms_plane_res_wire_t)) return -22;
        drmd_kms_plane_res_wire_t *wire = (void *)request->data;
        if (wire->value.count_planes != 0) wire->planes[0] = DRMD_KMS_PLANE_ID;
        wire->value.count_planes = 1;
        return 0;
    }
    case DRMD_IOCTL_MODE_GETPLANE: {
        if (request->data_size < sizeof(drmd_kms_plane_wire_t)) return -22;
        drmd_kms_plane_wire_t *wire = (void *)request->data;
        if (wire->value.plane_id != DRMD_KMS_PLANE_ID) return -2;
        if (wire->value.count_format_types != 0) wire->formats[0] = DRMD_FORMAT_XRGB8888;
        wire->value.crtc_id = kms.current_fb != 0 ? DRMD_KMS_CRTC_ID : 0;
        wire->value.fb_id = kms.current_fb;
        wire->value.possible_crtcs = 1;
        wire->value.gamma_size = 0;
        wire->value.count_format_types = 1;
        return 0;
    }
    case DRMD_IOCTL_MODE_DESTROY_DUMB: {
        if (request->data_size < sizeof(drmd_mode_destroy_dumb_t)) return -22;
        drmd_mode_destroy_dumb_t *destroy = (void *)request->data;
        drmd_kms_dumb_t *dumb = find_dumb(request->handle, destroy->handle);
        if (dumb == NULL) return -2;
        if (dumb->fb_refs != 0) return -16;
        drmd_kms_owner_context_t context;
        const int entered = enter_module(&context);
        kms.unref_resource(kms.vgdev, dumb->object);
        kms.notify(kms.vgdev);
        if (entered) leave_module(&context);
        pump_device();
        kb_subsystem_dma_unmap((kb_device_backend_t *)island->device_backend, NULL, dumb->dma_addr, dumb->size, KB_DMA_TO_DEVICE);
        (void)pacha_munmap(dumb->mapping, dumb->size);
        (void)pacha_fd_close(dumb->vmo_fd);
        kb_kfree(dumb->object);
        memset(dumb, 0, sizeof(*dumb));
        return 0;
    }
    default:
        *out_handled = 0;
        return 0;
    }
}

int drmd_kms_mmap(struct drmd_drm_island *island, const drmd_mmap_request_t *request, int *out_vmo_fd)
{
    (void)island;
    if (!kms.ready || request == NULL || out_vmo_fd == NULL || request->length == 0) return -22;
    drmd_kms_dumb_t *dumb = find_dumb_offset(request->handle, request->offset, request->length);
    if (dumb == NULL) return -6;
    *out_vmo_fd = dumb->vmo_fd;
    return 0;
}

void drmd_kms_handle_open(uint64_t handle)
{
    if (kms.master_handle == 0) kms.master_handle = handle;
}

void drmd_kms_handle_close(struct drmd_drm_island *island, uint64_t handle)
{
    if (kms.master_handle == handle) kms.master_handle = 0;
    int disable_scanout = 0;
    for (size_t i = 0; i < DRMD_KMS_FB_MAX; i++) {
        drmd_kms_fb_t *fb = &kms.fb[i];
        if (!fb->active || fb->owner != handle) continue;
        if (kms.current_fb == fb->id) {
            kms.current_fb = 0;
            disable_scanout = 1;
        }
        drmd_kms_dumb_t *dumb = find_dumb(handle, fb->handle);
        if (dumb != NULL && dumb->fb_refs != 0) dumb->fb_refs--;
        memset(fb, 0, sizeof(*fb));
    }
    drmd_kms_owner_context_t context;
    const int entered = enter_module(&context);
    if (disable_scanout) {
        kms.set_scanout(kms.vgdev, 0, 0, 0, 0, 0, 0);
    }
    for (size_t i = 0; i < DRMD_KMS_DUMB_MAX; i++) {
        drmd_kms_dumb_t *dumb = &kms.dumb[i];
        if (dumb->active && dumb->owner == handle) {
            kms.unref_resource(kms.vgdev, dumb->object);
        }
    }
    kms.notify(kms.vgdev);
    if (entered) leave_module(&context);
    pump_device();
    for (size_t i = 0; i < DRMD_KMS_DUMB_MAX; i++) {
        drmd_kms_dumb_t *dumb = &kms.dumb[i];
        if (!dumb->active || dumb->owner != handle) continue;
        kb_subsystem_dma_unmap((kb_device_backend_t *)island->device_backend, NULL,
            dumb->dma_addr, dumb->size, KB_DMA_TO_DEVICE);
        (void)pacha_munmap(dumb->mapping, dumb->size);
        (void)pacha_fd_close(dumb->vmo_fd);
        kb_kfree(dumb->object);
        memset(dumb, 0, sizeof(*dumb));
    }
}

void drmd_kms_get_state_counts(drmd_kms_state_counts_t *out_counts)
{
    if (out_counts == NULL) return;
    memset(out_counts, 0, sizeof(*out_counts));
    out_counts->master_handle = kms.master_handle;
    for (size_t i = 0; i < DRMD_KMS_FB_MAX; i++) {
        out_counts->fb += kms.fb[i].active ? 1u : 0u;
    }
    for (size_t i = 0; i < DRMD_KMS_DUMB_MAX; i++) {
        out_counts->dumb += kms.dumb[i].active ? 1u : 0u;
    }
}

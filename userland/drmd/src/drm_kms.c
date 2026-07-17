#include "drm_kms.h"

#include <kobox/module.h>
#include <kobox/shim.h>
#include "linux_subsystem/dma/dma.h"
#include "loader/module_context.h"

#include <pacha/abi.h>
#include <pacha/ipc.h>

#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

enum {
    DRMD_KMS_BUFFER_MAX = 16,
    DRMD_KMS_GEM_HANDLE_MAX = 32,
    DRMD_KMS_FB_MAX = 16,
    DRMD_KMS_EVENT_FILE_MAX = 32,
    DRMD_KMS_EVENT_QUEUE_MAX = 16,
    DRMD_KMS_WIDTH = 1024,
    DRMD_KMS_HEIGHT = 768,
    DRMD_KMS_MAX_WIDTH = 8192,
    DRMD_KMS_MAX_HEIGHT = 8192,
    DRMD_KMS_CRTC_ID = 51,
    DRMD_KMS_CONNECTOR_ID = 31,
    DRMD_KMS_ENCODER_ID = 41,
    DRMD_KMS_PLANE_ID = 61,
    DRMD_KMS_DPMS_PROP_ID = 101,
    DRMD_KMS_PLANE_TYPE_PROP_ID = 102,
    DRMD_KMS_IN_FORMATS_PROP_ID = 103,
    DRMD_KMS_IN_FORMATS_BLOB_ID = 201,
    DRMD_VIRTIO_GPU_OBJECT_BYTES = 0x2c0,
    DRMD_GEM_DEV_OFFSET = 0x08,
    DRMD_POLLIN = 0x0001,
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

typedef struct drmd_kms_buffer {
    int active;
    int cached;
    int reusable;
    uint64_t token;
    uint32_t resource_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t handle_refs;
    uint32_t export_refs;
    uint32_t fb_refs;
    uint64_t export_owner;
    uint64_t size;
    uint64_t mmap_offset;
    int vmo_fd;
    int acquire_sync_fd;
    void *mapping;
    uint64_t dma_addr;
    void *object;
} drmd_kms_buffer_t;

typedef struct drmd_kms_gem_handle {
    int active;
    uint64_t owner;
    uint32_t handle;
    drmd_kms_buffer_t *buffer;
} drmd_kms_gem_handle_t;

typedef struct drmd_kms_fb {
    int active;
    uint64_t owner;
    uint32_t id;
    uint32_t handle;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    drmd_kms_buffer_t *buffer;
} drmd_kms_fb_t;

typedef struct drmd_kms_event_file {
    int active;
    int universal_planes;
    int authenticated;
    uint64_t owner;
    uint32_t magic;
    uint32_t head;
    uint32_t count;
    drmd_event_vblank_t events[DRMD_KMS_EVENT_QUEUE_MAX];
} drmd_kms_event_file_t;

typedef struct drmd_kms_state {
    int ready;
    uint64_t master_handle;
    uint32_t next_handle;
    uint32_t next_fb;
    uint64_t next_token;
    uint32_t next_magic;
    uint32_t current_fb;
    uint32_t sequence;
    uint32_t delivered_flip_events;
    uint32_t dpms;
    drmd_modeinfo_t current_mode;
    kb_device_backend_t *device_backend;
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
    drmd_kms_buffer_t buffers[DRMD_KMS_BUFFER_MAX];
    drmd_kms_gem_handle_t gem_handles[DRMD_KMS_GEM_HANDLE_MAX];
    drmd_kms_fb_t fb[DRMD_KMS_FB_MAX];
    drmd_kms_event_file_t event_files[DRMD_KMS_EVENT_FILE_MAX];
} drmd_kms_state_t;

typedef struct drmd_kms_owner_context {
    unsigned long old_gs;
    kb_module_t *previous_owner;
    int active;
} drmd_kms_owner_context_t;

static drmd_kms_state_t kms;

static void reset_buffer(drmd_kms_buffer_t *buffer)
{
    if (buffer == NULL) return;
    if (buffer->acquire_sync_fd >= 16) {
        (void)pacha_fd_close(buffer->acquire_sync_fd);
    }
    memset(buffer, 0, sizeof(*buffer));
    buffer->acquire_sync_fd = -1;
}

typedef struct drmd_format_modifier_blob {
    uint32_t version;
    uint32_t flags;
    uint32_t count_formats;
    uint32_t formats_offset;
    uint32_t count_modifiers;
    uint32_t modifiers_offset;
    uint32_t format;
    uint32_t pad;
    struct {
        uint64_t formats;
        uint32_t offset;
        uint32_t pad;
        uint64_t modifier;
    } modifier;
} drmd_format_modifier_blob_t;

_Static_assert(sizeof(drmd_format_modifier_blob_t) == 56, "format modifier blob ABI");

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

static drmd_kms_event_file_t *find_event_file(uint64_t owner)
{
    for (size_t i = 0; i < DRMD_KMS_EVENT_FILE_MAX; i++) {
        if (kms.event_files[i].active && kms.event_files[i].owner == owner) {
            return &kms.event_files[i];
        }
    }
    return NULL;
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
    for (size_t i = 0; i < DRMD_KMS_BUFFER_MAX; i++) {
        if (kms.buffers[i].acquire_sync_fd >= 16) {
            (void)pacha_fd_close(kms.buffers[i].acquire_sync_fd);
        }
    }
    memset(&kms, 0, sizeof(kms));
    for (size_t i = 0; i < DRMD_KMS_BUFFER_MAX; i++) {
        kms.buffers[i].acquire_sync_fd = -1;
    }
    kms.next_handle = 1;
    kms.next_fb = 71;
    kms.next_token = 1;
    kms.next_magic = 1;
    kms.dpms = DRMD_MODE_DPMS_ON;
    kms.device_backend = (kb_device_backend_t *)island->device_backend;
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

static drmd_kms_gem_handle_t *find_gem_handle(uint64_t owner, uint32_t handle)
{
    for (size_t i = 0; i < DRMD_KMS_GEM_HANDLE_MAX; i++) {
        if (kms.gem_handles[i].active && kms.gem_handles[i].owner == owner &&
            kms.gem_handles[i].handle == handle) {
            return &kms.gem_handles[i];
        }
    }
    return NULL;
}

static drmd_kms_buffer_t *find_buffer_token(uint64_t token)
{
    for (size_t i = 0; i < DRMD_KMS_BUFFER_MAX; i++) {
        if (kms.buffers[i].active && kms.buffers[i].token == token) {
            return &kms.buffers[i];
        }
    }
    return NULL;
}

static drmd_kms_buffer_t *find_buffer_offset(uint64_t owner, uint64_t offset, uint64_t length)
{
    for (size_t i = 0; i < DRMD_KMS_GEM_HANDLE_MAX; i++) {
        drmd_kms_gem_handle_t *gem = &kms.gem_handles[i];
        if (gem->active && gem->owner == owner && gem->buffer != NULL &&
            gem->buffer->mmap_offset == offset && length <= gem->buffer->size) {
            return gem->buffer;
        }
    }
    return NULL;
}

static drmd_kms_gem_handle_t *alloc_gem_handle(uint64_t owner, drmd_kms_buffer_t *buffer)
{
    if (buffer == NULL || !buffer->active) return NULL;
    for (size_t i = 0; i < DRMD_KMS_GEM_HANDLE_MAX; i++) {
        drmd_kms_gem_handle_t *gem = &kms.gem_handles[i];
        if (!gem->active) {
            memset(gem, 0, sizeof(*gem));
            gem->active = 1;
            gem->owner = owner;
            gem->handle = kms.next_handle++;
            if (kms.next_handle == 0) kms.next_handle = 1;
            gem->buffer = buffer;
            buffer->handle_refs++;
            return gem;
        }
    }
    return NULL;
}

static void destroy_buffer(drmd_kms_buffer_t *buffer)
{
    if (buffer == NULL || !buffer->active) return;
    if (buffer->object != NULL) {
        drmd_kms_owner_context_t context;
        const int entered = enter_module(&context);
        kms.unref_resource(kms.vgdev, buffer->object);
        kms.notify(kms.vgdev);
        if (entered) leave_module(&context);
        pump_device();
    }
    if (buffer->reusable) {
        const int vmo_fd = buffer->vmo_fd;
        void *const mapping = buffer->mapping;
        const uint64_t dma_addr = buffer->dma_addr;
        const uint64_t size = buffer->size;
        if (buffer->object != NULL) kb_kfree(buffer->object);
        reset_buffer(buffer);
        buffer->cached = 1;
        buffer->reusable = 1;
        buffer->vmo_fd = vmo_fd;
        buffer->mapping = mapping;
        buffer->dma_addr = dma_addr;
        buffer->size = size;
        return;
    }
    kb_subsystem_dma_unmap(
        kms.device_backend, NULL, buffer->dma_addr, buffer->size, KB_DMA_TO_DEVICE);
    (void)pacha_munmap(buffer->mapping, buffer->size);
    (void)pacha_fd_close(buffer->vmo_fd);
    if (buffer->object != NULL) kb_kfree(buffer->object);
    reset_buffer(buffer);
}

static void release_cached_storage(drmd_kms_buffer_t *buffer)
{
    if (buffer == NULL || buffer->active || !buffer->cached) return;
    kb_subsystem_dma_unmap(
        kms.device_backend, NULL, buffer->dma_addr, buffer->size, KB_DMA_TO_DEVICE);
    (void)pacha_munmap(buffer->mapping, buffer->size);
    (void)pacha_fd_close(buffer->vmo_fd);
    reset_buffer(buffer);
}

static void maybe_destroy_buffer(drmd_kms_buffer_t *buffer)
{
    if (buffer != NULL && buffer->active && buffer->handle_refs == 0 &&
        buffer->export_refs == 0 && buffer->fb_refs == 0) {
        destroy_buffer(buffer);
    }
}

static void release_gem_handle(drmd_kms_gem_handle_t *gem)
{
    if (gem == NULL || !gem->active) return;
    drmd_kms_buffer_t *buffer = gem->buffer;
    memset(gem, 0, sizeof(*gem));
    if (buffer != NULL && buffer->handle_refs != 0) buffer->handle_refs--;
    maybe_destroy_buffer(buffer);
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

static int attach_scanout_resource(
    drmd_kms_buffer_t *buffer,
    uint32_t width,
    uint32_t height,
    uint32_t pitch)
{
    if (buffer == NULL || !buffer->active || width == 0 || height == 0 ||
        width > DRMD_KMS_MAX_WIDTH || height > DRMD_KMS_MAX_HEIGHT ||
        pitch < width * 4u || (uint64_t)pitch * height > buffer->size) {
        return -22;
    }
    if (buffer->object != NULL) {
        return buffer->width == width && buffer->height == height && buffer->pitch == pitch ? 0 : -22;
    }
    void *object = kb_kzalloc(DRMD_VIRTIO_GPU_OBJECT_BYTES, 0);
    if (object == NULL) return -12;
    *(uint32_t *)object = 1;
    memcpy((uint8_t *)object + DRMD_GEM_DEV_OFFSET, &kms.drm_device, sizeof(kms.drm_device));
    uint32_t resource_id = 0;
    drmd_kms_owner_context_t context;
    const int entered = enter_module(&context);
    int status = kms.resource_id_get(kms.vgdev, &resource_id);
    if (status == 0) {
        drmd_virtio_gpu_object_params_t params;
        memset(&params, 0, sizeof(params));
        params.size = buffer->size;
        params.dumb = 1;
        params.format = 2;
        params.width = width;
        params.height = height;
        memcpy((uint8_t *)object + 0x198, &resource_id, sizeof(resource_id));
        kms.create_resource(kms.vgdev, object, &params, NULL, NULL);
        drmd_virtio_gpu_mem_entry_t *entry = kb_kzalloc(sizeof(*entry), 0);
        if (entry == NULL) {
            status = -12;
        } else {
            entry->addr = buffer->dma_addr;
            entry->length = (uint32_t)buffer->size;
            kms.object_attach(kms.vgdev, object, entry, 1);
            kms.notify(kms.vgdev);
        }
    }
    if (entered) leave_module(&context);
    pump_device();
    if (status != 0) {
        kb_kfree(object);
        return status;
    }
    buffer->resource_id = resource_id;
    buffer->width = width;
    buffer->height = height;
    buffer->pitch = pitch;
    buffer->object = object;
    return 0;
}

static int create_dumb(struct drmd_drm_island *island, uint64_t owner, drmd_mode_create_dumb_t *args)
{
    if (args == NULL || args->flags != 0 || args->bpp != 32 || args->width == 0 || args->height == 0 ||
        args->width > DRMD_KMS_MAX_WIDTH || args->height > DRMD_KMS_MAX_HEIGHT) {
        return -22;
    }
    const uint64_t pitch = (uint64_t)args->width * 4u;
    uint64_t requested_size = pitch * args->height;
    requested_size = (requested_size + 4095u) & ~4095ull;
    if (requested_size == 0 || requested_size > 256u * 1024u * 1024u) {
        return -12;
    }
    drmd_kms_buffer_t *buffer = NULL;
    for (size_t i = 0; i < DRMD_KMS_BUFFER_MAX; i++) {
        drmd_kms_buffer_t *candidate = &kms.buffers[i];
        if (!candidate->active && candidate->cached && candidate->size >= requested_size &&
            (buffer == NULL || candidate->size < buffer->size)) {
            buffer = candidate;
        }
    }
    if (buffer == NULL) {
        for (size_t i = 0; i < DRMD_KMS_BUFFER_MAX; i++) {
            if (!kms.buffers[i].active && !kms.buffers[i].cached) {
                buffer = &kms.buffers[i];
                break;
            }
        }
    }
    if (buffer == NULL) {
        for (size_t i = 0; i < DRMD_KMS_BUFFER_MAX; i++) {
            if (!kms.buffers[i].active && kms.buffers[i].cached) {
                buffer = &kms.buffers[i];
                release_cached_storage(buffer);
                break;
            }
        }
    }
    if (buffer == NULL) return -24;
    const int reused_storage = buffer->cached;
    uint64_t size = reused_storage ? buffer->size : requested_size;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    kb_device_backend_t *backend = (kb_device_backend_t *)island->device_backend;
    int vmo_fd = reused_storage ? buffer->vmo_fd : -1;
    void *mapping = reused_storage ? buffer->mapping : NULL;
    uint64_t mapped_dma = reused_storage ? buffer->dma_addr : 0;
    if (reused_storage) {
        memset(mapping, 0, size);
    } else {
        vmo_fd = pacha_vmo_create(size, rights, 0);
        if (vmo_fd < 16) {
            return -12;
        }
        mapping = pacha_mmap(
            vmo_fd, size, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
        if (mapping == NULL) {
            (void)pacha_fd_close(vmo_fd);
            return -12;
        }
        kb_status_t dma_status = KB_ERR_INVALID;
        mapped_dma = kb_subsystem_dma_map(
            backend, NULL, mapping, size, KB_DMA_TO_DEVICE, &dma_status);
        if (dma_status != KB_OK || mapped_dma == 0) {
            (void)pacha_munmap(mapping, size);
            (void)pacha_fd_close(vmo_fd);
            return -5;
        }
    }
    void *object = kb_kzalloc(DRMD_VIRTIO_GPU_OBJECT_BYTES, 0);
    if (object == NULL) {
        if (!reused_storage) {
            kb_subsystem_dma_unmap(backend, NULL, mapped_dma, size, KB_DMA_TO_DEVICE);
            (void)pacha_munmap(mapping, size);
            (void)pacha_fd_close(vmo_fd);
        }
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
        if (!reused_storage) {
            kb_subsystem_dma_unmap(backend, NULL, mapped_dma, size, KB_DMA_TO_DEVICE);
            (void)pacha_munmap(mapping, size);
            (void)pacha_fd_close(vmo_fd);
        }
        return status;
    }
    reset_buffer(buffer);
    buffer->active = 1;
    buffer->reusable = 1;
    buffer->resource_id = resource_id;
    buffer->width = args->width;
    buffer->height = args->height;
    buffer->pitch = (uint32_t)pitch;
    buffer->size = size;
    buffer->vmo_fd = vmo_fd;
    buffer->mapping = mapping;
    buffer->dma_addr = mapped_dma;
    buffer->object = object;
    drmd_kms_gem_handle_t *gem = alloc_gem_handle(owner, buffer);
    if (gem == NULL) {
        destroy_buffer(buffer);
        return -24;
    }
    printf("[drmd] dumb storage %s bytes=%llu dma=0x%llx\n",
        reused_storage ? "reuse" : "allocate",
        (unsigned long long)size,
        (unsigned long long)mapped_dma);
    buffer->mmap_offset = (uint64_t)gem->handle << 32u;
    args->handle = gem->handle;
    args->pitch = buffer->pitch;
    args->size = buffer->size;
    return 0;
}

static int add_fb(uint64_t owner, uint32_t handle, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format, uint32_t *out_id)
{
    drmd_kms_gem_handle_t *gem = find_gem_handle(owner, handle);
    drmd_kms_buffer_t *buffer = gem != NULL ? gem->buffer : NULL;
    if (buffer == NULL || format != DRMD_FORMAT_XRGB8888 || out_id == NULL) {
        return -22;
    }
    const int attach_status = attach_scanout_resource(buffer, width, height, pitch);
    if (attach_status != 0 || width > buffer->width || height > buffer->height || pitch != buffer->pitch) {
        return attach_status != 0 ? attach_status : -22;
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
            fb->buffer = buffer;
            buffer->fb_refs++;
            *out_id = fb->id;
            size_t active_fbs = 0;
            for (size_t j = 0; j < DRMD_KMS_FB_MAX; j++) {
                if (kms.fb[j].active && kms.fb[j].format == DRMD_FORMAT_XRGB8888) active_fbs++;
            }
            if (active_fbs == 2) {
                printf("[drmd] kms framebuffer pool format=XR24 active=2\n");
            }
            return 0;
        }
    }
    return -24;
}

int drmd_kms_prime_export(
    uint64_t owner,
    uint32_t gem_handle,
    uint32_t flags,
    uint64_t *out_token,
    int *out_vmo_fd,
    uint64_t *out_rights)
{
    if ((flags & ~(uint32_t)(DRMD_CLOEXEC | DRMD_RDWR)) != 0 ||
        out_token == NULL || out_vmo_fd == NULL || out_rights == NULL) {
        return -22;
    }
    drmd_kms_gem_handle_t *gem = find_gem_handle(owner, gem_handle);
    drmd_kms_buffer_t *buffer = gem != NULL ? gem->buffer : NULL;
    if (buffer == NULL || buffer->export_refs == UINT32_MAX) return -2;
    if (buffer->token == 0) {
        buffer->token = kms.next_token++;
        if (kms.next_token == 0) kms.next_token = 1;
        buffer->export_owner = owner;
    }
    buffer->export_refs++;
    *out_token = buffer->token;
    *out_vmo_fd = buffer->vmo_fd;
    *out_rights = PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    return 0;
}

int drmd_kms_prime_import(uint64_t owner, uint64_t token, uint32_t flags, uint32_t *out_gem_handle)
{
    if (token == 0 || flags != 0 || out_gem_handle == NULL) return -22;
    drmd_kms_buffer_t *buffer = find_buffer_token(token);
    if (buffer == NULL || buffer->export_refs == 0) return -9;
    for (size_t i = 0; i < DRMD_KMS_GEM_HANDLE_MAX; i++) {
        drmd_kms_gem_handle_t *gem = &kms.gem_handles[i];
        if (gem->active && gem->owner == owner && gem->buffer == buffer) {
            *out_gem_handle = gem->handle;
            return 0;
        }
    }
    drmd_kms_gem_handle_t *gem = alloc_gem_handle(owner, buffer);
    if (gem == NULL) return -24;
    *out_gem_handle = gem->handle;
    return 0;
}

int drmd_kms_prime_import_vmo(
    uint64_t owner,
    int vmo_fd,
    uint64_t size,
    uint32_t flags,
    uint32_t *out_gem_handle)
{
    if (vmo_fd < 16 || size == 0 || size > 256u * 1024u * 1024u ||
        (size & 4095u) != 0 || flags != 0 || out_gem_handle == NULL) {
        return -22;
    }
    drmd_kms_buffer_t *buffer = NULL;
    for (size_t i = 0; i < DRMD_KMS_BUFFER_MAX; i++) {
        if (!kms.buffers[i].active && !kms.buffers[i].cached) {
            buffer = &kms.buffers[i];
            break;
        }
    }
    if (buffer == NULL) {
        for (size_t i = 0; i < DRMD_KMS_BUFFER_MAX; i++) {
            if (!kms.buffers[i].active && kms.buffers[i].cached) {
                buffer = &kms.buffers[i];
                release_cached_storage(buffer);
                break;
            }
        }
    }
    if (buffer == NULL) return -24;
    void *mapping = pacha_mmap(
        vmo_fd, size, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (mapping == NULL) return -12;
    kb_status_t dma_status = KB_ERR_INVALID;
    const uint64_t mapped_dma = kb_subsystem_dma_map(
        kms.device_backend, NULL, mapping, size, KB_DMA_TO_DEVICE, &dma_status);
    if (dma_status != KB_OK || mapped_dma == 0) {
        (void)pacha_munmap(mapping, size);
        return -5;
    }
    reset_buffer(buffer);
    buffer->active = 1;
    buffer->size = size;
    buffer->vmo_fd = vmo_fd;
    buffer->mapping = mapping;
    buffer->dma_addr = mapped_dma;
    drmd_kms_gem_handle_t *gem = alloc_gem_handle(owner, buffer);
    if (gem == NULL) {
        kb_subsystem_dma_unmap(
            kms.device_backend, NULL, mapped_dma, size, KB_DMA_TO_DEVICE);
        (void)pacha_munmap(mapping, size);
        reset_buffer(buffer);
        return -24;
    }
    buffer->mmap_offset = (uint64_t)gem->handle << 32u;
    *out_gem_handle = gem->handle;
    return 0;
}

int drmd_kms_prime_import_sync_file(uint64_t token, int wait_fd)
{
    drmd_kms_buffer_t *buffer = token != 0 ? find_buffer_token(token) : NULL;
    if (buffer == NULL || buffer->export_refs == 0 || wait_fd < 16) return -9;
    if (buffer->acquire_sync_fd >= 16) {
        const int close_status = pacha_fd_close(buffer->acquire_sync_fd);
        if (close_status != 0) return close_status;
    }
    buffer->acquire_sync_fd = wait_fd;
    return 0;
}

int drmd_kms_prime_acquire(uint64_t token)
{
    drmd_kms_buffer_t *buffer = token != 0 ? find_buffer_token(token) : NULL;
    if (buffer == NULL || buffer->export_refs == 0) return -9;
    if (buffer->export_refs == UINT32_MAX) return -75;
    buffer->export_refs++;
    return 0;
}

int drmd_kms_prime_release(uint64_t token)
{
    drmd_kms_buffer_t *buffer = token != 0 ? find_buffer_token(token) : NULL;
    if (buffer == NULL || buffer->export_refs == 0) return -9;
    buffer->export_refs--;
    maybe_destroy_buffer(buffer);
    return 0;
}

void drmd_kms_handle_orphan(uint64_t handle)
{
    for (size_t i = 0; i < DRMD_KMS_BUFFER_MAX; i++) {
        drmd_kms_buffer_t *buffer = &kms.buffers[i];
        if (!buffer->active || buffer->export_owner != handle || buffer->export_refs == 0) {
            continue;
        }
        const uint64_t token = buffer->token;
        const uint32_t refs = buffer->export_refs;
        buffer->export_refs = 0;
        printf("[drmd] orphan token reap owner=%llu token=%llu refs=%u\n",
            (unsigned long long)handle,
            (unsigned long long)token,
            refs);
        maybe_destroy_buffer(buffer);
    }
}

static int consume_acquire_sync_file(drmd_kms_buffer_t *buffer)
{
    if (buffer == NULL || buffer->acquire_sync_fd < 16) return 0;
    const int wait_fd = buffer->acquire_sync_fd;
    buffer->acquire_sync_fd = -1;
    struct pacha_pollfd pollfd = {
        .fd = wait_fd,
        .events = PACHA_FD_EVENT_READABLE |
            PACHA_FD_EVENT_ERROR |
            PACHA_FD_EVENT_HANGUP,
    };
    const long wait_status =
        pacha_fd_wait_many(&pollfd, 1, PACHA_FD_WAIT_FOREVER);
    const int close_status = pacha_fd_close(wait_fd);
    if (wait_status < 0) return (int)wait_status;
    if ((pollfd.revents & PACHA_FD_EVENT_READABLE) == 0) return -5;
    return close_status;
}

static int submit_scanout_fb(drmd_kms_fb_t *fb)
{
    drmd_kms_buffer_t *buffer = fb != NULL ? fb->buffer : NULL;
    if (buffer == NULL) {
        return -2;
    }
    const int acquire_status = consume_acquire_sync_file(buffer);
    if (acquire_status != 0) return acquire_status;
    drmd_kms_owner_context_t context;
    const int entered = enter_module(&context);
    void *objects = kms.array_alloc(1);
    if (objects == NULL) {
        if (entered) leave_module(&context);
        return -12;
    }
    kms.array_add(objects, buffer->object);
    kms.transfer_2d(kms.vgdev, 0, fb->width, fb->height, 0, 0, objects, NULL);
    kms.set_scanout(kms.vgdev, 0, buffer->resource_id, fb->width, fb->height, 0, 0);
    kms.flush(kms.vgdev, buffer->resource_id, 0, 0, fb->width, fb->height, NULL, NULL);
    kms.notify(kms.vgdev);
    if (entered) leave_module(&context);
    return 0;
}

static int scanout_fb(drmd_kms_fb_t *fb)
{
    const int status = submit_scanout_fb(fb);
    if (status != 0) {
        return status;
    }
    pump_device();
    kms.current_fb = fb->id;
    return 0;
}

static int disable_scanout(void)
{
    drmd_kms_owner_context_t context;
    const int entered = enter_module(&context);
    kms.set_scanout(kms.vgdev, 0, 0, 0, 0, 0, 0);
    kms.notify(kms.vgdev);
    if (entered) leave_module(&context);
    pump_device();
    kms.current_fb = 0;
    return 0;
}

static int require_master(uint64_t handle)
{
    return kms.master_handle == handle ? 0 : -13;
}

static drmd_kms_event_file_t *find_event_file_magic(uint32_t magic)
{
    for (size_t i = 0; i < DRMD_KMS_EVENT_FILE_MAX; i++) {
        if (kms.event_files[i].active && kms.event_files[i].magic == magic) {
            return &kms.event_files[i];
        }
    }
    return NULL;
}

static void queue_flip_event(
    drmd_kms_event_file_t *event_file,
    uint64_t user_data,
    uint32_t fb_id)
{
    const uint32_t tail =
        (event_file->head + event_file->count) % DRMD_KMS_EVENT_QUEUE_MAX;
    drmd_event_vblank_t *event = &event_file->events[tail];
    memset(event, 0, sizeof(*event));
    struct timespec now;
    memset(&now, 0, sizeof(now));
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    event->type = DRMD_EVENT_FLIP_COMPLETE;
    event->length = sizeof(*event);
    event->user_data = user_data;
    event->tv_sec = now.tv_sec < 0 ? 0u : (uint32_t)now.tv_sec;
    event->tv_usec = now.tv_nsec < 0 ? 0u : (uint32_t)(now.tv_nsec / 1000);
    event->sequence = ++kms.sequence;
    event->crtc_id = DRMD_KMS_CRTC_ID;
    event_file->count++;
    kms.current_fb = fb_id;
}

static void note_flip_event_delivered(const drmd_event_vblank_t *event)
{
    if (event == NULL || event->type != DRMD_EVENT_FLIP_COMPLETE) return;
    kms.delivered_flip_events++;
    if (kms.delivered_flip_events == 2) {
        printf("[drmd] legacy page-flip events delivered count=2 crtc=%u\n",
            event->crtc_id);
    }
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
    const uint32_t prop_capacity = wire->value.count_props;
    if (mode_capacity != 0) wire->modes[0] = kms.current_mode;
    if (encoder_capacity != 0) wire->encoders[0] = DRMD_KMS_ENCODER_ID;
    if (prop_capacity != 0) {
        wire->props[0] = DRMD_KMS_DPMS_PROP_ID;
        wire->prop_values[0] = kms.dpms;
    }
    wire->value.count_modes = 1;
    wire->value.count_props = 1;
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

static int ioctl_object_properties(drmd_kms_object_properties_wire_t *wire)
{
    if (wire == NULL) return -22;
    const uint32_t capacity = wire->value.count_props;
    uint32_t count = 0;
    if (wire->value.obj_id == DRMD_KMS_CONNECTOR_ID &&
        (wire->value.obj_type == DRMD_MODE_OBJECT_ANY ||
         wire->value.obj_type == DRMD_MODE_OBJECT_CONNECTOR)) {
        if (capacity != 0) {
            wire->props[0] = DRMD_KMS_DPMS_PROP_ID;
            wire->prop_values[0] = kms.dpms;
        }
        count = 1;
    } else if (wire->value.obj_id == DRMD_KMS_CRTC_ID &&
        (wire->value.obj_type == DRMD_MODE_OBJECT_ANY ||
         wire->value.obj_type == DRMD_MODE_OBJECT_CRTC)) {
        count = 0;
    } else if (wire->value.obj_id == DRMD_KMS_PLANE_ID &&
        (wire->value.obj_type == DRMD_MODE_OBJECT_ANY ||
         wire->value.obj_type == DRMD_MODE_OBJECT_PLANE)) {
        if (capacity != 0) {
            wire->props[0] = DRMD_KMS_PLANE_TYPE_PROP_ID;
            wire->prop_values[0] = DRMD_MODE_PLANE_TYPE_PRIMARY;
        }
        if (capacity > 1) {
            wire->props[1] = DRMD_KMS_IN_FORMATS_PROP_ID;
            wire->prop_values[1] = DRMD_KMS_IN_FORMATS_BLOB_ID;
        }
        count = 2;
    } else {
        return -2;
    }
    wire->value.count_props = count;
    return 0;
}

static void set_property_enum(
    drmd_mode_property_enum_t *entry,
    uint64_t value,
    const char *name)
{
    entry->value = value;
    strncpy(entry->name, name, sizeof(entry->name) - 1);
}

static int ioctl_property(drmd_kms_property_wire_t *wire)
{
    if (wire == NULL) return -22;
    const uint32_t enum_capacity = wire->value.count_enum_blobs;
    memset(wire->value.name, 0, sizeof(wire->value.name));
    wire->value.count_values = 0;
    switch (wire->value.prop_id) {
    case DRMD_KMS_DPMS_PROP_ID:
        wire->value.flags = DRMD_MODE_PROP_ENUM;
        memcpy(wire->value.name, "DPMS", 5);
        if (enum_capacity > 0) set_property_enum(&wire->enums[0], DRMD_MODE_DPMS_ON, "On");
        if (enum_capacity > 1) set_property_enum(&wire->enums[1], DRMD_MODE_DPMS_STANDBY, "Standby");
        if (enum_capacity > 2) set_property_enum(&wire->enums[2], DRMD_MODE_DPMS_SUSPEND, "Suspend");
        if (enum_capacity > 3) set_property_enum(&wire->enums[3], DRMD_MODE_DPMS_OFF, "Off");
        wire->value.count_enum_blobs = 4;
        return 0;
    case DRMD_KMS_PLANE_TYPE_PROP_ID:
        wire->value.flags = DRMD_MODE_PROP_ENUM | DRMD_MODE_PROP_IMMUTABLE;
        memcpy(wire->value.name, "type", 5);
        if (enum_capacity > 0) set_property_enum(&wire->enums[0], DRMD_MODE_PLANE_TYPE_OVERLAY, "Overlay");
        if (enum_capacity > 1) set_property_enum(&wire->enums[1], DRMD_MODE_PLANE_TYPE_PRIMARY, "Primary");
        if (enum_capacity > 2) set_property_enum(&wire->enums[2], DRMD_MODE_PLANE_TYPE_CURSOR, "Cursor");
        wire->value.count_enum_blobs = 3;
        return 0;
    case DRMD_KMS_IN_FORMATS_PROP_ID:
        wire->value.flags = DRMD_MODE_PROP_BLOB | DRMD_MODE_PROP_IMMUTABLE;
        memcpy(wire->value.name, "IN_FORMATS", 11);
        wire->value.count_enum_blobs = 0;
        return 0;
    default:
        return -2;
    }
}

static int ioctl_property_blob(drmd_kms_property_blob_wire_t *wire)
{
    if (wire == NULL || wire->value.blob_id != DRMD_KMS_IN_FORMATS_BLOB_ID) return -2;
    drmd_format_modifier_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = 1;
    blob.count_formats = 1;
    blob.formats_offset = offsetof(drmd_format_modifier_blob_t, format);
    blob.count_modifiers = 1;
    blob.modifiers_offset = offsetof(drmd_format_modifier_blob_t, modifier);
    blob.format = DRMD_FORMAT_XRGB8888;
    blob.modifier.formats = 1;
    blob.modifier.modifier = DRMD_FORMAT_MOD_LINEAR;
    if (wire->value.length != 0) {
        uint32_t length = wire->value.length < sizeof(blob) ? wire->value.length : sizeof(blob);
        memcpy(wire->data, &blob, length);
    }
    wire->value.length = sizeof(blob);
    return 0;
}

int drmd_kms_ioctl(struct drmd_drm_island *island, drmd_ioctl_request_t *request, int *out_handled)
{
    if (out_handled == NULL || request == NULL || !kms.ready) return -22;
    *out_handled = 1;
    switch ((uint32_t)request->request) {
    case DRMD_IOCTL_GET_MAGIC: {
        if (request->data_size < sizeof(uint32_t)) return -22;
        drmd_kms_event_file_t *event_file = find_event_file(request->handle);
        if (event_file == NULL) return -9;
        if (event_file->magic == 0) {
            event_file->magic = kms.next_magic++;
            if (kms.next_magic == 0) kms.next_magic = 1;
        }
        *(uint32_t *)request->data = event_file->magic;
        return 0;
    }
    case DRMD_IOCTL_AUTH_MAGIC: {
        if (require_master(request->handle) != 0) return -13;
        if (request->data_size < sizeof(uint32_t)) return -22;
        const uint32_t magic = *(const uint32_t *)request->data;
        if (magic == 0) return -22;
        drmd_kms_event_file_t *event_file = find_event_file_magic(magic);
        if (event_file == NULL) return -22;
        event_file->authenticated = 1;
        event_file->magic = 0;
        return 0;
    }
    case DRMD_IOCTL_GET_CAP: {
        if (request->data_size < sizeof(drmd_get_cap_t)) return -22;
        drmd_get_cap_t *cap = (void *)request->data;
        switch (cap->capability) {
        case DRMD_CAP_DUMB_BUFFER:
            cap->value = 1;
            return 0;
        case DRMD_CAP_PRIME:
            cap->value = DRMD_PRIME_CAP_IMPORT | DRMD_PRIME_CAP_EXPORT;
            return 0;
        case DRMD_CAP_TIMESTAMP_MONOTONIC:
        case DRMD_CAP_CRTC_IN_VBLANK_EVENT:
            cap->value = 1;
            return 0;
        case DRMD_CAP_ADDFB2_MODIFIERS:
            cap->value = 1;
            return 0;
        default:
            return -22;
        }
    }
    case DRMD_IOCTL_SET_CLIENT_CAP: {
        if (request->data_size < sizeof(drmd_set_client_cap_t)) return -22;
        const drmd_set_client_cap_t *cap = (const void *)request->data;
        if (cap->value > 1) return -22;
        if (cap->capability == DRMD_CLIENT_CAP_ATOMIC) {
            return cap->value == 0 ? 0 : -95;
        }
        if (cap->capability != DRMD_CLIENT_CAP_UNIVERSAL_PLANES) return -95;
        drmd_kms_event_file_t *event_file = find_event_file(request->handle);
        if (event_file == NULL) return -9;
        event_file->universal_planes = cap->value != 0;
        return 0;
    }
    case DRMD_IOCTL_SET_MASTER:
        if (kms.master_handle != 0 && kms.master_handle != request->handle) return -16;
        kms.master_handle = request->handle;
        {
            drmd_kms_event_file_t *event_file = find_event_file(request->handle);
            if (event_file != NULL) event_file->authenticated = 1;
        }
        return 0;
    case DRMD_IOCTL_DROP_MASTER:
        if (kms.master_handle != request->handle) return -22;
        kms.master_handle = 0;
        return 0;
    case DRMD_IOCTL_GEM_CLOSE: {
        if (request->data_size < sizeof(drmd_gem_close_t)) return -22;
        const drmd_gem_close_t *close = (const void *)request->data;
        if (close->pad != 0) return -22;
        drmd_kms_gem_handle_t *gem = find_gem_handle(request->handle, close->handle);
        if (gem == NULL) return -2;
        release_gem_handle(gem);
        return 0;
    }
    case DRMD_IOCTL_MODE_GETRESOURCES:
        return request->data_size >= sizeof(drmd_kms_resources_wire_t) ? ioctl_resources((void *)request->data) : -22;
    case DRMD_IOCTL_MODE_GETCONNECTOR:
        return request->data_size >= sizeof(drmd_kms_connector_wire_t) ? ioctl_connector((void *)request->data) : -22;
    case DRMD_IOCTL_MODE_GETPROPERTY:
        return request->data_size >= sizeof(drmd_kms_property_wire_t) ?
            ioctl_property((void *)request->data) : -22;
    case DRMD_IOCTL_MODE_GETPROPBLOB:
        return request->data_size >= sizeof(drmd_kms_property_blob_wire_t) ?
            ioctl_property_blob((void *)request->data) : -22;
    case DRMD_IOCTL_MODE_OBJ_GETPROPERTIES:
        return request->data_size >= sizeof(drmd_kms_object_properties_wire_t) ?
            ioctl_object_properties((void *)request->data) : -22;
    case DRMD_IOCTL_MODE_CREATE_LEASE:
        return -95;
    case DRMD_IOCTL_MODE_SETPROPERTY: {
        if (require_master(request->handle) != 0 ||
            request->data_size < sizeof(drmd_mode_connector_set_property_t)) return -13;
        const drmd_mode_connector_set_property_t *property = (const void *)request->data;
        if (property->connector_id != DRMD_KMS_CONNECTOR_ID ||
            property->prop_id != DRMD_KMS_DPMS_PROP_ID ||
            property->value > DRMD_MODE_DPMS_OFF) return -22;
        kms.dpms = property->value;
        return kms.dpms == DRMD_MODE_DPMS_ON ? 0 : disable_scanout();
    }
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
        if (wire->value.crtc_id != DRMD_KMS_CRTC_ID) return -22;
        if (wire->value.count_connectors == 0 && wire->value.fb_id == 0 &&
            !wire->value.mode_valid) return disable_scanout();
        if (wire->value.count_connectors != 1 ||
            wire->connectors[0] != DRMD_KMS_CONNECTOR_ID || !wire->value.mode_valid ||
            wire->value.mode.hdisplay == 0 || wire->value.mode.vdisplay == 0) return -22;
        drmd_kms_fb_t *fb = find_fb(request->handle, wire->value.fb_id);
        if (fb == NULL || fb->width != wire->value.mode.hdisplay || fb->height != wire->value.mode.vdisplay) return -22;
        kms.current_mode = wire->value.mode;
        return scanout_fb(fb);
    }
    case DRMD_IOCTL_MODE_CURSOR: {
        if (require_master(request->handle) != 0 ||
            request->data_size < sizeof(drmd_mode_cursor_t)) return -13;
        const drmd_mode_cursor_t *cursor = (const void *)request->data;
        if (cursor->crtc_id != DRMD_KMS_CRTC_ID) return -2;
        if (cursor->flags == DRMD_MODE_CURSOR_BO && cursor->handle == 0 &&
            cursor->width == 0 && cursor->height == 0) return 0;
        return -95;
    }
    case DRMD_IOCTL_MODE_CREATE_DUMB:
        return request->data_size >= sizeof(drmd_mode_create_dumb_t) ? create_dumb(island, request->handle, (void *)request->data) : -22;
    case DRMD_IOCTL_MODE_MAP_DUMB: {
        if (request->data_size < sizeof(drmd_mode_map_dumb_t)) return -22;
        drmd_mode_map_dumb_t *map = (void *)request->data;
        drmd_kms_gem_handle_t *gem = find_gem_handle(request->handle, map->handle);
        if (gem == NULL || gem->buffer == NULL) return -2;
        map->offset = gem->buffer->mmap_offset;
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
        if ((fb->flags != 0 && fb->flags != DRMD_MODE_FB_MODIFIERS) ||
            fb->offsets[0] != 0 || fb->handles[1] != 0 ||
            (fb->flags == DRMD_MODE_FB_MODIFIERS && fb->modifier[0] != DRMD_FORMAT_MOD_LINEAR)) return -22;
        return add_fb(request->handle, fb->handles[0], fb->width, fb->height, fb->pitches[0], fb->pixel_format, &fb->fb_id);
    }
    case DRMD_IOCTL_MODE_RMFB: {
        if (request->data_size < sizeof(uint32_t)) return -22;
        const uint32_t id = *(uint32_t *)request->data;
        drmd_kms_fb_t *fb = find_fb(request->handle, id);
        if (fb == NULL) return -2;
        if (kms.current_fb == id) return -16;
        drmd_kms_buffer_t *buffer = fb->buffer;
        if (buffer != NULL && buffer->fb_refs != 0) buffer->fb_refs--;
        memset(fb, 0, sizeof(*fb));
        maybe_destroy_buffer(buffer);
        return 0;
    }
    case DRMD_IOCTL_MODE_CLOSEFB:
        return -22;
    case DRMD_IOCTL_MODE_PAGE_FLIP: {
        if (require_master(request->handle) != 0 || request->data_size < sizeof(drmd_mode_crtc_page_flip_t)) return -13;
        drmd_mode_crtc_page_flip_t *flip = (void *)request->data;
        if (flip->crtc_id != DRMD_KMS_CRTC_ID || flip->reserved != 0 ||
            (flip->flags != 0 && flip->flags != DRMD_MODE_PAGE_FLIP_EVENT)) return -22;
        drmd_kms_fb_t *fb = find_fb(request->handle, flip->fb_id);
        if (fb == NULL) return -2;
        if (flip->flags == 0) return scanout_fb(fb);
        drmd_kms_event_file_t *event_file = find_event_file(request->handle);
        if (event_file == NULL) return -9;
        if (event_file->count >= DRMD_KMS_EVENT_QUEUE_MAX) return -28;
        const int status = submit_scanout_fb(fb);
        if (status != 0) return status;
        queue_flip_event(event_file, flip->user_data, fb->id);
        return 0;
    }
    case DRMD_IOCTL_MODE_GETPLANERESOURCES: {
        if (request->data_size < sizeof(drmd_kms_plane_res_wire_t)) return -22;
        drmd_kms_plane_res_wire_t *wire = (void *)request->data;
        drmd_kms_event_file_t *event_file = find_event_file(request->handle);
        if (event_file == NULL) return -9;
        if (event_file->universal_planes && wire->value.count_planes != 0) {
            wire->planes[0] = DRMD_KMS_PLANE_ID;
        }
        wire->value.count_planes = event_file->universal_planes ? 1 : 0;
        return 0;
    }
    case DRMD_IOCTL_MODE_GETPLANE: {
        if (request->data_size < sizeof(drmd_kms_plane_wire_t)) return -22;
        drmd_kms_plane_wire_t *wire = (void *)request->data;
        drmd_kms_event_file_t *event_file = find_event_file(request->handle);
        if (event_file == NULL || !event_file->universal_planes) return -2;
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
        drmd_kms_gem_handle_t *gem = find_gem_handle(request->handle, destroy->handle);
        if (gem == NULL || gem->buffer == NULL) return -2;
        if (gem->buffer->fb_refs != 0) return -16;
        release_gem_handle(gem);
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
    drmd_kms_buffer_t *buffer = find_buffer_offset(request->handle, request->offset, request->length);
    if (buffer == NULL) return -6;
    *out_vmo_fd = buffer->vmo_fd;
    return 0;
}

void drmd_kms_handle_open(uint64_t handle)
{
    for (size_t i = 0; i < DRMD_KMS_EVENT_FILE_MAX; i++) {
        if (!kms.event_files[i].active) {
            memset(&kms.event_files[i], 0, sizeof(kms.event_files[i]));
            kms.event_files[i].active = 1;
            kms.event_files[i].owner = handle;
            break;
        }
    }
    if (kms.master_handle == 0) kms.master_handle = handle;
}

int drmd_kms_read(uint64_t handle, void *data, uint64_t capacity, uint64_t *out_size)
{
    if (data == NULL || out_size == NULL) return -22;
    *out_size = 0;
    drmd_kms_event_file_t *event_file = find_event_file(handle);
    if (event_file == NULL) return -9;
    if (event_file->count == 0) return -11;
    if (capacity < sizeof(drmd_event_vblank_t)) return -22;
    uint8_t *output = data;
    while (event_file->count != 0 && *out_size + sizeof(drmd_event_vblank_t) <= capacity) {
        note_flip_event_delivered(&event_file->events[event_file->head]);
        memcpy(output + *out_size, &event_file->events[event_file->head], sizeof(drmd_event_vblank_t));
        memset(&event_file->events[event_file->head], 0, sizeof(drmd_event_vblank_t));
        event_file->head = (event_file->head + 1u) % DRMD_KMS_EVENT_QUEUE_MAX;
        event_file->count--;
        *out_size += sizeof(drmd_event_vblank_t);
    }
    return 0;
}

int drmd_kms_peek_event(uint64_t handle, void *data, uint64_t capacity, uint64_t *out_size)
{
    if (data == NULL || out_size == NULL || capacity < sizeof(drmd_event_vblank_t)) return -22;
    *out_size = 0;
    drmd_kms_event_file_t *event_file = find_event_file(handle);
    if (event_file == NULL) return -9;
    if (event_file->count == 0) return -11;
    memcpy(data, &event_file->events[event_file->head], sizeof(drmd_event_vblank_t));
    *out_size = sizeof(drmd_event_vblank_t);
    return 0;
}

int drmd_kms_consume_event(uint64_t handle)
{
    drmd_kms_event_file_t *event_file = find_event_file(handle);
    if (event_file == NULL) return -9;
    if (event_file->count == 0) return -11;
    note_flip_event_delivered(&event_file->events[event_file->head]);
    memset(&event_file->events[event_file->head], 0, sizeof(drmd_event_vblank_t));
    event_file->head = (event_file->head + 1u) % DRMD_KMS_EVENT_QUEUE_MAX;
    event_file->count--;
    return 0;
}

int drmd_kms_poll(uint64_t handle, uint32_t events, uint32_t *out_revents)
{
    if (out_revents == NULL) return -22;
    drmd_kms_event_file_t *event_file = find_event_file(handle);
    if (event_file == NULL) return -9;
    *out_revents = event_file->count != 0 ? events & DRMD_POLLIN : 0;
    return 0;
}

void drmd_kms_handle_close(struct drmd_drm_island *island, uint64_t handle)
{
    drmd_kms_event_file_t *event_file = find_event_file(handle);
    if (event_file != NULL) memset(event_file, 0, sizeof(*event_file));
    if (kms.master_handle == handle) kms.master_handle = 0;
    int disable_scanout = 0;
    for (size_t i = 0; i < DRMD_KMS_FB_MAX; i++) {
        drmd_kms_fb_t *fb = &kms.fb[i];
        if (!fb->active || fb->owner != handle) continue;
        if (kms.current_fb == fb->id) {
            kms.current_fb = 0;
            disable_scanout = 1;
        }
        drmd_kms_buffer_t *buffer = fb->buffer;
        if (buffer != NULL && buffer->fb_refs != 0) buffer->fb_refs--;
        memset(fb, 0, sizeof(*fb));
        maybe_destroy_buffer(buffer);
    }
    if (disable_scanout) {
        drmd_kms_owner_context_t context;
        const int entered = enter_module(&context);
        kms.set_scanout(kms.vgdev, 0, 0, 0, 0, 0, 0);
        kms.notify(kms.vgdev);
        if (entered) leave_module(&context);
        pump_device();
    }
    for (size_t i = 0; i < DRMD_KMS_GEM_HANDLE_MAX; i++) {
        if (kms.gem_handles[i].active && kms.gem_handles[i].owner == handle) {
            release_gem_handle(&kms.gem_handles[i]);
        }
    }
}

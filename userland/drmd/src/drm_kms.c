#include "drm_kms.h"
#include "drm_atomic_state.h"
#include "drm_flip_completion.h"
#include "drm_property_metadata.h"
#include "drm_syncobj_state.h"
#include "virtio_gpu_unref_bridge.h"

#include <kobox/module.h>
#include <kobox/shim.h>
#include "linux_subsystem/dma/dma.h"
#include "loader/module_context.h"

#include <pacha/abi.h>
#include <pacha/ipc.h>

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    DRMD_KMS_FB_MAX = 16,
    DRMD_KMS_EVENT_FILE_MAX = 32,
    DRMD_KMS_EVENT_QUEUE_MAX = 16,
    DRMD_KMS_WIDTH = 1024,
    DRMD_KMS_HEIGHT = 768,
    DRMD_KMS_MAX_WIDTH = 8192,
    DRMD_KMS_MAX_HEIGHT = 8192,
    DRMD_KMS_BLOB_MAX = 32,
    DRMD_VIRTIO_GPU_OBJECT_BYTES = 0x2c0,
    DRMD_GEM_DEV_OFFSET = 0x08,
    DRMD_DRM_FILE_DRIVER_PRIV_OFFSET = 0x98,
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
typedef void *(*drmd_fence_alloc_fn)(void *, uint64_t, uint32_t);
typedef void (*drmd_notify_fn)(void *);
typedef void *(*drmd_array_alloc_fn)(uint32_t);
typedef void (*drmd_array_add_fn)(void *, void *);
typedef void (*drmd_array_put_free_fn)(void *);
typedef void (*drmd_create_context_fn)(void *, void *);
typedef void (*drmd_create_resource_3d_fn)(void *, void *, void *, void *, void *);
typedef void (*drmd_context_resource_fn)(void *, uint32_t, void *);
typedef void (*drmd_submit_3d_fn)(void *, void *, uint32_t, uint32_t, void *, void *);
typedef void (*drmd_transfer_3d_fn)(
    void *, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t, void *, void *, void *);

typedef struct drmd_kms_buffer {
    struct drmd_kms_buffer *next;
    int active;
    int cached;
    int reusable;
    int virgl;
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
    int destroy_requested;
    int destroy_poisoned;
    drmd_virtio_gpu_unref_request_t unref_request;
} drmd_kms_buffer_t;

typedef struct drmd_kms_gem_handle {
    struct drmd_kms_gem_handle *next;
    int active;
    int context_attached;
    uint64_t owner;
    uint32_t handle;
    uint32_t context_id;
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
    int atomic;
    int authenticated;
    uint64_t owner;
    uint32_t magic;
    uint32_t head;
    uint32_t count;
    drmd_event_vblank_t events[DRMD_KMS_EVENT_QUEUE_MAX];
} drmd_kms_event_file_t;

typedef struct drmd_kms_mode_blob {
    int active;
    int user_ref;
    uint64_t owner;
    uint32_t id;
    uint32_t length;
    uint32_t state_refs;
    uint8_t data[DRMD_KMS_PROPERTY_BLOB_BYTES];
} drmd_kms_mode_blob_t;

enum {
    DRMD_ATOMIC_PENDING_NONE = 0,
    DRMD_ATOMIC_PENDING_WAIT_ACQUIRE = 1,
    DRMD_ATOMIC_PENDING_SUBMITTED = 2,
};

typedef struct drmd_kms_atomic_pending {
    int active;
    int phase;
    int input_wait_fd;
    int blocking;
    uint64_t owner;
    uint64_t user_data;
    uint32_t flags;
    uint32_t mode_blob_id;
    drmd_atomic_snapshot_t snapshot;
} drmd_kms_atomic_pending_t;

typedef struct drmd_kms_state {
    int ready;
    uint64_t master_handle;
    uint32_t next_handle;
    uint32_t next_fb;
    uint64_t next_token;
    uint32_t next_magic;
    uint32_t next_blob;
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
    drmd_fence_alloc_fn fence_alloc;
    drmd_notify_fn notify;
    drmd_array_alloc_fn array_alloc;
    drmd_array_add_fn array_add;
    drmd_array_put_free_fn array_put_free;
    drmd_create_context_fn create_context;
    drmd_create_resource_3d_fn create_resource_3d;
    drmd_context_resource_fn context_attach_resource;
    drmd_context_resource_fn context_detach_resource;
    drmd_submit_3d_fn submit_3d;
    drmd_transfer_3d_fn transfer_from_host_3d;
    drmd_transfer_3d_fn transfer_to_host_3d;
    drmd_kms_buffer_t *buffers;
    drmd_kms_gem_handle_t *gem_handles;
    drmd_kms_fb_t fb[DRMD_KMS_FB_MAX];
    drmd_kms_event_file_t event_files[DRMD_KMS_EVENT_FILE_MAX];
    drmd_kms_mode_blob_t mode_blobs[DRMD_KMS_BLOB_MAX];
    drmd_atomic_lifecycle_t atomic;
    drmd_kms_atomic_pending_t atomic_pending;
    int deferred_result_ready;
    int deferred_result_status;
    drmd_flip_completion_t pending_flip;
    drmd_syncobj_state_t syncobjs;
    drmd_virtio_gpu_unref_bridge_t unref_bridge;
} drmd_kms_state_t;

typedef struct drmd_kms_owner_context {
    unsigned long old_gs;
    kb_module_t *previous_owner;
    int active;
} drmd_kms_owner_context_t;

static drmd_kms_state_t kms;

static int attach_gem_context(void *drm_file, drmd_kms_gem_handle_t *gem);
static int detach_gem_context(drmd_kms_gem_handle_t *gem);

static uint64_t monotonic_ns(void)
{
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static int syncobj_poll_fd(void *context, int fd)
{
    (void)context;
    struct pacha_pollfd pollfd = {
        .fd = fd,
        .events = PACHA_FD_EVENT_READABLE |
            PACHA_FD_EVENT_ERROR | PACHA_FD_EVENT_HANGUP,
    };
    const long status = pacha_fd_poll(&pollfd, 1);
    if (status < 0) return (int)status;
    if ((pollfd.revents & PACHA_FD_EVENT_READABLE) != 0) return 1;
    if ((pollfd.revents & (PACHA_FD_EVENT_ERROR | PACHA_FD_EVENT_HANGUP)) != 0) {
        return -5;
    }
    return 0;
}

static int syncobj_signal_fd(void *context, int fd)
{
    (void)context;
    const struct pacha_ipc_msg message = {0};
    return pacha_ipc_send(fd, &message);
}

static void syncobj_close_fd(void *context, int fd)
{
    (void)context;
    if (fd >= 16) (void)pacha_fd_close(fd);
}

static void reset_buffer(drmd_kms_buffer_t *buffer)
{
    if (buffer == NULL) return;
    drmd_kms_buffer_t *const next = buffer->next;
    if (buffer->acquire_sync_fd >= 16) {
        (void)pacha_fd_close(buffer->acquire_sync_fd);
    }
    memset(buffer, 0, sizeof(*buffer));
    buffer->next = next;
    buffer->acquire_sync_fd = -1;
}

static drmd_kms_buffer_t *alloc_buffer_record(void)
{
    drmd_kms_buffer_t *buffer = calloc(1, sizeof(*buffer));
    if (buffer == NULL) return NULL;
    buffer->acquire_sync_fd = -1;
    buffer->next = kms.buffers;
    kms.buffers = buffer;
    return buffer;
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
        drmd_kms_progress_page_flip();
    }
}

static uint32_t object_refcount(const drmd_kms_buffer_t *buffer)
{
    uint32_t refs = 0;
    if (buffer != NULL && buffer->object != NULL) {
        memcpy(&refs, buffer->object, sizeof(refs));
    }
    return refs;
}

static int wait_buffer_idle(drmd_kms_buffer_t *buffer, int nowait)
{
    if (buffer == NULL || buffer->object == NULL) return 0;
    pump_device();
    if (object_refcount(buffer) <= 1) return 0;
    if (nowait) return -16;

    const uint64_t start = monotonic_ns();
    const uint64_t timeout = UINT64_C(15000000000);
    while (object_refcount(buffer) > 1) {
        (void)kb_handle_any_irq(UINT64_C(1000000));
        kb_run_deferred_work();
        drmd_kms_progress_page_flip();
        const uint64_t now = monotonic_ns();
        if (now >= start && now - start >= timeout) return -16;
    }
    return 0;
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

static drmd_kms_mode_blob_t *find_mode_blob(uint32_t id)
{
    if (id == 0 || id == DRMD_KMS_IN_FORMATS_BLOB_ID) return NULL;
    for (size_t i = 0; i < DRMD_KMS_BLOB_MAX; i++) {
        if (kms.mode_blobs[i].active && kms.mode_blobs[i].id == id) {
            return &kms.mode_blobs[i];
        }
    }
    return NULL;
}

static void maybe_free_mode_blob(drmd_kms_mode_blob_t *blob)
{
    if (blob != NULL && blob->active && !blob->user_ref && blob->state_refs == 0) {
        memset(blob, 0, sizeof(*blob));
    }
}

static int mode_blob_ref(uint32_t id)
{
    if (id == 0) return 0;
    drmd_kms_mode_blob_t *blob = find_mode_blob(id);
    if (blob == NULL || blob->state_refs == UINT32_MAX) return -2;
    blob->state_refs++;
    return 0;
}

static void mode_blob_unref(uint32_t id)
{
    drmd_kms_mode_blob_t *blob = find_mode_blob(id);
    if (blob == NULL || blob->state_refs == 0) return;
    blob->state_refs--;
    maybe_free_mode_blob(blob);
}

static int create_mode_blob(uint64_t owner, drmd_mode_create_blob_wire_t *wire)
{
    if (wire == NULL || wire->length != sizeof(drmd_modeinfo_t)) return -22;
    const drmd_modeinfo_t *mode = (const void *)wire->data;
    if (mode->hdisplay == 0 || mode->vdisplay == 0 ||
        mode->hdisplay > DRMD_KMS_MAX_WIDTH ||
        mode->vdisplay > DRMD_KMS_MAX_HEIGHT) return -22;
    for (size_t i = 0; i < DRMD_KMS_BLOB_MAX; i++) {
        drmd_kms_mode_blob_t *blob = &kms.mode_blobs[i];
        if (blob->active) continue;
        uint32_t id = kms.next_blob++;
        if (id <= DRMD_KMS_IN_FORMATS_BLOB_ID) {
            id = DRMD_KMS_IN_FORMATS_BLOB_ID + 1u;
            kms.next_blob = id + 1u;
        }
        memset(blob, 0, sizeof(*blob));
        blob->active = 1;
        blob->user_ref = 1;
        blob->owner = owner;
        blob->id = id;
        blob->length = wire->length;
        memcpy(blob->data, wire->data, wire->length);
        wire->blob_id = id;
        return 0;
    }
    return -28;
}

static int destroy_mode_blob(uint64_t owner, uint32_t id)
{
    drmd_kms_mode_blob_t *blob = find_mode_blob(id);
    if (blob == NULL || !blob->user_ref || blob->owner != owner) return -2;
    blob->user_ref = 0;
    maybe_free_mode_blob(blob);
    return 0;
}

static int mode_blob_mode(
    uint64_t owner,
    uint32_t id,
    int require_user_ref,
    drmd_modeinfo_t *out_mode)
{
    drmd_kms_mode_blob_t *blob = find_mode_blob(id);
    if (blob == NULL || blob->owner != owner ||
        (require_user_ref && !blob->user_ref) ||
        blob->length != sizeof(drmd_modeinfo_t)) return -2;
    if (out_mode != NULL) memcpy(out_mode, blob->data, sizeof(*out_mode));
    return 0;
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
    if (kms.ready) return 0;
    memset(&kms, 0, sizeof(kms));
    kms.next_handle = 1;
    kms.next_fb = 71;
    kms.next_token = 1;
    kms.next_magic = 1;
    kms.next_blob = DRMD_KMS_IN_FORMATS_BLOB_ID + 1u;
    kms.dpms = DRMD_MODE_DPMS_ON;
    kms.device_backend = (kb_device_backend_t *)island->device_backend;
    kms.drm_device = kb_drm_primary_device();
    kms.vgdev = kb_drm_device_private(kms.drm_device);
    fill_mode(&kms.current_mode);
    drmd_atomic_lifecycle_init(&kms.atomic);
    kms.atomic_pending.input_wait_fd = -1;
    const drmd_syncobj_fd_ops_t syncobj_ops = {
        .poll = syncobj_poll_fd,
        .signal = syncobj_signal_fd,
        .close = syncobj_close_fd,
    };
    drmd_syncobj_state_init(&kms.syncobjs, &syncobj_ops, NULL);
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
        find_symbol("virtio_gpu_fence_alloc", (void **)&kms.fence_alloc) != 0 ||
        find_symbol("virtio_gpu_notify", (void **)&kms.notify) != 0 ||
        find_symbol("virtio_gpu_array_alloc", (void **)&kms.array_alloc) != 0 ||
        find_symbol("virtio_gpu_array_add_obj", (void **)&kms.array_add) != 0 ||
        find_symbol("virtio_gpu_array_put_free", (void **)&kms.array_put_free) != 0 ||
        find_symbol("virtio_gpu_create_context", (void **)&kms.create_context) != 0 ||
        find_symbol("virtio_gpu_cmd_resource_create_3d", (void **)&kms.create_resource_3d) != 0 ||
        find_symbol("virtio_gpu_cmd_context_attach_resource", (void **)&kms.context_attach_resource) != 0 ||
        find_symbol("virtio_gpu_cmd_context_detach_resource", (void **)&kms.context_detach_resource) != 0 ||
        find_symbol("virtio_gpu_cmd_submit", (void **)&kms.submit_3d) != 0 ||
        find_symbol("virtio_gpu_cmd_transfer_from_host_3d", (void **)&kms.transfer_from_host_3d) != 0 ||
        find_symbol("virtio_gpu_cmd_transfer_to_host_3d", (void **)&kms.transfer_to_host_3d) != 0 ||
        drmd_virtio_gpu_unref_bridge_init(
            &kms.unref_bridge, kms.module, kms.vgdev) != 0) {
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
    for (drmd_kms_gem_handle_t *gem = kms.gem_handles; gem != NULL; gem = gem->next) {
        if (gem->active && gem->owner == owner && gem->handle == handle) {
            return gem;
        }
    }
    return NULL;
}

static drmd_kms_buffer_t *find_buffer_token(uint64_t token)
{
    for (drmd_kms_buffer_t *buffer = kms.buffers; buffer != NULL; buffer = buffer->next) {
        if (buffer->active && buffer->token == token) {
            return buffer;
        }
    }
    return NULL;
}

static drmd_kms_buffer_t *find_buffer_offset(uint64_t owner, uint64_t offset, uint64_t length)
{
    for (drmd_kms_gem_handle_t *gem = kms.gem_handles; gem != NULL; gem = gem->next) {
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
    drmd_kms_gem_handle_t *gem = kms.gem_handles;
    while (gem != NULL && gem->active) gem = gem->next;
    if (gem == NULL) {
        gem = calloc(1, sizeof(*gem));
        if (gem == NULL) return NULL;
        gem->next = kms.gem_handles;
        kms.gem_handles = gem;
    }
    drmd_kms_gem_handle_t *const next = gem->next;
    memset(gem, 0, sizeof(*gem));
    gem->next = next;
    gem->active = 1;
    gem->owner = owner;
    gem->handle = kms.next_handle++;
    if (kms.next_handle == 0) kms.next_handle = 1;
    gem->buffer = buffer;
    buffer->handle_refs++;
    return gem;
}

static void finalize_destroyed_buffer(drmd_kms_buffer_t *buffer)
{
    if (buffer == NULL || !buffer->active || !buffer->destroy_requested) return;
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

static void poison_buffer_destruction(
    drmd_kms_buffer_t *buffer,
    int status,
    const drmd_virtio_gpu_unref_result_t *result)
{
    if (buffer == NULL || buffer->destroy_poisoned) return;
    buffer->destroy_poisoned = 1;
    printf("[drmd] retaining failed RESOURCE_UNREF resource=%u status=%d response=0x%x flags=0x%x\n",
        buffer->resource_id,
        status,
        result != NULL ? result->response_type : 0,
        result != NULL ? result->response_flags : 0);
}

static void progress_buffer_destruction(drmd_kms_buffer_t *buffer)
{
    if (buffer == NULL || !buffer->active || !buffer->destroy_requested ||
        buffer->destroy_poisoned) return;
    if (buffer->object == NULL) {
        finalize_destroyed_buffer(buffer);
        return;
    }

    if (buffer->unref_request.submitted == 0) {
        if (object_refcount(buffer) > 1) return;
        drmd_kms_owner_context_t context;
        const int entered = enter_module(&context);
        if (!entered) {
            poison_buffer_destruction(buffer, -5, NULL);
            return;
        }
        void *fence = kms.fence_alloc(kms.vgdev, 0, 0);
        int status = fence != NULL ?
            drmd_virtio_gpu_unref_request_submit(
                &kms.unref_bridge,
                &buffer->unref_request,
                buffer->object,
                fence) : -12;
        const uint32_t submitted_refs =
            status == 0 ? drmd_dma_fence_refcount(fence) : 0;
        if (status == 0) kms.notify(kms.vgdev);
        leave_module(&context);
        if (status != 0) {
            if (fence != NULL) kb_kfree(fence);
            poison_buffer_destruction(buffer, status, NULL);
            return;
        }
        if (submitted_refs != 2) {
            poison_buffer_destruction(buffer, -5, NULL);
            return;
        }
    }

    drmd_virtio_gpu_unref_result_t result;
    memset(&result, 0, sizeof(result));
    const int status = drmd_virtio_gpu_unref_request_poll(
        &buffer->unref_request, &result);
    if (status == 0) return;
    if (status < 0 ||
        drmd_dma_fence_refcount(buffer->unref_request.fence) != 1) {
        poison_buffer_destruction(buffer, status < 0 ? status : -5, &result);
        return;
    }
    kb_kfree(buffer->unref_request.fence);
    finalize_destroyed_buffer(buffer);
}

static void progress_buffer_destructions(void)
{
    for (drmd_kms_buffer_t *buffer = kms.buffers;
         buffer != NULL;
         buffer = buffer->next) {
        progress_buffer_destruction(buffer);
    }
}

static void destroy_buffer(drmd_kms_buffer_t *buffer)
{
    if (buffer == NULL || !buffer->active || buffer->destroy_requested) return;
    buffer->destroy_requested = 1;
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
    if (detach_gem_context(gem) != 0) return;
    drmd_kms_buffer_t *buffer = gem->buffer;
    drmd_kms_gem_handle_t *const next = gem->next;
    memset(gem, 0, sizeof(*gem));
    gem->next = next;
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

static drmd_kms_fb_t *find_fb_id(uint32_t id)
{
    for (size_t i = 0; i < DRMD_KMS_FB_MAX; i++) {
        if (kms.fb[i].active && kms.fb[i].id == id) return &kms.fb[i];
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
    for (drmd_kms_buffer_t *candidate = kms.buffers;
         candidate != NULL;
         candidate = candidate->next) {
        if (!candidate->active && candidate->cached && candidate->size >= requested_size &&
            (buffer == NULL || candidate->size < buffer->size)) {
            buffer = candidate;
        }
    }
    if (buffer == NULL) {
        for (drmd_kms_buffer_t *candidate = kms.buffers;
             candidate != NULL;
             candidate = candidate->next) {
            if (!candidate->active && !candidate->cached) {
                buffer = candidate;
                break;
            }
        }
    }
    if (buffer == NULL) buffer = alloc_buffer_record();
    if (buffer == NULL) return -12;
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

int drmd_kms_prime_import(
    uint64_t owner,
    void *drm_file,
    uint64_t token,
    uint32_t flags,
    uint32_t *out_gem_handle)
{
    if (drm_file == NULL || token == 0 || flags != 0 || out_gem_handle == NULL) return -22;
    drmd_kms_buffer_t *buffer = find_buffer_token(token);
    if (buffer == NULL || buffer->export_refs == 0) return -9;
    for (drmd_kms_gem_handle_t *gem = kms.gem_handles; gem != NULL; gem = gem->next) {
        if (gem->active && gem->owner == owner && gem->buffer == buffer) {
            *out_gem_handle = gem->handle;
            return 0;
        }
    }
    drmd_kms_gem_handle_t *gem = alloc_gem_handle(owner, buffer);
    if (gem == NULL) return -24;
    if (buffer->virgl) {
        const int attach_status = attach_gem_context(drm_file, gem);
        if (attach_status != 0) {
            release_gem_handle(gem);
            return attach_status;
        }
    }
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
    for (drmd_kms_buffer_t *candidate = kms.buffers;
         candidate != NULL;
         candidate = candidate->next) {
        if (!candidate->active && !candidate->cached) {
            buffer = candidate;
            break;
        }
    }
    if (buffer == NULL) buffer = alloc_buffer_record();
    if (buffer == NULL) return -12;
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
    for (drmd_kms_buffer_t *buffer = kms.buffers; buffer != NULL; buffer = buffer->next) {
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

static int submit_scanout_fb(
    drmd_kms_fb_t *fb,
    int consume_buffer_acquire,
    void **out_fence)
{
    drmd_kms_buffer_t *buffer = fb != NULL ? fb->buffer : NULL;
    if (buffer == NULL || out_fence == NULL) return -2;
    *out_fence = NULL;
    if (consume_buffer_acquire) {
        const int acquire_status = consume_acquire_sync_file(buffer);
        if (acquire_status != 0) return acquire_status;
    }
    drmd_kms_owner_context_t context;
    const int entered = enter_module(&context);
    void *fence = kms.fence_alloc(kms.vgdev, 0, 0);
    if (fence == NULL) {
        if (entered) leave_module(&context);
        return -12;
    }
    if (!buffer->virgl) {
        void *objects = kms.array_alloc(1);
        if (objects == NULL) {
            kb_kfree(fence);
            if (entered) leave_module(&context);
            return -12;
        }
        kms.array_add(objects, buffer->object);
        kms.transfer_2d(kms.vgdev, 0, fb->width, fb->height, 0, 0, objects, NULL);
    }
    kms.set_scanout(kms.vgdev, 0, buffer->resource_id, fb->width, fb->height, 0, 0);
    kms.flush(kms.vgdev, buffer->resource_id, 0, 0, fb->width, fb->height, NULL, fence);
    const uint32_t submitted_refs = drmd_dma_fence_refcount(fence);
    kms.notify(kms.vgdev);
    if (entered) leave_module(&context);
    if (submitted_refs != 2) {
        if (submitted_refs == 1) kb_kfree(fence);
        return -5;
    }
    *out_fence = fence;
    return 0;
}

static int wait_scanout_fence(void *fence)
{
    if (fence == NULL || drmd_dma_fence_refcount(fence) != 2) return -5;
    while (drmd_dma_fence_refcount(fence) == 2) {
        (void)kb_handle_any_irq(UINT64_C(1000000));
        kb_run_deferred_work();
        drmd_kms_progress_page_flip();
    }
    return drmd_dma_fence_refcount(fence) == 1 ? 0 : -5;
}

static int scanout_fb(drmd_kms_fb_t *fb)
{
    if (kms.pending_flip.active || kms.atomic_pending.active) return -16;
    void *fence = NULL;
    int status = submit_scanout_fb(fb, 1, &fence);
    if (status != 0) return status;
    status = wait_scanout_fence(fence);
    if (status != 0) return status;
    kb_kfree(fence);
    kms.current_fb = fb->id;
    return 0;
}

static int submit_disable_scanout(void **out_fence)
{
    if (out_fence == NULL) return -22;
    *out_fence = NULL;
    drmd_kms_fb_t *current = find_fb_id(kms.current_fb);
    drmd_kms_buffer_t *buffer = current != NULL ? current->buffer : NULL;
    drmd_kms_owner_context_t context;
    const int entered = enter_module(&context);
    void *fence = NULL;
    if (buffer != NULL) {
        fence = kms.fence_alloc(kms.vgdev, 0, 0);
        if (fence == NULL) {
            if (entered) leave_module(&context);
            return -12;
        }
    }
    kms.set_scanout(kms.vgdev, 0, 0, 0, 0, 0, 0);
    if (fence != NULL) {
        kms.flush(kms.vgdev, buffer->resource_id, 0, 0,
            current->width, current->height, NULL, fence);
    }
    const uint32_t submitted_refs =
        fence != NULL ? drmd_dma_fence_refcount(fence) : 0;
    kms.notify(kms.vgdev);
    if (entered) leave_module(&context);
    if (fence != NULL) {
        if (submitted_refs != 2) {
            if (submitted_refs == 1) kb_kfree(fence);
            return -5;
        }
        *out_fence = fence;
    }
    return 0;
}

static int disable_scanout(void)
{
    if (kms.pending_flip.active || kms.atomic_pending.active) return -16;
    void *fence = NULL;
    int status = submit_disable_scanout(&fence);
    if (status != 0) return status;
    if (fence != NULL) {
        status = wait_scanout_fence(fence);
        if (status != 0) return status;
        kb_kfree(fence);
    } else {
        pump_device();
    }
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
    uint64_t user_data)
{
    const uint32_t tail =
        (event_file->head + event_file->count) % DRMD_KMS_EVENT_QUEUE_MAX;
    drmd_event_vblank_t *event = &event_file->events[tail];
    memset(event, 0, sizeof(*event));
    const uint64_t queued_ns = monotonic_ns();
    event->type = DRMD_EVENT_FLIP_COMPLETE;
    event->length = sizeof(*event);
    event->user_data = user_data;
    event->tv_sec = (uint32_t)(queued_ns / UINT64_C(1000000000));
    event->tv_usec = (uint32_t)((queued_ns % UINT64_C(1000000000)) / 1000u);
    event->sequence = ++kms.sequence;
    event->crtc_id = DRMD_KMS_CRTC_ID;
    event_file->count++;
}

static void atomic_pending_clear(void)
{
    if (kms.atomic_pending.input_wait_fd >= 16) {
        (void)pacha_fd_close(kms.atomic_pending.input_wait_fd);
    }
    memset(&kms.atomic_pending, 0, sizeof(kms.atomic_pending));
    kms.atomic_pending.input_wait_fd = -1;
}

static void atomic_finish(int status)
{
    if (!kms.atomic_pending.active) return;
    const int blocking = kms.atomic_pending.blocking;
    if (status == 0) {
        const uint32_t old_mode = kms.atomic.presented.crtc_mode_id;
        const uint32_t new_mode = kms.atomic_pending.snapshot.crtc_mode_id;
        if (drmd_atomic_lifecycle_complete(&kms.atomic) != 0) {
            status = -5;
        } else {
            mode_blob_unref(old_mode);
            kms.current_fb = kms.atomic.presented.plane_fb_id;
            if (new_mode != 0) {
                drmd_modeinfo_t mode;
                if (mode_blob_mode(
                        kms.atomic_pending.owner, new_mode, 0, &mode) == 0) {
                    kms.current_mode = mode;
                }
            }
            if ((kms.atomic_pending.flags & DRMD_MODE_PAGE_FLIP_EVENT) != 0) {
                drmd_kms_event_file_t *event_file =
                    find_event_file(kms.atomic_pending.owner);
                if (event_file != NULL &&
                    event_file->count < DRMD_KMS_EVENT_QUEUE_MAX) {
                    queue_flip_event(event_file, kms.atomic_pending.user_data);
                }
            }
        }
    }
    if (status != 0) {
        mode_blob_unref(kms.atomic_pending.mode_blob_id);
        drmd_atomic_lifecycle_cancel(&kms.atomic);
    }
    atomic_pending_clear();
    if (blocking) {
        kms.deferred_result_status = status;
        kms.deferred_result_ready = 1;
    }
}

static int atomic_acquire_ready(void)
{
    if (kms.atomic_pending.input_wait_fd < 16) return 1;
    return syncobj_poll_fd(NULL, kms.atomic_pending.input_wait_fd);
}

static int atomic_submit_ready(void)
{
    if (!kms.atomic_pending.active ||
        kms.atomic_pending.phase != DRMD_ATOMIC_PENDING_WAIT_ACQUIRE) return 0;
    const int ready = atomic_acquire_ready();
    if (ready <= 0) return ready;
    if (kms.atomic_pending.input_wait_fd >= 16) {
        const int wait_fd = kms.atomic_pending.input_wait_fd;
        kms.atomic_pending.input_wait_fd = -1;
        (void)pacha_fd_close(wait_fd);
    }

    void *fence = NULL;
    int status;
    if (kms.atomic_pending.snapshot.plane_fb_id != 0) {
        drmd_kms_fb_t *fb = find_fb(
            kms.atomic_pending.owner,
            kms.atomic_pending.snapshot.plane_fb_id);
        status = fb != NULL ? submit_scanout_fb(fb, 0, &fence) : -2;
    } else {
        status = submit_disable_scanout(&fence);
    }
    if (status != 0) return status;
    kms.atomic_pending.phase = DRMD_ATOMIC_PENDING_SUBMITTED;
    if (fence == NULL) {
        atomic_finish(0);
        return 1;
    }
    status = drmd_flip_completion_begin(
        &kms.pending_flip,
        kms.atomic_pending.owner,
        kms.atomic_pending.user_data,
        kms.atomic_pending.snapshot.plane_fb_id,
        fence);
    if (status != 0) {
        return status;
    }
    return 1;
}

static void atomic_progress(void)
{
    drmd_syncobj_progress(&kms.syncobjs);
    if (!kms.atomic_pending.active ||
        kms.atomic_pending.phase != DRMD_ATOMIC_PENDING_WAIT_ACQUIRE) return;
    const int status = atomic_submit_ready();
    if (status < 0) atomic_finish(status);
}

void drmd_kms_progress_page_flip(void)
{
    progress_buffer_destructions();
    atomic_progress();
    drmd_flip_completion_t completed;
    if (!drmd_flip_completion_take(&kms.pending_flip, &completed)) return;
    if (kms.atomic_pending.active &&
        kms.atomic_pending.phase == DRMD_ATOMIC_PENDING_SUBMITTED &&
        kms.atomic_pending.owner == completed.owner) {
        atomic_finish(0);
    } else {
        kms.current_fb = completed.fb_id;
        drmd_kms_event_file_t *event_file = find_event_file(completed.owner);
        if (event_file != NULL && event_file->count < DRMD_KMS_EVENT_QUEUE_MAX) {
            queue_flip_event(event_file, completed.user_data);
        }
    }
    kb_kfree(completed.fence);
}

size_t drmd_kms_collect_wait_sources(int *out_fds, size_t capacity)
{
    if (out_fds == NULL || capacity == 0) return 0;
    size_t count = drmd_syncobj_collect_wait_fds(
        &kms.syncobjs, out_fds, capacity);
    if (count < capacity && kms.atomic_pending.active &&
        kms.atomic_pending.phase == DRMD_ATOMIC_PENDING_WAIT_ACQUIRE &&
        kms.atomic_pending.input_wait_fd >= 16) {
        out_fds[count++] = kms.atomic_pending.input_wait_fd;
    }
    return count;
}

int drmd_kms_take_deferred_ioctl_result(int *out_status)
{
    if (out_status == NULL || !kms.deferred_result_ready) return 0;
    *out_status = kms.deferred_result_status;
    kms.deferred_result_ready = 0;
    kms.deferred_result_status = 0;
    return 1;
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

static int ioctl_connector(uint64_t owner, drmd_kms_connector_wire_t *wire)
{
    if (wire == NULL || wire->value.connector_id != DRMD_KMS_CONNECTOR_ID) return -2;
    const drmd_kms_event_file_t *event_file = find_event_file(owner);
    if (event_file == NULL) return -9;
    const uint32_t mode_capacity = wire->value.count_modes;
    const uint32_t encoder_capacity = wire->value.count_encoders;
    const uint32_t prop_capacity = wire->value.count_props;
    if (mode_capacity != 0) wire->modes[0] = kms.current_mode;
    if (encoder_capacity != 0) wire->encoders[0] = DRMD_KMS_ENCODER_ID;
    if (prop_capacity != 0) {
        wire->props[0] = DRMD_KMS_DPMS_PROP_ID;
        wire->prop_values[0] = kms.dpms;
    }
    if (event_file->atomic && prop_capacity > 1) {
        wire->props[1] = DRMD_KMS_CONNECTOR_CRTC_ID_PROP_ID;
        wire->prop_values[1] = kms.atomic.presented.connector_crtc_id;
    }
    wire->value.count_modes = 1;
    wire->value.count_props = event_file->atomic ? 2 : 1;
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

static void put_object_property(
    drmd_kms_object_properties_wire_t *wire,
    uint32_t capacity,
    uint32_t index,
    uint32_t property,
    uint64_t value)
{
    if (index >= capacity || index >= DRMD_KMS_PROPERTY_CAPACITY) return;
    wire->props[index] = property;
    wire->prop_values[index] = value;
}

static int ioctl_object_properties(
    uint64_t owner,
    drmd_kms_object_properties_wire_t *wire)
{
    if (wire == NULL) return -22;
    const drmd_kms_event_file_t *event_file = find_event_file(owner);
    if (event_file == NULL) return -9;
    const uint32_t capacity = wire->value.count_props;
    uint32_t count = 0;
    if (wire->value.obj_id == DRMD_KMS_CONNECTOR_ID &&
        (wire->value.obj_type == DRMD_MODE_OBJECT_ANY ||
         wire->value.obj_type == DRMD_MODE_OBJECT_CONNECTOR)) {
        put_object_property(wire, capacity, count++,
            DRMD_KMS_DPMS_PROP_ID, kms.dpms);
        if (event_file->atomic) {
            put_object_property(wire, capacity, count++,
                DRMD_KMS_CONNECTOR_CRTC_ID_PROP_ID,
                kms.atomic.presented.connector_crtc_id);
        }
    } else if (wire->value.obj_id == DRMD_KMS_CRTC_ID &&
        (wire->value.obj_type == DRMD_MODE_OBJECT_ANY ||
         wire->value.obj_type == DRMD_MODE_OBJECT_CRTC)) {
        if (event_file->atomic) {
            put_object_property(wire, capacity, count++,
                DRMD_KMS_CRTC_ACTIVE_PROP_ID,
                kms.atomic.presented.crtc_active);
            put_object_property(wire, capacity, count++,
                DRMD_KMS_CRTC_MODE_ID_PROP_ID,
                kms.atomic.presented.crtc_mode_id);
        }
    } else if (wire->value.obj_id == DRMD_KMS_PLANE_ID &&
        (wire->value.obj_type == DRMD_MODE_OBJECT_ANY ||
         wire->value.obj_type == DRMD_MODE_OBJECT_PLANE)) {
        put_object_property(wire, capacity, count++,
            DRMD_KMS_PLANE_TYPE_PROP_ID, DRMD_MODE_PLANE_TYPE_PRIMARY);
        put_object_property(wire, capacity, count++,
            DRMD_KMS_IN_FORMATS_PROP_ID, DRMD_KMS_IN_FORMATS_BLOB_ID);
        if (event_file->atomic) {
            const drmd_atomic_snapshot_t *state = &kms.atomic.presented;
            put_object_property(wire, capacity, count++, DRMD_KMS_PLANE_SRC_X_PROP_ID, state->src_x);
            put_object_property(wire, capacity, count++, DRMD_KMS_PLANE_SRC_Y_PROP_ID, state->src_y);
            put_object_property(wire, capacity, count++, DRMD_KMS_PLANE_SRC_W_PROP_ID, state->src_w);
            put_object_property(wire, capacity, count++, DRMD_KMS_PLANE_SRC_H_PROP_ID, state->src_h);
            put_object_property(wire, capacity, count++, DRMD_KMS_PLANE_CRTC_X_PROP_ID, (uint64_t)state->crtc_x);
            put_object_property(wire, capacity, count++, DRMD_KMS_PLANE_CRTC_Y_PROP_ID, (uint64_t)state->crtc_y);
            put_object_property(wire, capacity, count++, DRMD_KMS_PLANE_CRTC_W_PROP_ID, state->crtc_w);
            put_object_property(wire, capacity, count++, DRMD_KMS_PLANE_CRTC_H_PROP_ID, state->crtc_h);
            put_object_property(wire, capacity, count++, DRMD_KMS_PLANE_FB_ID_PROP_ID, state->plane_fb_id);
            put_object_property(wire, capacity, count++, DRMD_KMS_PLANE_CRTC_ID_PROP_ID, state->plane_crtc_id);
            put_object_property(wire, capacity, count++, DRMD_KMS_PLANE_IN_FENCE_FD_PROP_ID, UINT64_MAX);
        }
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

static int atomic_property_id(uint32_t id)
{
    return id >= DRMD_KMS_CONNECTOR_CRTC_ID_PROP_ID &&
        id <= DRMD_KMS_PLANE_IN_FENCE_FD_PROP_ID;
}

static int ioctl_property(uint64_t owner, drmd_kms_property_wire_t *wire)
{
    if (wire == NULL) return -22;
    const drmd_kms_event_file_t *event_file = find_event_file(owner);
    if (event_file == NULL) return -9;
    if (atomic_property_id(wire->value.prop_id) && !event_file->atomic) return -2;
    const uint32_t enum_capacity = wire->value.count_enum_blobs;
    const uint32_t value_capacity = wire->value.count_values;
    memset(wire->value.name, 0, sizeof(wire->value.name));
    wire->value.count_values = 0;
    if (atomic_property_id(wire->value.prop_id)) {
        drmd_property_metadata_t metadata;
        const int status = drmd_atomic_property_metadata(
            wire->value.prop_id, &metadata);
        if (status != 0) return status;
        wire->value.flags = metadata.flags;
        memcpy(wire->value.name, metadata.name, sizeof(wire->value.name));
        for (uint32_t i = 0; i < metadata.count_values &&
            i < value_capacity && i < DRMD_KMS_PROPERTY_VALUE_CAPACITY; i++) {
            wire->values[i] = metadata.values[i];
        }
        wire->value.count_values = metadata.count_values;
        wire->value.count_enum_blobs = 0;
        return 0;
    }
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

static int ioctl_property_blob(uint64_t owner, drmd_kms_property_blob_wire_t *wire)
{
    if (wire == NULL) return -22;
    if (wire->value.blob_id != DRMD_KMS_IN_FORMATS_BLOB_ID) {
        drmd_kms_mode_blob_t *mode = find_mode_blob(wire->value.blob_id);
        if (mode == NULL || mode->owner != owner) return -2;
        if (wire->value.length != 0) {
            const uint32_t length = wire->value.length < mode->length ?
                wire->value.length : mode->length;
            memcpy(wire->data, mode->data, length);
        }
        wire->value.length = mode->length;
        return 0;
    }
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

static int render_context_id(void *drm_file, uint32_t *out_context_id)
{
    if (drm_file == NULL || out_context_id == NULL) return -22;
    void *private_data = NULL;
    memcpy(&private_data,
        (const uint8_t *)drm_file + DRMD_DRM_FILE_DRIVER_PRIV_OFFSET,
        sizeof(private_data));
    if (private_data == NULL) return -19;
    kms.create_context(kms.drm_device, drm_file);
    memcpy(out_context_id, private_data, sizeof(*out_context_id));
    return *out_context_id != 0 ? 0 : -19;
}

static int render_context_available(void *drm_file)
{
    void *private_data = NULL;
    if (drm_file != NULL) {
        memcpy(&private_data,
            (const uint8_t *)drm_file + DRMD_DRM_FILE_DRIVER_PRIV_OFFSET,
            sizeof(private_data));
    }
    return private_data != NULL;
}

static int attach_gem_context(void *drm_file, drmd_kms_gem_handle_t *gem)
{
    if (drm_file == NULL || gem == NULL || !gem->active || gem->buffer == NULL ||
        gem->buffer->object == NULL) return -22;

    drmd_kms_owner_context_t context;
    const int entered = enter_module(&context);
    uint32_t context_id = 0;
    int status = render_context_id(drm_file, &context_id);
    if (status == 0 && gem->context_attached) {
        status = gem->context_id == context_id ? 0 : -22;
    }
    void *objects = NULL;
    if (status == 0 && !gem->context_attached) {
        objects = kms.array_alloc(1);
        if (objects == NULL) {
            status = -12;
        } else {
            kms.array_add(objects, gem->buffer->object);
            kms.context_attach_resource(kms.vgdev, context_id, objects);
            kms.notify(kms.vgdev);
            objects = NULL;
            gem->context_id = context_id;
            gem->context_attached = 1;
        }
    }
    if (objects != NULL) kms.array_put_free(objects);
    if (entered) leave_module(&context);
    if (status == 0) pump_device();
    return status;
}

static int detach_gem_context(drmd_kms_gem_handle_t *gem)
{
    if (gem == NULL || !gem->active || !gem->context_attached) return 0;
    drmd_kms_buffer_t *buffer = gem->buffer;
    if (buffer == NULL || buffer->object == NULL) return -22;
    if (wait_buffer_idle(buffer, 0) != 0) return -16;

    drmd_kms_owner_context_t context;
    const int entered = enter_module(&context);
    void *objects = kms.array_alloc(1);
    if (objects == NULL) {
        if (entered) leave_module(&context);
        return -12;
    }
    kms.array_add(objects, buffer->object);
    kms.context_detach_resource(kms.vgdev, gem->context_id, objects);
    kms.notify(kms.vgdev);
    if (entered) leave_module(&context);
    if (wait_buffer_idle(buffer, 0) != 0) return -16;
    gem->context_attached = 0;
    gem->context_id = 0;
    return 0;
}

static drmd_kms_buffer_t *alloc_render_buffer_slot(void)
{
    for (drmd_kms_buffer_t *buffer = kms.buffers; buffer != NULL; buffer = buffer->next) {
        if (!buffer->active && !buffer->cached) return buffer;
    }
    return alloc_buffer_record();
}

static int create_render_resource(
    struct drmd_drm_island *island,
    uint64_t owner,
    void *drm_file,
    drmd_virtgpu_resource_create_t *args)
{
    if (island == NULL || args == NULL || drm_file == NULL ||
        args->bo_handle != 0 || args->res_handle != 0) return -22;
    uint64_t size = args->size != 0 ? args->size : 4096u;
    size = (size + 4095u) & ~UINT64_C(4095);
    if (size == 0 || size > 256u * 1024u * 1024u) return -12;
    drmd_kms_buffer_t *buffer = alloc_render_buffer_slot();
    if (buffer == NULL) return -24;

    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const int vmo_fd = pacha_vmo_create(size, rights, 0);
    if (vmo_fd < 16) return -12;
    void *mapping = pacha_mmap(
        vmo_fd, size, PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED, 0);
    if (mapping == NULL) {
        (void)pacha_fd_close(vmo_fd);
        return -12;
    }
    kb_status_t dma_status = KB_ERR_INVALID;
    const uint64_t dma_addr = kb_subsystem_dma_map(
        (kb_device_backend_t *)island->device_backend,
        NULL,
        mapping,
        size,
        KB_DMA_TO_DEVICE,
        &dma_status);
    if (dma_status != KB_OK || dma_addr == 0) {
        (void)pacha_munmap(mapping, size);
        (void)pacha_fd_close(vmo_fd);
        return -5;
    }
    void *object = kb_kzalloc(DRMD_VIRTIO_GPU_OBJECT_BYTES, 0);
    if (object == NULL) {
        kb_subsystem_dma_unmap(kms.device_backend, NULL, dma_addr, size, KB_DMA_TO_DEVICE);
        (void)pacha_munmap(mapping, size);
        (void)pacha_fd_close(vmo_fd);
        return -12;
    }
    *(uint32_t *)object = 1;
    memcpy((uint8_t *)object + DRMD_GEM_DEV_OFFSET,
        &kms.drm_device, sizeof(kms.drm_device));

    uint32_t resource_id = 0;
    uint32_t context_id = 0;
    int status = 0;
    drmd_virtio_gpu_mem_entry_t *entry = NULL;
    void *objects = NULL;
    drmd_kms_owner_context_t context;
    const int entered = enter_module(&context);
    status = render_context_id(drm_file, &context_id);
    if (status == 0) {
        entry = kb_kzalloc(sizeof(*entry), 0);
        objects = entry != NULL ? kms.array_alloc(1) : NULL;
        if (entry == NULL || objects == NULL) {
            if (entry != NULL) kb_kfree(entry);
            if (objects != NULL) kms.array_put_free(objects);
            entry = NULL;
            objects = NULL;
            status = -12;
        }
    }
    if (status == 0) status = kms.resource_id_get(kms.vgdev, &resource_id);
    if (status == 0) {
        drmd_virtio_gpu_object_params_t params;
        memset(&params, 0, sizeof(params));
        params.size = size;
        params.virgl = 1;
        params.format = args->format;
        params.width = args->width;
        params.height = args->height;
        params.target = args->target;
        params.bind = args->bind;
        params.depth = args->depth;
        params.array_size = args->array_size;
        params.last_level = args->last_level;
        params.nr_samples = args->nr_samples;
        params.flags = args->flags;
        memcpy((uint8_t *)object + 0x198, &resource_id, sizeof(resource_id));
        kms.create_resource_3d(kms.vgdev, object, &params, NULL, NULL);
        entry->addr = dma_addr;
        entry->length = (uint32_t)size;
        kms.object_attach(kms.vgdev, object, entry, 1);
        kms.array_add(objects, object);
        kms.context_attach_resource(kms.vgdev, context_id, objects);
        kms.notify(kms.vgdev);
    } else {
        if (objects != NULL) kms.array_put_free(objects);
        if (entry != NULL) kb_kfree(entry);
    }
    if (entered) leave_module(&context);
    pump_device();
    if (status != 0) {
        kb_kfree(object);
        kb_subsystem_dma_unmap(kms.device_backend, NULL, dma_addr, size, KB_DMA_TO_DEVICE);
        (void)pacha_munmap(mapping, size);
        (void)pacha_fd_close(vmo_fd);
        return status;
    }

    reset_buffer(buffer);
    buffer->active = 1;
    buffer->virgl = 1;
    buffer->resource_id = resource_id;
    buffer->width = args->width;
    buffer->height = args->height;
    buffer->pitch = args->stride;
    buffer->size = size;
    buffer->vmo_fd = vmo_fd;
    buffer->mapping = mapping;
    buffer->dma_addr = dma_addr;
    buffer->object = object;
    drmd_kms_gem_handle_t *gem = alloc_gem_handle(owner, buffer);
    if (gem == NULL) {
        destroy_buffer(buffer);
        return -24;
    }
    buffer->mmap_offset = (uint64_t)gem->handle << 32u;
    gem->context_attached = 1;
    gem->context_id = context_id;
    args->bo_handle = gem->handle;
    args->res_handle = resource_id;
    args->size = (uint32_t)size;
    return 0;
}

static int submit_render_commands(
    uint64_t owner,
    void *drm_file,
    drmd_virtgpu_execbuffer_t *args,
    void *aux_data,
    uint64_t aux_size)
{
    if (args == NULL || drm_file == NULL || aux_data == NULL ||
        args->flags != 0 || args->size == 0 || args->command != 0 ||
        args->num_in_syncobjs != 0 || args->num_out_syncobjs != 0) return -22;
    const uint64_t command_bytes = ((uint64_t)args->size + 7u) & ~UINT64_C(7);
    const uint64_t handles_bytes =
        (uint64_t)args->num_bo_handles * sizeof(uint32_t);
    if (command_bytes < args->size || args->bo_handles != command_bytes ||
        command_bytes > aux_size || handles_bytes > aux_size - command_bytes ||
        command_bytes + handles_bytes != aux_size) return -22;
    const uint32_t *handles = (const uint32_t *)((const uint8_t *)aux_data + command_bytes);
    for (uint32_t i = 0; i < args->num_bo_handles; i++) {
        if (find_gem_handle(owner, handles[i]) == NULL) return -2;
    }

    drmd_kms_owner_context_t context;
    const int entered = enter_module(&context);
    uint32_t context_id = 0;
    int status = render_context_id(drm_file, &context_id);
    void *commands = NULL;
    void *objects = NULL;
    if (status == 0) {
        commands = kb_kmalloc(args->size, 0);
        if (commands == NULL) status = -12;
    }
    if (status == 0 && args->num_bo_handles != 0) {
        objects = kms.array_alloc(args->num_bo_handles);
        if (objects == NULL) status = -12;
    }
    if (status == 0) {
        memcpy(commands, aux_data, args->size);
        for (uint32_t i = 0; i < args->num_bo_handles; i++) {
            drmd_kms_gem_handle_t *gem = find_gem_handle(owner, handles[i]);
            status = attach_gem_context(drm_file, gem);
            if (status != 0) break;
            kms.array_add(objects, gem->buffer->object);
        }
    }
    if (status == 0) {
        kms.submit_3d(kms.vgdev, commands, args->size, context_id, objects, NULL);
        kms.notify(kms.vgdev);
        commands = NULL;
        objects = NULL;
    }
    if (objects != NULL) kms.array_put_free(objects);
    if (commands != NULL) kb_kfree(commands);
    if (entered) leave_module(&context);
    if (status == 0) pump_device();
    return status;
}

static int transfer_render_resource(
    uint64_t owner,
    void *drm_file,
    drmd_virtgpu_3d_transfer_t *args,
    int from_host)
{
    if (args == NULL || drm_file == NULL) return -22;
    drmd_kms_gem_handle_t *gem = find_gem_handle(owner, args->bo_handle);
    if (gem == NULL || gem->buffer == NULL) return -2;
    drmd_kms_owner_context_t context;
    const int entered = enter_module(&context);
    uint32_t context_id = 0;
    int status = render_context_id(drm_file, &context_id);
    void *objects = NULL;
    if (status == 0) {
        objects = kms.array_alloc(1);
        if (objects == NULL) status = -12;
    }
    if (status == 0) {
        kms.array_add(objects, gem->buffer->object);
        drmd_transfer_3d_fn transfer = from_host ?
            kms.transfer_from_host_3d : kms.transfer_to_host_3d;
        transfer(kms.vgdev, context_id, args->offset, args->level,
            args->stride, args->layer_stride, &args->box, objects, NULL);
        kms.notify(kms.vgdev);
        objects = NULL;
    }
    if (objects != NULL) kms.array_put_free(objects);
    if (entered) leave_module(&context);
    return status;
}

int drmd_kms_render_ioctl(
    struct drmd_drm_island *island,
    drmd_ioctl_request_t *request,
    void *drm_file,
    void *aux_data,
    uint64_t aux_size,
    int *out_handled)
{
    if (request == NULL || out_handled == NULL || !kms.ready) return -22;
    if (!render_context_available(drm_file)) {
        *out_handled = 0;
        return 0;
    }
    *out_handled = 1;
    switch ((uint32_t)request->request) {
    case DRMD_IOCTL_VIRTGPU_RESOURCE_CREATE:
        return request->data_size >= sizeof(drmd_virtgpu_resource_create_t) ?
            create_render_resource(island, request->handle, drm_file,
                (void *)request->data) : -22;
    case DRMD_IOCTL_VIRTGPU_RESOURCE_INFO: {
        if (request->data_size < sizeof(drmd_virtgpu_resource_info_t)) return -22;
        drmd_virtgpu_resource_info_t *info = (void *)request->data;
        drmd_kms_gem_handle_t *gem = find_gem_handle(request->handle, info->bo_handle);
        if (gem == NULL || gem->buffer == NULL) return -2;
        info->res_handle = gem->buffer->resource_id;
        info->size = (uint32_t)gem->buffer->size;
        info->blob_mem = 0;
        return 0;
    }
    case DRMD_IOCTL_VIRTGPU_MAP: {
        if (request->data_size < sizeof(drmd_virtgpu_map_t)) return -22;
        drmd_virtgpu_map_t *map = (void *)request->data;
        if (map->pad != 0) return -22;
        drmd_kms_gem_handle_t *gem = find_gem_handle(request->handle, map->handle);
        if (gem == NULL || gem->buffer == NULL) return -2;
        map->offset = gem->buffer->mmap_offset;
        return 0;
    }
    case DRMD_IOCTL_VIRTGPU_EXECBUFFER:
        return request->data_size >= sizeof(drmd_virtgpu_execbuffer_t) ?
            submit_render_commands(request->handle, drm_file,
                (void *)request->data, aux_data, aux_size) : -22;
    case DRMD_IOCTL_VIRTGPU_TRANSFER_FROM_HOST:
        return request->data_size >= sizeof(drmd_virtgpu_3d_transfer_t) ?
            transfer_render_resource(request->handle, drm_file,
                (void *)request->data, 1) : -22;
    case DRMD_IOCTL_VIRTGPU_TRANSFER_TO_HOST:
        return request->data_size >= sizeof(drmd_virtgpu_3d_transfer_t) ?
            transfer_render_resource(request->handle, drm_file,
                (void *)request->data, 0) : -22;
    case DRMD_IOCTL_VIRTGPU_WAIT: {
        if (request->data_size < sizeof(drmd_virtgpu_3d_wait_t)) return -22;
        const drmd_virtgpu_3d_wait_t *wait = (const void *)request->data;
        drmd_kms_gem_handle_t *gem = find_gem_handle(request->handle, wait->handle);
        if ((wait->flags & ~DRMD_VIRTGPU_WAIT_NOWAIT) != 0 ||
            gem == NULL || gem->buffer == NULL) return -22;
        return wait_buffer_idle(
            gem->buffer, (wait->flags & DRMD_VIRTGPU_WAIT_NOWAIT) != 0);
    }
    case DRMD_IOCTL_GEM_CLOSE: {
        if (request->data_size < sizeof(drmd_gem_close_t)) return -22;
        drmd_gem_close_t *close = (void *)request->data;
        drmd_kms_gem_handle_t *gem = find_gem_handle(request->handle, close->handle);
        if (gem == NULL || close->pad != 0) return -2;
        release_gem_handle(gem);
        return 0;
    }
    default:
        *out_handled = 0;
        return 0;
    }
}

static int ioctl_atomic(
    drmd_ioctl_request_t *request,
    drmd_ioctl_attachments_t *attachments)
{
    if (request == NULL || attachments == NULL ||
        request->data_size < sizeof(drmd_mode_atomic_wire_t) ||
        request->aux_size != 0) return -22;
    if (require_master(request->handle) != 0) return -13;
    drmd_kms_event_file_t *event_file = find_event_file(request->handle);
    if (event_file == NULL) return -9;
    if (!event_file->atomic) return -95;
    if (kms.pending_flip.active || kms.atomic_pending.active ||
        kms.atomic.pending_active) return -16;
    if ((request->fd_flags & DRMD_IOCTL_FD_OUTPUT_NOTIFY) != 0) return -22;

    drmd_mode_atomic_wire_t *wire = (void *)request->data;
    const int has_input =
        (request->fd_flags & DRMD_IOCTL_FD_INPUT_WAIT) != 0;
    drmd_atomic_stage_result_t staged;
    int status = drmd_atomic_stage(
        &kms.atomic.presented, wire, has_input, &staged);
    if (status != 0) return status;

    drmd_atomic_resource_info_t resources;
    memset(&resources, 0, sizeof(resources));
    drmd_modeinfo_t mode;
    memset(&mode, 0, sizeof(mode));
    if (staged.snapshot.crtc_mode_id != 0) {
        status = mode_blob_mode(
            request->handle,
            staged.snapshot.crtc_mode_id,
            staged.snapshot.crtc_mode_id !=
                kms.atomic.presented.crtc_mode_id,
            &mode);
        if (status != 0) return status;
        resources.mode_exists = 1;
        resources.mode_width = mode.hdisplay;
        resources.mode_height = mode.vdisplay;
    }
    if (staged.snapshot.plane_fb_id != 0) {
        drmd_kms_fb_t *fb = find_fb(
            request->handle, staged.snapshot.plane_fb_id);
        if (fb == NULL) return -2;
        resources.fb_exists = 1;
        resources.fb_width = fb->width;
        resources.fb_height = fb->height;
    }
    status = drmd_atomic_validate_resources(&staged.snapshot, &resources);
    if (status != 0) return status;
    if ((wire->flags & DRMD_MODE_PAGE_FLIP_EVENT) != 0 &&
        event_file->count >= DRMD_KMS_EVENT_QUEUE_MAX) return -28;
    if ((wire->flags & DRMD_MODE_ATOMIC_TEST_ONLY) != 0) return 0;

    status = mode_blob_ref(staged.snapshot.crtc_mode_id);
    if (status != 0) return status;
    status = drmd_atomic_lifecycle_begin(
        &kms.atomic, &staged.snapshot, 0);
    if (status != 0) {
        mode_blob_unref(staged.snapshot.crtc_mode_id);
        return status;
    }
    memset(&kms.atomic_pending, 0, sizeof(kms.atomic_pending));
    kms.atomic_pending.active = 1;
    kms.atomic_pending.phase = DRMD_ATOMIC_PENDING_WAIT_ACQUIRE;
    kms.atomic_pending.input_wait_fd = has_input ?
        attachments->input_wait_fd : -1;
    kms.atomic_pending.blocking =
        (wire->flags & DRMD_MODE_ATOMIC_NONBLOCK) == 0;
    kms.atomic_pending.owner = request->handle;
    kms.atomic_pending.user_data = wire->user_data;
    kms.atomic_pending.flags = wire->flags;
    kms.atomic_pending.mode_blob_id = staged.snapshot.crtc_mode_id;
    kms.atomic_pending.snapshot = staged.snapshot;
    if (has_input) {
        attachments->consumed_fd_flags |= DRMD_IOCTL_FD_INPUT_WAIT;
    }

    status = atomic_submit_ready();
    if (status < 0) {
        kms.atomic_pending.blocking = 0;
        atomic_finish(status);
        return status;
    }
    return (wire->flags & DRMD_MODE_ATOMIC_NONBLOCK) != 0 ?
        0 : DRMD_IOCTL_DEFERRED;
}

int drmd_kms_ioctl(
    struct drmd_drm_island *island,
    drmd_ioctl_request_t *request,
    drmd_ioctl_attachments_t *attachments,
    int *out_handled)
{
    if (out_handled == NULL || request == NULL || attachments == NULL ||
        !kms.ready) return -22;
    *out_handled = 1;
    const uint32_t command = (uint32_t)request->request;
    const int accepts_descriptors = command == DRMD_IOCTL_MODE_ATOMIC ||
        command == DRMD_IOCTL_SYNCOBJ_FD_TO_HANDLE ||
        command == DRMD_IOCTL_SYNCOBJ_HANDLE_TO_FD;
    if (request->aux_size != 0 ||
        (request->fd_flags != 0 && !accepts_descriptors)) return -22;
    switch (command) {
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
        case DRMD_CAP_SYNCOBJ:
        case DRMD_CAP_SYNCOBJ_TIMELINE:
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
            drmd_kms_event_file_t *event_file = find_event_file(request->handle);
            if (event_file == NULL) return -9;
            event_file->atomic = cap->value != 0;
            event_file->universal_planes = cap->value != 0;
            return 0;
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
        return request->data_size >= sizeof(drmd_kms_connector_wire_t) ?
            ioctl_connector(request->handle, (void *)request->data) : -22;
    case DRMD_IOCTL_MODE_GETPROPERTY:
        return request->data_size >= sizeof(drmd_kms_property_wire_t) ?
            ioctl_property(request->handle, (void *)request->data) : -22;
    case DRMD_IOCTL_MODE_GETPROPBLOB:
        return request->data_size >= sizeof(drmd_kms_property_blob_wire_t) ?
            ioctl_property_blob(request->handle, (void *)request->data) : -22;
    case DRMD_IOCTL_MODE_OBJ_GETPROPERTIES:
        return request->data_size >= sizeof(drmd_kms_object_properties_wire_t) ?
            ioctl_object_properties(request->handle, (void *)request->data) : -22;
    case DRMD_IOCTL_MODE_CREATEPROPBLOB:
        return request->data_size >= sizeof(drmd_mode_create_blob_wire_t) ?
            create_mode_blob(request->handle, (void *)request->data) : -22;
    case DRMD_IOCTL_MODE_DESTROYPROPBLOB:
        return request->data_size >= sizeof(uint32_t) ?
            destroy_mode_blob(request->handle, *(uint32_t *)request->data) : -22;
    case DRMD_IOCTL_MODE_ATOMIC:
        return ioctl_atomic(request, attachments);
    case DRMD_IOCTL_SYNCOBJ_CREATE: {
        if (request->data_size < sizeof(drmd_syncobj_create_t) ||
            request->fd_flags != 0) return -22;
        drmd_syncobj_create_t *create = (void *)request->data;
        return drmd_syncobj_create(
            &kms.syncobjs, request->handle, create->flags, &create->handle);
    }
    case DRMD_IOCTL_SYNCOBJ_DESTROY: {
        if (request->data_size < sizeof(drmd_syncobj_destroy_t) ||
            request->fd_flags != 0) return -22;
        const drmd_syncobj_destroy_t *destroy = (const void *)request->data;
        if (destroy->pad != 0) return -22;
        return drmd_syncobj_destroy(
            &kms.syncobjs, request->handle, destroy->handle);
    }
    case DRMD_IOCTL_SYNCOBJ_FD_TO_HANDLE: {
        if (request->data_size < sizeof(drmd_syncobj_handle_t) ||
            request->fd_flags != DRMD_IOCTL_FD_INPUT_WAIT) return -22;
        drmd_syncobj_handle_t *handle = (void *)request->data;
        if (handle->fd != -1 || handle->pad != 0 ||
            attachments->input_wait_fd < 16) return -22;
        const int status = drmd_syncobj_import_sync_file(
            &kms.syncobjs,
            request->handle,
            handle->handle,
            handle->flags,
            attachments->input_wait_fd);
        if (status == 0) {
            attachments->consumed_fd_flags |= DRMD_IOCTL_FD_INPUT_WAIT;
        }
        return status;
    }
    case DRMD_IOCTL_SYNCOBJ_HANDLE_TO_FD: {
        if (request->data_size < sizeof(drmd_syncobj_handle_t) ||
            request->fd_flags != DRMD_IOCTL_FD_OUTPUT_NOTIFY) return -22;
        drmd_syncobj_handle_t *handle = (void *)request->data;
        if (handle->fd != -1 || handle->pad != 0 ||
            attachments->output_notify_fd < 16) return -22;
        const int status = drmd_syncobj_export_sync_file(
            &kms.syncobjs,
            request->handle,
            handle->handle,
            handle->flags,
            attachments->output_notify_fd);
        if (status == 0) {
            attachments->consumed_fd_flags |= DRMD_IOCTL_FD_OUTPUT_NOTIFY;
        }
        return status;
    }
    case DRMD_IOCTL_SYNCOBJ_TRANSFER: {
        if (request->data_size < sizeof(drmd_syncobj_transfer_t) ||
            request->fd_flags != 0) return -22;
        const drmd_syncobj_transfer_t *transfer = (const void *)request->data;
        if (transfer->pad != 0) return -22;
        return drmd_syncobj_transfer(
            &kms.syncobjs,
            request->handle,
            transfer->src_handle,
            transfer->src_point,
            transfer->dst_handle,
            transfer->dst_point,
            transfer->flags);
    }
    case DRMD_IOCTL_SYNCOBJ_EVENTFD:
        return -95;
    case DRMD_IOCTL_MODE_CREATE_LEASE:
        return -95;
    case DRMD_IOCTL_MODE_SETPROPERTY: {
        if (require_master(request->handle) != 0 ||
            request->data_size < sizeof(drmd_mode_connector_set_property_t)) return -13;
        const drmd_mode_connector_set_property_t *property = (const void *)request->data;
        if (property->connector_id != DRMD_KMS_CONNECTOR_ID ||
            property->prop_id != DRMD_KMS_DPMS_PROP_ID ||
            property->value > DRMD_MODE_DPMS_OFF) return -22;
        if (property->value != DRMD_MODE_DPMS_ON) {
            const int status = disable_scanout();
            if (status != 0) return status;
        }
        kms.dpms = property->value;
        return 0;
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
        const int status = scanout_fb(fb);
        if (status == 0) kms.current_mode = wire->value.mode;
        return status;
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
        if (kms.current_fb == id ||
            (kms.pending_flip.active && kms.pending_flip.fb_id == id) ||
            (kms.atomic_pending.active &&
                kms.atomic_pending.snapshot.plane_fb_id == id)) return -16;
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
        if (kms.pending_flip.active || kms.atomic_pending.active) return -16;
        if (flip->flags == 0) return scanout_fb(fb);
        drmd_kms_event_file_t *event_file = find_event_file(request->handle);
        if (event_file == NULL) return -9;
        if (event_file->count >= DRMD_KMS_EVENT_QUEUE_MAX) return -28;
        void *fence = NULL;
        const int status = submit_scanout_fb(fb, 1, &fence);
        if (status != 0) return status;
        const int pending_status = drmd_flip_completion_begin(
            &kms.pending_flip,
            request->handle,
            flip->user_data,
            fb->id,
            fence);
        if (pending_status != 0) {
            if (wait_scanout_fence(fence) == 0) kb_kfree(fence);
            return pending_status;
        }
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

void drmd_kms_handle_open(uint64_t handle, uint32_t device_minor)
{
    if (device_minor != 0) return;
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
    if (kms.atomic_pending.active && kms.atomic_pending.owner == handle &&
        kms.atomic_pending.phase == DRMD_ATOMIC_PENDING_WAIT_ACQUIRE) {
        atomic_finish(-9);
    }
    while (kms.pending_flip.active && kms.pending_flip.owner == handle) {
        (void)kb_handle_any_irq(UINT64_C(1000000));
        kb_run_deferred_work();
        drmd_kms_progress_page_flip();
    }
    int owns_scanout = 0;
    for (size_t i = 0; i < DRMD_KMS_FB_MAX; i++) {
        if (kms.fb[i].active && kms.fb[i].owner == handle &&
            kms.fb[i].id == kms.current_fb) {
            owns_scanout = 1;
            break;
        }
    }
    if (owns_scanout) {
        (void)disable_scanout();
        mode_blob_unref(kms.atomic.presented.crtc_mode_id);
        drmd_atomic_lifecycle_init(&kms.atomic);
    }

    drmd_syncobj_owner_close(&kms.syncobjs, handle);
    for (size_t i = 0; i < DRMD_KMS_BLOB_MAX; i++) {
        drmd_kms_mode_blob_t *blob = &kms.mode_blobs[i];
        if (!blob->active || blob->owner != handle) continue;
        blob->user_ref = 0;
        maybe_free_mode_blob(blob);
    }

    drmd_kms_event_file_t *event_file = find_event_file(handle);
    if (event_file != NULL) memset(event_file, 0, sizeof(*event_file));
    if (kms.master_handle == handle) kms.master_handle = 0;
    for (size_t i = 0; i < DRMD_KMS_FB_MAX; i++) {
        drmd_kms_fb_t *fb = &kms.fb[i];
        if (!fb->active || fb->owner != handle) continue;
        drmd_kms_buffer_t *buffer = fb->buffer;
        if (buffer != NULL && buffer->fb_refs != 0) buffer->fb_refs--;
        memset(fb, 0, sizeof(*fb));
        maybe_destroy_buffer(buffer);
    }
    for (drmd_kms_gem_handle_t *gem = kms.gem_handles; gem != NULL; gem = gem->next) {
        if (gem->active && gem->owner == handle) {
            release_gem_handle(gem);
        }
    }
}

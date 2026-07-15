#include "drm_island.h"
#include "drm_kms.h"

#include <kobox/device_pachaos_capsule.h>
#include <kobox/module.h>
#include <kobox/platform.h>
#include <kobox/shim.h>
#include <pacha/ipc.h>
#include "linux_subsystem/fs/kernel_object_registry.h"
#include "linux_subsystem/kvm/kvm_symbols.h"
#include "loader/module_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DRMD_DRM_MAJOR = 226,
    DRMD_DRM_CARD0_MINOR = 0,
    DRMD_HANDLE_MAX = 64,
    DRMD_FAKE_INODE_BYTES = 768,
    DRMD_FAKE_MAPPING_BYTES = 256,
    DRMD_FAKE_FILE_BYTES = 1024,
    DRMD_FILE_MODE_OFFSET = 0x14,
    DRMD_FILE_FLAGS_OFFSET = 0x48,
    DRMD_FILE_INODE_OFFSET = 0xa8,
    DRMD_FILE_OP_OFFSET = 0xb0,
    DRMD_FILE_PRIVATE_DATA_OFFSET = 0xc8,
    DRMD_INODE_MAPPING_OFFSET = 0x30,
    DRMD_INODE_RDEV_OFFSET = 0x4c,
    DRMD_INODE_CDEV_OFFSET = 0x238,
    DRMD_FMODE_READ = 0x1,
    DRMD_FMODE_WRITE = 0x2,
    DRMD_IOCTL_VERSION = 0xc0406400u,
};

typedef int (*drmd_fops_open_fn)(void *, void *);
typedef int (*drmd_fops_release_fn)(void *, void *);
typedef long (*drmd_fops_ioctl_fn)(void *, unsigned int, unsigned long);
typedef int (*drmd_fops_mmap_fn)(void *, void *);

typedef struct drmd_owner_context {
    unsigned long old_gs;
    kb_module_t *previous_owner;
    int active;
} drmd_owner_context_t;

typedef struct drmd_handle {
    int active;
    uint32_t refs;
    uint64_t id;
    int notify_fd;
    uint32_t flags;
    uint64_t dev;
    void *cdev;
    void *fops;
    kb_file_ops_view_t fops_view;
    kb_module_t *owner;
    uint8_t inode[DRMD_FAKE_INODE_BYTES];
    uint8_t mapping[DRMD_FAKE_MAPPING_BYTES];
    uint8_t file[DRMD_FAKE_FILE_BYTES];
} drmd_handle_t;

enum {
    DRMD_TRANSFER_LEASE_MAX = 32,
    DRMD_PRIME_LEASE_MAX = 32,
};

typedef struct drmd_transfer_lease {
    int active;
    int lease_fd;
    uint64_t handle;
} drmd_transfer_lease_t;

typedef struct drmd_prime_lease {
    int active;
    int lease_fd;
    uint64_t token;
} drmd_prime_lease_t;

typedef struct drmd_drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
} drmd_drm_version_t;

static drmd_handle_t handles[DRMD_HANDLE_MAX];
static drmd_transfer_lease_t transfer_leases[DRMD_TRANSFER_LEASE_MAX];
static drmd_prime_lease_t prime_leases[DRMD_PRIME_LEASE_MAX];
static uint64_t next_handle = 1;

static unsigned active_handle_count(void)
{
    unsigned count = 0;
    for (size_t i = 0; i < DRMD_HANDLE_MAX; i++) {
        count += handles[i].active ? 1u : 0u;
    }
    return count;
}

static void write_ptr(void *base, size_t offset, const void *value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void write_u32(void *base, size_t offset, uint32_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void write_u64(void *base, size_t offset, uint64_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static int enter_owner(kb_module_t *owner, drmd_owner_context_t *context)
{
    memset(context, 0, sizeof(*context));
    if (owner == NULL) {
        return 0;
    }
    context->previous_owner = kb_loader_active_module();
    if (kb_loader_enter_module_context(owner, &context->old_gs) != KB_OK) {
        return 0;
    }
    kb_loader_set_active_module(owner);
    context->active = 1;
    return 1;
}

static void leave_owner(const drmd_owner_context_t *context)
{
    if (context != NULL && context->active) {
        kb_loader_leave_module_context(context->old_gs);
        kb_loader_set_active_module(context->previous_owner);
    }
}

static drmd_handle_t *find_handle(uint64_t id)
{
    for (size_t i = 0; i < DRMD_HANDLE_MAX; i++) {
        if (handles[i].active && handles[i].id == id) {
            return &handles[i];
        }
    }
    return NULL;
}

static drmd_handle_t *alloc_handle(void)
{
    for (size_t i = 0; i < DRMD_HANDLE_MAX; i++) {
        if (!handles[i].active) {
            memset(&handles[i], 0, sizeof(handles[i]));
            handles[i].active = 1;
            handles[i].refs = 1;
            handles[i].id = next_handle++;
            if (next_handle == 0) {
                next_handle = 1;
            }
            return &handles[i];
        }
    }
    return NULL;
}

static void prepare_file(drmd_handle_t *handle)
{
    memset(handle->inode, 0, sizeof(handle->inode));
    memset(handle->mapping, 0, sizeof(handle->mapping));
    memset(handle->file, 0, sizeof(handle->file));
    write_ptr(handle->inode, DRMD_INODE_MAPPING_OFFSET, handle->mapping);
    write_u64(handle->inode, DRMD_INODE_RDEV_OFFSET, handle->dev);
    write_ptr(handle->inode, DRMD_INODE_CDEV_OFFSET, handle->cdev);
    write_ptr(handle->file, DRMD_FILE_INODE_OFFSET, handle->inode);
    write_ptr(handle->file, DRMD_FILE_OP_OFFSET, handle->fops);
    write_u32(handle->file, DRMD_FILE_MODE_OFFSET, DRMD_FMODE_READ | DRMD_FMODE_WRITE);
    write_u32(handle->file, DRMD_FILE_FLAGS_OFFSET, handle->flags);
    write_ptr(handle->file, DRMD_FILE_PRIVATE_DATA_OFFSET, NULL);
}

int drmd_drm_island_init(struct drmd_drm_island *island, const struct drmd_boot_config *cfg)
{
    if (island == NULL || cfg == NULL || cfg->device_fd < 16 ||
        cfg->module_count != DRMD_MAX_MODULES) {
        return -22;
    }
    memset(island, 0, sizeof(*island));
    (void)setenv("KOBOX_DEVICE_BACKEND", "pachaos", 1);
    (void)setenv("KOBOX_PCI_LAYOUT", "arch68", 1);
    (void)setenv("KOBOX_VIRTIO_NO_INDIRECT", "1", 1);
    (void)setenv("KOBOX_VIRTIO_NO_EVENT_IDX", "1", 1);

    kb_device_backend_t *backend = NULL;
    kb_status_t status = kb_pachaos_capsule_device_create(cfg->device_fd, &backend);
    if (status != KB_OK || backend == NULL) {
        island->load_status = status;
        return -5;
    }
    island->device_backend = backend;
    kb_shim_set_device_backend(backend);
    if (kb_kvm_prepare_dma_arena(backend) != 0) {
        return -5;
    }
    const kb_platform_desc_t platform_desc = {
        .name = "drmd-virtio-gpu-island",
        .device_backend = backend,
        .interfaces = NULL,
        .interface_count = 0,
    };
    kb_platform_t *platform = NULL;
    if (kb_platform_create(&platform_desc, &platform) != KB_OK) {
        return -5;
    }

    for (uint32_t i = 0; i < DRMD_MAX_MODULES; i++) {
        const struct drmd_module_config *module = &cfg->modules[i];
        if (module->image_va == 0 || module->image_size == 0 || module->name[0] == '\0') {
            return -22;
        }
        const kb_module_image_t image = {
            .data = (const void *)(uintptr_t)module->image_va,
            .size = (size_t)module->image_size,
            .name = module->name,
        };
        printf("[drmd] module open name=%s bytes=%llu\n",
            module->name,
            (unsigned long long)module->image_size);
        status = kb_module_open_image(&image, backend, (kb_module_t **)&island->modules[i]);
        if (status != KB_OK || island->modules[i] == NULL) {
            island->load_status = status;
            return -5;
        }
        island->loaded_module_count++;
        int init_result = 0;
        status = kb_module_call_init((kb_module_t *)island->modules[i], &init_result);
        if (status != KB_OK && status != KB_ERR_NOT_FOUND) {
            island->load_status = status;
            return -5;
        }
        if (status == KB_OK && init_result != 0) {
            island->load_status = init_result;
            return init_result;
        }
        printf("[drmd] module ready name=%s init=%d\n", module->name, init_result);
    }

    for (unsigned i = 0; i < 64; i++) {
        kb_run_deferred_work();
        (void)kb_handle_any_irq(0);
        if (kb_linux_kernel_find_active_cdev(
                kb_linux_kernel_encode_dev(DRMD_DRM_MAJOR, DRMD_DRM_CARD0_MINOR)) != NULL) {
            island->ready = 1;
            break;
        }
    }
    if (!island->ready) {
        return -19;
    }
    return drmd_kms_init(island);
}

int drmd_drm_island_open(
    struct drmd_drm_island *island,
    const drmd_open_request_t *request,
    int notify_fd,
    uint64_t *out_handle)
{
    if (island == NULL || request == NULL || out_handle == NULL ||
        !island->ready || request->card_index != 0 || notify_fd < 16) {
        return -19;
    }
    const uint64_t dev = kb_linux_kernel_encode_dev(DRMD_DRM_MAJOR, DRMD_DRM_CARD0_MINOR);
    const kb_cdev_record_t *record = kb_linux_kernel_find_active_cdev(dev);
    if (record == NULL || !record->has_fops_view || record->fops_view.open == NULL) {
        return -19;
    }
    drmd_handle_t *handle = alloc_handle();
    if (handle == NULL) {
        return -24;
    }
    handle->flags = (uint32_t)request->flags;
    handle->notify_fd = notify_fd;
    handle->dev = dev;
    handle->cdev = record->cdev;
    handle->fops = record->fops;
    handle->fops_view = record->fops_view;
    handle->owner = record->owner_module;
    prepare_file(handle);

    drmd_owner_context_t context;
    kb_module_t *owner = kb_module_find_owner_for_address(handle->fops_view.open);
    const int entered = enter_owner(owner != NULL ? owner : handle->owner, &context);
    const int status = ((drmd_fops_open_fn)handle->fops_view.open)(handle->inode, handle->file);
    if (entered) {
        leave_owner(&context);
    }
    if (status != 0) {
        memset(handle, 0, sizeof(*handle));
        return status;
    }
    drmd_kms_handle_open(handle->id);
    *out_handle = handle->id;
    return 0;
}

int drmd_drm_island_close(struct drmd_drm_island *island, uint64_t id)
{
    (void)island;
    drmd_handle_t *handle = find_handle(id);
    if (handle == NULL) {
        return -9;
    }
    if (handle->refs > 1) {
        handle->refs--;
        return 0;
    }
    if (handle->notify_fd >= 16) (void)pacha_fd_close(handle->notify_fd);
    drmd_kms_handle_close(island, id);
    int status = 0;
    if (handle->fops_view.release != NULL) {
        drmd_owner_context_t context;
        kb_module_t *owner = kb_module_find_owner_for_address(handle->fops_view.release);
        const int entered = enter_owner(owner != NULL ? owner : handle->owner, &context);
        status = ((drmd_fops_release_fn)handle->fops_view.release)(handle->inode, handle->file);
        if (entered) {
            leave_owner(&context);
        }
    }
    memset(handle, 0, sizeof(*handle));
    drmd_kms_state_counts_t counts;
    drmd_kms_get_state_counts(&counts);
    printf("[drmd] close handle=%llu status=%d state handles=%u fb=%u dumb=%u eventq=%u events=%u master=%llu\n",
        (unsigned long long)id,
        status,
        active_handle_count(),
        counts.fb,
        counts.dumb,
        counts.event_queues,
        counts.events,
        (unsigned long long)counts.master_handle);
    return status;
}

int drmd_drm_island_dup(struct drmd_drm_island *island, uint64_t id, uint64_t *out_handle)
{
    (void)island;
    drmd_handle_t *handle = find_handle(id);
    if (handle == NULL || out_handle == NULL || handle->refs == UINT32_MAX) {
        return -9;
    }
    handle->refs++;
    *out_handle = id;
    return 0;
}

int drmd_drm_island_transfer_dup(
    struct drmd_drm_island *island,
    uint64_t id,
    int lease_fd,
    uint64_t *out_handle)
{
    (void)island;
    drmd_handle_t *handle = find_handle(id);
    if (handle == NULL || out_handle == NULL || lease_fd < 16 ||
        handle->refs == UINT32_MAX) return -9;
    drmd_transfer_lease_t *lease = NULL;
    for (size_t i = 0; i < DRMD_TRANSFER_LEASE_MAX; ++i) {
        if (!transfer_leases[i].active) {
            lease = &transfer_leases[i];
            break;
        }
    }
    if (lease == NULL) return -24;
    memset(lease, 0, sizeof(*lease));
    lease->active = 1;
    lease->lease_fd = lease_fd;
    lease->handle = id;
    handle->refs++;
    *out_handle = id;
    return 0;
}

static uint32_t drmd_transfer_lease_count(uint64_t handle)
{
    uint32_t count = 0;
    for (size_t i = 0; i < DRMD_TRANSFER_LEASE_MAX; ++i)
        if (transfer_leases[i].active && transfer_leases[i].handle == handle) count++;
    return count;
}

int drmd_drm_island_ioctl(struct drmd_drm_island *island, drmd_ioctl_request_t *request)
{
    (void)island;
    if (request == NULL || request->data_size > DRMD_IOCTL_DATA_BYTES) {
        return -22;
    }
    drmd_handle_t *handle = find_handle(request->handle);
    if (handle == NULL || handle->fops_view.unlocked_ioctl == NULL) {
        return -9;
    }

    int kms_handled = 0;
    const int kms_status = drmd_kms_ioctl(island, request, &kms_handled);
    if (kms_handled) {
        return kms_status;
    }

    void *argument = request->data;
    drmd_drm_version_t version;
    char name[DRMD_VERSION_NAME_BYTES];
    char date[DRMD_VERSION_DATE_BYTES];
    char desc[DRMD_VERSION_DESC_BYTES];
    drmd_version_wire_t *wire = NULL;
    if ((uint32_t)request->request == DRMD_IOCTL_VERSION) {
        if (request->data_size < sizeof(drmd_version_wire_t)) {
            return -22;
        }
        wire = (drmd_version_wire_t *)request->data;
        memset(&version, 0, sizeof(version));
        memset(name, 0, sizeof(name));
        memset(date, 0, sizeof(date));
        memset(desc, 0, sizeof(desc));
        version.name_len = wire->name_capacity < sizeof(name) ? wire->name_capacity : sizeof(name);
        version.date_len = wire->date_capacity < sizeof(date) ? wire->date_capacity : sizeof(date);
        version.desc_len = wire->desc_capacity < sizeof(desc) ? wire->desc_capacity : sizeof(desc);
        version.name = name;
        version.date = date;
        version.desc = desc;
        argument = &version;
    }

    drmd_owner_context_t context;
    kb_module_t *owner = kb_module_find_owner_for_address(handle->fops_view.unlocked_ioctl);
    const int entered = enter_owner(owner != NULL ? owner : handle->owner, &context);
    const long status = ((drmd_fops_ioctl_fn)handle->fops_view.unlocked_ioctl)(
        handle->file,
        (unsigned int)request->request,
        (unsigned long)(uintptr_t)argument);
    if (entered) {
        leave_owner(&context);
    }
    if (status != 0) {
        return (int)status;
    }
    if (wire != NULL) {
        wire->major = version.version_major;
        wire->minor = version.version_minor;
        wire->patchlevel = version.version_patchlevel;
        wire->name_length = version.name_len;
        wire->date_length = version.date_len;
        wire->desc_length = version.desc_len;
        memcpy(wire->name, name, sizeof(wire->name));
        memcpy(wire->date, date, sizeof(wire->date));
        memcpy(wire->desc, desc, sizeof(wire->desc));
    }
    return 0;
}

int drmd_drm_island_mmap(
    struct drmd_drm_island *island,
    const drmd_mmap_request_t *request,
    int *out_vmo_fd)
{
    if (request == NULL || find_handle(request->handle) == NULL) {
        return -9;
    }
    return drmd_kms_mmap(island, request, out_vmo_fd);
}

int drmd_drm_island_read(
    struct drmd_drm_island *island,
    drmd_read_request_t *request,
    uint64_t *out_size)
{
    (void)island;
    if (request == NULL || out_size == NULL || find_handle(request->handle) == NULL ||
        request->capacity > sizeof(request->data)) {
        return request != NULL && request->capacity > sizeof(request->data) ? -22 : -9;
    }
    request->data_size = 0;
    const int status = drmd_kms_read(
        request->handle, request->data, request->capacity, &request->data_size);
    *out_size = status == 0 ? request->data_size : 0;
    if (status == 0) drmd_drm_island_notify_readable(island);
    return status;
}

size_t drmd_drm_island_collect_wait_sources(int *out_fds, size_t capacity)
{
    if (out_fds == NULL || capacity == 0) return 0;
    size_t count = 0;
    for (size_t i = 0; i < DRMD_HANDLE_MAX && count < capacity; i++) {
        const drmd_handle_t *handle = &handles[i];
        if (!handle->active || handle->notify_fd < 16) continue;
        out_fds[count++] = handle->notify_fd;
    }
    for (size_t i = 0; i < DRMD_TRANSFER_LEASE_MAX && count < capacity; ++i) {
        if (!transfer_leases[i].active || transfer_leases[i].lease_fd < 16) continue;
        out_fds[count++] = transfer_leases[i].lease_fd;
    }
    for (size_t i = 0; i < DRMD_PRIME_LEASE_MAX && count < capacity; ++i) {
        if (!prime_leases[i].active || prime_leases[i].lease_fd < 16) continue;
        out_fds[count++] = prime_leases[i].lease_fd;
    }
    return count;
}

size_t drmd_drm_island_reap_hangups(struct drmd_drm_island *island)
{
    if (island == NULL) return 0;
    size_t reaped = 0;
    for (size_t i = 0; i < DRMD_HANDLE_MAX; i++) {
        drmd_handle_t *handle = &handles[i];
        if (!handle->active || handle->notify_fd < 16) continue;
        struct pacha_pollfd pollfd = {
            .fd = handle->notify_fd,
            .events = PACHA_FD_EVENT_HANGUP,
        };
        if (pacha_fd_poll(&pollfd, 1) <= 0 ||
            (pollfd.revents & PACHA_FD_EVENT_HANGUP) == 0) continue;

        const uint64_t orphan_id = handle->id;
        const uint32_t orphan_refs = handle->refs;
        const int notify_fd = handle->notify_fd;
        handle->notify_fd = -1;
        if (notify_fd >= 16) (void)pacha_fd_close(notify_fd);
        handle->refs = drmd_transfer_lease_count(orphan_id) + 1u;
        if (handle->refs == 1u) drmd_kms_handle_orphan(orphan_id);
        const int close_status = drmd_drm_island_close(island, orphan_id);
        printf("[drmd] orphan reap handle=%llu refs=%u status=%d\n",
            (unsigned long long)orphan_id, orphan_refs, close_status);
        reaped++;
    }
    for (size_t i = 0; i < DRMD_TRANSFER_LEASE_MAX; ++i) {
        drmd_transfer_lease_t *lease = &transfer_leases[i];
        if (!lease->active || lease->lease_fd < 16) continue;
        struct pacha_pollfd pollfd = {
            .fd = lease->lease_fd,
            .events = PACHA_FD_EVENT_HANGUP,
        };
        if (pacha_fd_poll(&pollfd, 1) <= 0 ||
            (pollfd.revents & PACHA_FD_EVENT_HANGUP) == 0) continue;
        const uint64_t handle = lease->handle;
        (void)pacha_fd_close(lease->lease_fd);
        memset(lease, 0, sizeof(*lease));
        drmd_handle_t *resource = find_handle(handle);
        if (resource != NULL && resource->notify_fd < 16 &&
            drmd_transfer_lease_count(handle) == 0)
            drmd_kms_handle_orphan(handle);
        (void)drmd_drm_island_close(island, handle);
        reaped++;
    }
    for (size_t i = 0; i < DRMD_PRIME_LEASE_MAX; ++i) {
        drmd_prime_lease_t *lease = &prime_leases[i];
        if (!lease->active || lease->lease_fd < 16) continue;
        struct pacha_pollfd pollfd = {
            .fd = lease->lease_fd,
            .events = PACHA_FD_EVENT_HANGUP,
        };
        if (pacha_fd_poll(&pollfd, 1) <= 0 ||
            (pollfd.revents & PACHA_FD_EVENT_HANGUP) == 0) continue;
        const uint64_t token = lease->token;
        (void)pacha_fd_close(lease->lease_fd);
        memset(lease, 0, sizeof(*lease));
        (void)drmd_drm_island_prime_release(island, token);
        reaped++;
    }
    return reaped;
}

void drmd_drm_island_notify_readable(struct drmd_drm_island *island)
{
    (void)island;
    for (size_t i = 0; i < DRMD_HANDLE_MAX; i++) {
        drmd_handle_t *handle = &handles[i];
        if (!handle->active || handle->notify_fd < 16) continue;
        for (;;) {
            struct pacha_ipc_msg message;
            memset(&message, 0, sizeof(message));
            uint64_t event_size = 0;
            const int peek_status = drmd_kms_peek_event(
                handle->id, &message.word0, 4u * sizeof(message.word0), &event_size);
            if (peek_status != 0) break;
            const int send_status = event_size == 4u * sizeof(message.word0) ?
                pacha_ipc_send(handle->notify_fd, &message) : -22;
            if (send_status != 0) break;
            if (drmd_kms_consume_event(handle->id) != 0) break;
        }
    }
}

int drmd_drm_island_poll(
    struct drmd_drm_island *island,
    const drmd_handle_request_t *request,
    uint64_t *out_events)
{
    (void)island;
    if (request == NULL || out_events == NULL || find_handle(request->handle) == NULL) {
        return -9;
    }
    uint32_t revents = 0;
    const int status = drmd_kms_poll(request->handle, (uint32_t)request->arg0, &revents);
    *out_events = revents;
    return status;
}

int drmd_drm_island_prime_export(
    struct drmd_drm_island *island,
    const drmd_prime_export_request_t *request,
    uint64_t *out_token,
    int *out_vmo_fd,
    uint64_t *out_rights)
{
    if (island == NULL || request == NULL || find_handle(request->handle) == NULL) return -9;
    return drmd_kms_prime_export(
        request->handle, request->gem_handle, request->flags,
        out_token, out_vmo_fd, out_rights);
}

int drmd_drm_island_prime_import(
    struct drmd_drm_island *island,
    const drmd_prime_import_request_t *request,
    int import_vmo_fd,
    uint64_t *out_gem_handle)
{
    if (island == NULL || request == NULL || request->reserved0 != 0 ||
        find_handle(request->handle) == NULL) return -9;
    uint32_t gem_handle = 0;
    const int token_import = request->token != 0 && request->size == 0 && import_vmo_fd < 16;
    const int vmo_import = request->token == 0 && request->size != 0 && import_vmo_fd >= 16;
    if (!token_import && !vmo_import) return -22;
    const int status = token_import ?
        drmd_kms_prime_import(
            request->handle, request->token, request->flags, &gem_handle) :
        drmd_kms_prime_import_vmo(
            request->handle, import_vmo_fd, request->size, request->flags, &gem_handle);
    if (status == 0 && out_gem_handle != NULL) *out_gem_handle = gem_handle;
    return status;
}

int drmd_drm_island_prime_acquire(
    struct drmd_drm_island *island,
    uint64_t token,
    int lease_fd)
{
    if (island == NULL || !island->ready) return -19;
    drmd_prime_lease_t *lease = NULL;
    if (lease_fd >= 16) {
        for (size_t i = 0; i < DRMD_PRIME_LEASE_MAX; ++i) {
            if (!prime_leases[i].active) {
                lease = &prime_leases[i];
                break;
            }
        }
        if (lease == NULL) return -24;
    }
    const int status = drmd_kms_prime_acquire(token);
    if (status != 0) return status;
    if (lease != NULL) {
        memset(lease, 0, sizeof(*lease));
        lease->active = 1;
        lease->lease_fd = lease_fd;
        lease->token = token;
    }
    return 0;
}

int drmd_drm_island_prime_release(struct drmd_drm_island *island, uint64_t token)
{
    return island != NULL && island->ready ? drmd_kms_prime_release(token) : -19;
}

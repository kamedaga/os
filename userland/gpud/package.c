/* SPDX-License-Identifier: MIT */
#include "package.h"

#include "../kobox2_adapter/device_authority.h"
#include <filed_client/module_image.h>
#include <kobox2/closure.h>
#include <kobox2/pci_function_layout.h>
#include <kobox2/resource_grant.h>
#include <kobox2/sha256.h>
#include <pacha/capsule.h>
#include <pacha/ipc.h>
#include <stdlib.h>
#include <string.h>

static const char *const artifact_paths[GPUD_PACKAGE_ARTIFACTS] = {
    "/srv/kobox2/core.so",
    "/srv/kobox2/modules/i2c-core.ko",
    "/srv/kobox2/modules/drm_panel_orientation_quirks.ko",
    "/srv/kobox2/modules/drm.ko",
    "/srv/kobox2/modules/drm_shmem_helper.ko",
    "/srv/kobox2/modules/virtio_ring.ko",
    "/srv/kobox2/modules/virtio.ko",
    "/srv/kobox2/modules/virtio_pci_modern_dev.ko",
    "/srv/kobox2/modules/virtio_pci.ko",
    "/srv/kobox2/modules/virtio_dma_buf.ko",
    "/srv/kobox2/modules/drm_kms_helper.ko",
    "/srv/kobox2/modules/virtio-gpu.ko",
};

static const char *const artifact_names[GPUD_PACKAGE_ARTIFACTS] = {
    "core", "i2c_core", "drm_panel_orientation_quirks", "drm",
    "drm_shmem_helper", "virtio_ring", "virtio", "virtio_pci_modern_dev",
    "virtio_pci", "virtio_dma_buf", "drm_kms_helper", "virtio_gpu",
};

static const kb2_closure_manifest_dependency_t dependencies[] = {
    {2, 1},  {3, 1},  {4, 1},  {5, 1},  {6, 1},  {7, 1},  {8, 1},   {9, 1},
    {10, 1}, {11, 1}, {12, 1}, {4, 2},  {4, 3},  {5, 4},  {7, 6},   {9, 6},
    {9, 7},  {9, 8},  {11, 4}, {12, 4}, {12, 5}, {12, 6}, {12, 7}, {12, 10},
    {12, 11},
};

static const uint8_t device_schema[32] = KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES;

static int make_blob(const void *bytes, size_t size, struct ph_bootstrap_item *item) {
    if (!bytes || !size || size > UINT64_MAX - 4095)
        return -22;
    uint64_t mapped = (size + 4095) & ~UINT64_C(4095);
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    int fd = pacha_vmo_create(mapped, rights, 0);
    if (fd < 16)
        return fd < 0 ? fd : -12;
    void *view = pacha_mmap(fd, mapped,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!view) {
        (void)pacha_fd_close(fd);
        return -5;
    }
    memcpy(view, bytes, size);
    if (size < mapped)
        memset((unsigned char *)view + size, 0, mapped - size);
    int error = pacha_munmap(view, mapped);
    if (error) {
        (void)pacha_fd_close(fd);
        return error;
    }
    *item = (struct ph_bootstrap_item){.size = size,
        .capability = {.fd = (uint64_t)fd, .rights = PH_BOOTSTRAP_BLOB_RIGHTS,
            .flags = PACHA_FD_FLAG_CLOEXEC}};
    return 0;
}

static int load_blob(int filed_fd, const char *path, const char *name,
    struct ph_bootstrap_item *item, uint8_t digest[32]) {
    struct filed_client_module_image image = {0};
    int error = filed_client_load_module_image(filed_fd, path, name, &image);
    if (!error) {
        kb2_sha256(image.data, image.size, digest);
        error = make_blob(image.data, image.size, item);
    }
    filed_client_release_module_image(&image);
    return error;
}

static int make_manifest(struct gpud_package *package) {
    kb2_closure_manifest_resource_t resource = {.slot_id = 1,
        .type = KB2_CLOSURE_RESOURCE_DEVICE, .minimum_count = 1, .maximum_count = 1,
        .required_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
        .maximum_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
        .flags = KB2_CLOSURE_RESOURCE_FLAG_REQUIRED | KB2_CLOSURE_RESOURCE_FLAG_SHARED};
    memcpy(resource.interface_schema_digest, device_schema, sizeof(device_schema));
    kb2_closure_manifest_binding_t bindings[GPUD_PACKAGE_ARTIFACTS];
    for (uint32_t i = 0; i < GPUD_PACKAGE_ARTIFACTS; ++i)
        bindings[i] = (kb2_closure_manifest_binding_t){.slot_id = 1, .node_id = i + 1};
    kb2_closure_manifest_source_t source = {.artifacts = package->artifacts,
        .artifact_count = GPUD_PACKAGE_ARTIFACTS, .dependencies = dependencies,
        .dependency_count = sizeof(dependencies) / sizeof(dependencies[0]),
        .resources = &resource, .resource_count = 1, .bindings = bindings,
        .binding_count = GPUD_PACKAGE_ARTIFACTS};
    return kb2_closure_manifest_encode(package->manifest, sizeof(package->manifest),
        &package->manifest_size, &source) == KB2_PROTOCOL_OK ? 0 : -22;
}

int gpud_package_open(struct gpud_package *package, int filed_fd, int device_fd) {
    if (!package || package->device_fd || filed_fd < 16 || device_fd < 16)
        return -22;
    struct pacha_capsule_info info = {0};
    int error = pacha_capsule_query(device_fd, &info);
    const uint64_t device_rights = PH_DEVICE_FUNCTION_RIGHTS | PACHA_FD_RIGHT_TRANSFER;
    if (error || info.kind != PACHA_CAPSULE_KIND_DEVICE ||
        (info.rights & device_rights) != device_rights || !info.device ||
        !(info.flags & PACHA_CAPSULE_DMA_TRANSLATED) ||
        (info.flags & PACHA_CAPSULE_DMA_QUARANTINED))
        return error ? error : -13;
    package->device_fd = device_fd;
    package->device = (struct ph_device_authority){
        .object_id = 1, .native_device = info.device, .slot_id = 1, .node_id = 1};
    struct filed_client_module_image sandbox = {0};
    error = filed_client_load_module_image(
        filed_fd, "/srv/kobox2/sandbox.elf", "gpud-sandbox", &sandbox);
    if (error)
        goto fail;
    package->sandbox = sandbox.data;
    package->sandbox_size = sandbox.size;
    package->sandbox_storage = sandbox.data;
    for (size_t i = 0; i < GPUD_PACKAGE_ARTIFACTS; ++i) {
        package->artifacts[i] = (kb2_closure_manifest_artifact_t){.node_id = i + 1,
            .kind = i ? KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE :
                KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER,
            .flags = KB2_CLOSURE_ARTIFACT_FLAG_NATIVE_LINUX |
                (i == GPUD_PACKAGE_ARTIFACTS - 1 || i == 8 ?
                    KB2_CLOSURE_ARTIFACT_FLAG_ROOT : 0),
            .content_size = package->items[i + 2].size,
            .namespace_name = {.data = artifact_names[i], .length = strlen(artifact_names[i])}};
        error = load_blob(filed_fd, artifact_paths[i], artifact_names[i],
            &package->items[i + 2], package->artifacts[i].content_digest);
        if (error)
            goto fail;
        package->artifacts[i].content_size = package->items[i + 2].size;
    }
    error = make_manifest(package);
    if (!error) {
        kb2_sha256(package->manifest, package->manifest_size, package->identity.manifest_digest);
        error = make_blob(package->manifest, package->manifest_size, &package->items[0]);
    }
    if (error)
        goto fail;
    return 0;
fail:
    (void)gpud_package_close(package);
    return error;
}

int gpud_package_bind(struct gpud_package *package, uint64_t generation) {
    if (!package || !package->device_fd || !package->manifest_size || !generation ||
        package->items[1].capability.fd)
        return -22;
    package->identity.generation = generation;
    package->device.generation = generation;
    kb2_resource_grant_slot_source_t slot = {.slot_id = 1,
        .resource_type = KB2_CLOSURE_RESOURCE_DEVICE,
        .state = KB2_RESOURCE_GRANT_SLOT_PRESENT};
    memcpy(slot.interface_schema_digest, device_schema, sizeof(device_schema));
    kb2_resource_grant_object_source_t object = {.slot_id = 1, .object_id = 1,
        .granted_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS};
    kb2_resource_grant_handle_binding_t handle = {.object_id = 1,
        .role = KB2_PCI_FUNCTION_NATIVE_HANDLE_ROLE_DEVICE, .transfer_handle_index = 0};
    kb2_resource_grant_source_t source = {.generation = generation, .slots = &slot,
        .slot_count = 1, .objects = &object, .object_count = 1,
        .handle_bindings = &handle, .handle_binding_count = 1};
    memcpy(source.closure_manifest_digest, package->identity.manifest_digest, 32);
    if (kb2_resource_grant_encode(package->grant, sizeof(package->grant),
            &package->grant_size, &source) != KB2_PROTOCOL_OK)
        return -22;
    kb2_sha256(package->grant, package->grant_size, package->identity.grant_digest);
    int error = make_blob(package->grant, package->grant_size, &package->items[1]);
    if (error)
        return error;
    package->items[GPUD_PACKAGE_ITEMS - 1] = (struct ph_bootstrap_item){
        .capability = {.fd = (uint64_t)package->device_fd,
            .rights = PH_DEVICE_FUNCTION_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC}};
    return 0;
}

int gpud_package_unbind(struct gpud_package *package, uint64_t generation) {
    if (!package || !generation || package->identity.generation != generation ||
        package->device.generation != generation)
        return -22;
    uint64_t fd = package->items[1].capability.fd;
    if (fd >= 16) {
        int error = pacha_fd_close((int)fd);
        if (error)
            return error;
        package->items[1] = (struct ph_bootstrap_item){0};
    }
    package->identity.generation = 0;
    package->device.generation = 0;
    package->grant_size = 0;
    memset(package->identity.grant_digest, 0, sizeof(package->identity.grant_digest));
    return 0;
}

static void *gpud_package_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void gpud_package_deallocate(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

int gpud_package_configure_controller(
    const struct gpud_package *package, kb2_controller_t *controller) {
    if (!package || !controller || !package->manifest_size)
        return -22;
    kb2_closure_builder_t *builder = NULL;
    kb2_closure_t *closure = NULL;
    kb2_status_t status = kb2_closure_builder_create(
        gpud_package_allocate, gpud_package_deallocate, NULL,
        package->identity.manifest_digest, 32, &builder);
    if (status != KB2_STATUS_OK)
        return -12;
    for (size_t i = 0; i < GPUD_PACKAGE_ARTIFACTS && status == KB2_STATUS_OK; ++i) {
        const kb2_closure_manifest_artifact_t *artifact = &package->artifacts[i];
        status = kb2_closure_builder_add_artifact(builder, artifact->node_id,
            i ? KB2_ARTIFACT_RELOCATABLE_MODULE : KB2_ARTIFACT_SHARED_PROVIDER,
            artifact->content_digest, 32, artifact->namespace_name.data,
            artifact->namespace_name.length);
        if (status == KB2_STATUS_OK)
            status = kb2_closure_builder_set_native_lifecycle(builder, artifact->node_id);
    }
    for (size_t i = 0; i < sizeof(dependencies) / sizeof(dependencies[0]) &&
            status == KB2_STATUS_OK; ++i)
        status = kb2_closure_builder_add_dependency(builder,
            dependencies[i].consumer_node_id, dependencies[i].provider_node_id);
    if (status == KB2_STATUS_OK)
        status = kb2_closure_builder_mark_root(builder, 9);
    if (status == KB2_STATUS_OK)
        status = kb2_closure_builder_mark_root(builder, 12);
    if (status == KB2_STATUS_OK)
        status = kb2_closure_builder_add_resource(builder, 1, KB2_RESOURCE_DEVICE,
            device_schema, sizeof(device_schema), 1, 1,
            KB2_PCI_FUNCTION_REQUIRED_RIGHTS, KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
            KB2_RESOURCE_REQUIRED | KB2_RESOURCE_SHARED);
    for (uint32_t node = 1; node <= GPUD_PACKAGE_ARTIFACTS && status == KB2_STATUS_OK; ++node)
        status = kb2_closure_builder_bind_resource(builder, 1, node);
    if (status == KB2_STATUS_OK)
        status = kb2_closure_builder_seal(builder, &closure);
    if (status == KB2_STATUS_OK)
        status = kb2_controller_set_closure(controller, closure);
    kb2_closure_destroy(closure);
    kb2_closure_builder_destroy(builder);
    if (status != KB2_STATUS_OK)
        return -22;
    uint8_t digest[32];
    memset(digest, 0x41, sizeof(digest));
    const uint64_t limits[] = {256u << 20, 1, 2, 64};
    for (unsigned kind = KB2_DIGEST_PROFILE; kind <= KB2_DIGEST_CHANNEL_SET; ++kind)
        if (kb2_controller_set_digest(controller, kind, digest, sizeof(digest)) != KB2_STATUS_OK)
            return -22;
    for (unsigned kind = 0; kind <= KB2_LIMIT_OUTSTANDING_REQUEST_COUNT; ++kind)
        if (kb2_controller_set_limit(controller, kind, limits[kind]) != KB2_STATUS_OK)
            return -22;
    return 0;
}

int gpud_package_close(struct gpud_package *package) {
    if (!package)
        return -22;
    int first = 0;
    for (size_t i = 0; i < GPUD_PACKAGE_ITEMS - 1; ++i) {
        uint64_t fd = package->items[i].capability.fd;
        if (fd >= 16) {
            int error = pacha_fd_close((int)fd);
            if (error && !first)
                first = error;
            if (!error)
                package->items[i].capability.fd = 0;
        }
    }
    if (!first) {
        free(package->sandbox_storage);
        memset(package, 0, sizeof(*package));
    }
    return first;
}

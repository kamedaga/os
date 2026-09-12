/* SPDX-License-Identifier: MIT */
/* Apache controller stays here; the separately launched child links GPL core
 * support. This verifies the host boot path, not a published DRM service. */
#include "../userland/gpud/bootstrap.h"
#include "../userland/gpud/launch.h"
#include "../userland/gpud/lifecycle.h"
#include "../userland/gpud/process.h"
#include "../userland/seed0boot/src/bootfs_reader.h"
#include "native_gpud_core_common.h"
#include "native_kobox2_ipc_common.h"
#include <kobox2/closure.h>
#include <kobox2/closure_manifest.h>
#include <kobox2/resource_grant.h>
#include <kobox2/sha256.h>
#if GPUD_CORE_TEST_DEVICE
#include "../userland/gpud/drm_reply.h"
#include "../userland/gpud/drm_translate.h"
#include "../userland/gpud/gpu_channel.h"
#include "../userland/gpud/gpu_rpc.h"
#include "../userland/kobox2_adapter/gpu_query_message.h"
#include "../userland/kobox2_adapter/gpu_queue_message.h"
#include "../userland/seed0boot/src/bootstrap_abi.h"
#include <kobox2/gpu_session.h>
#include <kobox2/pci_function_layout.h>
#include <kobox2/virtqueue_x86_64.h>
#include <pacha/capsule.h>
#endif

struct core_package {
    const void *bytes[GPUD_CORE_TEST_BLOBS];
    size_t sizes[GPUD_CORE_TEST_BLOBS];
    uint8_t manifest[8192], grant[4096];
    kb2_closure_manifest_artifact_t artifacts[GPUD_CORE_TEST_ARTIFACTS];
    struct ph_package_identity identity;
    struct ph_bootstrap_item items[GPUD_CORE_TEST_BLOBS + GPUD_CORE_TEST_RESOURCES];
    struct ph_device_authority device;
    int device_fd;
};

/* The fixed package's actual native modinfo dependencies, plus the core
 * provider for every module. Both manifest and controller use this graph. */
static const kb2_closure_manifest_dependency_t dependencies[] = {
    {2, 1},
#if GPUD_CORE_TEST_DEVICE
    {3, 1},  {4, 1},  {5, 1},  {6, 1},  {7, 1},  {8, 1},  {9, 1},   {10, 1},
    {11, 1}, {12, 1}, {4, 2},  {4, 3},  {5, 4},  {7, 6},  {9, 6},   {9, 7},
    {9, 8},  {11, 4}, {12, 4}, {12, 5}, {12, 6}, {12, 7}, {12, 10}, {12, 11},
#endif
};

#if GPUD_CORE_TEST_DEVICE
static const uint8_t device_schema[32] = KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES;

static void select_device(struct core_package *package) {
    const struct seed0_init_descriptor_page *boot = seed0_bootstrap_descriptor();
    IPC_CHECK(boot && boot->device_count <= SEED0_INIT_MAX_DEVICE_DESCRIPTORS);
    for (uint64_t i = 0; i < boot->device_count; ++i) {
        const struct seed0_device_descriptor *candidate = &boot->devices[i];
        if (candidate->vendor_id != 0x1af4 || candidate->device_id != 0x1050)
            continue;
        IPC_CHECK(!package->device_fd && candidate->init_device_fd >= 16 &&
                  candidate->init_device_fd < PACHA_FD_TABLE_LIMIT);
        package->device_fd = candidate->init_device_fd;
    }
    IPC_CHECK(package->device_fd);
    struct pacha_capsule_info info = {0};
    IPC_CHECK(pacha_syscall3(
                  PACHA_CAPSULE_SYSCALL_QUERY, package->device_fd, (uintptr_t)&info, 11) == 11 &&
              info.kind == PACHA_CAPSULE_KIND_DEVICE);
    const uint64_t rights = PH_DEVICE_FUNCTION_RIGHTS | PACHA_FD_RIGHT_TRANSFER;
    IPC_CHECK((info.rights & rights) == rights && info.device &&
              (info.flags & PACHA_CAPSULE_DMA_TRANSLATED) &&
              !(info.flags & PACHA_CAPSULE_DMA_QUARANTINED));
    package->device = (struct ph_device_authority){
        .object_id = 1, .native_device = info.device, .slot_id = 1, .node_id = 1};
}
#endif

int strcmp(const char *left, const char *right) {
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

static void *allocate(void *context, size_t size) {
    (void)context;
    size = (size + IPC_TEST_PAGE - 1) & ~(IPC_TEST_PAGE - 1);
    long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP,
                                  0,
                                  0,
                                  size,
                                  IPC_TEST_RW,
                                  PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS,
                                  0);
    return address >= (long)IPC_TEST_PAGE ? (void *)(uintptr_t)address : NULL;
}

static void deallocate(void *context, void *address, size_t size) {
    (void)context;
    ipc_test_unmap(address, (size + IPC_TEST_PAGE - 1) & ~(IPC_TEST_PAGE - 1));
}

static void complete(kb2_controller_t *controller, kb2_action_type_t type, uint64_t resource_id) {
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    IPC_CHECK(action && kb2_action_type(action) == type);
    IPC_CHECK(kb2_controller_complete_action(controller,
                                             kb2_action_generation(action),
                                             kb2_action_token(action),
                                             KB2_STATUS_OK,
                                             resource_id,
                                             0) == KB2_STATUS_OK);
}

static void wait_channel(int fd, uint64_t deadline) {
    for (;;) {
        uint64_t now = ipc_test_now();
        IPC_CHECK(now < deadline);
        struct pacha_pollfd event = {.fd = fd,
                                     .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
        /* Native wait timeout is in milliseconds. Round up, then recheck
         * the absolute monotonic deadline after interruption/timeout. */
        uint64_t timeout = (deadline - now + 999999) / 1000000;
        long result = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY, (uintptr_t)&event, 1, timeout, 0);
        if (result == 1 && event.revents)
            return;
        IPC_CHECK(!event.revents && (result == 0 || result == PACHA_SYSCALL_ERR_NOT_READY));
    }
}

static void prepare_package(struct core_package *package) {
#if GPUD_CORE_TEST_DEVICE
    const char *paths[] = {"/srv/kobox2/core.so",
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
                           "/srv/kobox2/modules/virtio-gpu.ko"};
    const char *names[] = {"core",
                           "i2c_core",
                           "drm_panel_orientation_quirks",
                           "drm",
                           "drm_shmem_helper",
                           "virtio_ring",
                           "virtio",
                           "virtio_pci_modern_dev",
                           "virtio_pci",
                           "virtio_dma_buf",
                           "drm_kms_helper",
                           "virtio_gpu"};
#else
    const char *paths[] = {"/tests/package-core.so", "/tests/package-module.ko"};
    const char *names[] = {"core", "drm_panel_orientation_quirks"};
#endif
    for (size_t i = 0; i < GPUD_CORE_TEST_ARTIFACTS; ++i) {
        const unsigned char *bytes;
        uint32_t size;
        IPC_CHECK(!seed0_bootfs_open_file(paths[i], &bytes, &size));
        package->bytes[i + 2] = bytes;
        package->sizes[i + 2] = size;
        package->artifacts[i] = (kb2_closure_manifest_artifact_t){
            .node_id = i + 1,
            .kind =
                i ? KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE : KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER,
            .flags = KB2_CLOSURE_ARTIFACT_FLAG_NATIVE_LINUX |
                     (i == GPUD_CORE_TEST_ARTIFACTS - 1 || (GPUD_CORE_TEST_DEVICE && i == 8)
                          ? KB2_CLOSURE_ARTIFACT_FLAG_ROOT
                          : 0),
            .content_size = size,
            .namespace_name = {.data = names[i], .length = strlen(names[i])}};
        kb2_sha256(bytes, size, package->artifacts[i].content_digest);
    }
    kb2_closure_manifest_source_t source = {.artifacts = package->artifacts,
                                            .artifact_count = GPUD_CORE_TEST_ARTIFACTS,
                                            .dependencies = dependencies,
                                            .dependency_count =
                                                sizeof(dependencies) / sizeof(dependencies[0])};
#if GPUD_CORE_TEST_DEVICE
    /* The core owns native machine ports; every module shares this one trust
     * domain and is explicitly bound, not authorized by dependency edges. */
    kb2_closure_manifest_resource_t resource = {.slot_id = 1,
                                                .type = KB2_CLOSURE_RESOURCE_DEVICE,
                                                .minimum_count = 1,
                                                .maximum_count = 1,
                                                .required_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
                                                .maximum_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
                                                .flags = KB2_CLOSURE_RESOURCE_FLAG_REQUIRED |
                                                         KB2_CLOSURE_RESOURCE_FLAG_SHARED};
    memcpy(resource.interface_schema_digest, device_schema, 32);
    kb2_closure_manifest_binding_t bindings[GPUD_CORE_TEST_ARTIFACTS];
    for (size_t i = 0; i < GPUD_CORE_TEST_ARTIFACTS; ++i)
        bindings[i] = (kb2_closure_manifest_binding_t){.slot_id = 1, .node_id = i + 1};
    source.resources = &resource;
    source.resource_count = 1;
    source.bindings = bindings;
    source.binding_count = GPUD_CORE_TEST_ARTIFACTS;
    select_device(package);
#endif
    IPC_CHECK(kb2_closure_manifest_encode(
                  package->manifest, sizeof(package->manifest), &package->sizes[0], &source) ==
              KB2_PROTOCOL_OK);
    package->bytes[0] = package->manifest;
    kb2_sha256(package->manifest, package->sizes[0], package->identity.manifest_digest);
}

static kb2_controller_t *create_controller(const struct core_package *package) {
    kb2_controller_t *controller;
    kb2_closure_builder_t *builder;
    kb2_closure_t *closure;
    IPC_CHECK(kb2_controller_create(allocate, deallocate, NULL, &controller) == KB2_STATUS_OK);
    IPC_CHECK(kb2_closure_builder_create(
                  allocate, deallocate, NULL, package->identity.manifest_digest, 32, &builder) ==
              KB2_STATUS_OK);
    for (size_t i = 0; i < GPUD_CORE_TEST_ARTIFACTS; ++i) {
        const kb2_closure_manifest_artifact_t *artifact = &package->artifacts[i];
        IPC_CHECK(kb2_closure_builder_add_artifact(
                      builder,
                      artifact->node_id,
                      i ? KB2_ARTIFACT_RELOCATABLE_MODULE : KB2_ARTIFACT_SHARED_PROVIDER,
                      artifact->content_digest,
                      32,
                      artifact->namespace_name.data,
                      artifact->namespace_name.length) == KB2_STATUS_OK);
        IPC_CHECK(kb2_closure_builder_set_native_lifecycle(builder, artifact->node_id) ==
                  KB2_STATUS_OK);
    }
    for (size_t i = 0; i < sizeof(dependencies) / sizeof(dependencies[0]); ++i)
        IPC_CHECK(kb2_closure_builder_add_dependency(builder,
                                                     dependencies[i].consumer_node_id,
                                                     dependencies[i].provider_node_id) ==
                  KB2_STATUS_OK);
    IPC_CHECK(kb2_closure_builder_mark_root(builder, GPUD_CORE_TEST_ARTIFACTS) == KB2_STATUS_OK);
#if GPUD_CORE_TEST_DEVICE
    IPC_CHECK(kb2_closure_builder_mark_root(builder, 9) == KB2_STATUS_OK);
    IPC_CHECK(kb2_closure_builder_add_resource(builder,
                                               1,
                                               KB2_RESOURCE_DEVICE,
                                               device_schema,
                                               32,
                                               1,
                                               1,
                                               KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
                                               KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
                                               KB2_RESOURCE_REQUIRED | KB2_RESOURCE_SHARED) ==
              KB2_STATUS_OK);
    for (uint32_t node = 1; node <= GPUD_CORE_TEST_ARTIFACTS; ++node)
        IPC_CHECK(kb2_closure_builder_bind_resource(builder, 1, node) == KB2_STATUS_OK);
#endif
    IPC_CHECK(kb2_closure_builder_seal(builder, &closure) == KB2_STATUS_OK);
    IPC_CHECK(kb2_controller_set_closure(controller, closure) == KB2_STATUS_OK);
    kb2_closure_destroy(closure);
    kb2_closure_builder_destroy(builder);
    /* Host fixture configuration; no capability/channel digest is claimed
     * to describe a real GPU resource inventory. No READY is synthesized. */
    uint8_t digest[32];
    memset(digest, 0x41, sizeof(digest));
    for (unsigned int kind = KB2_DIGEST_PROFILE; kind <= KB2_DIGEST_CHANNEL_SET; ++kind)
        IPC_CHECK(kb2_controller_set_digest(controller, kind, digest, sizeof(digest)) ==
                  KB2_STATUS_OK);
    const uint64_t limits[] = {256u << 20, 1, 2, 64};
    for (unsigned int kind = 0; kind <= KB2_LIMIT_OUTSTANDING_REQUEST_COUNT; ++kind)
        IPC_CHECK(kb2_controller_set_limit(controller, kind, limits[kind]) == KB2_STATUS_OK);
    return controller;
}

static void publish_package(struct core_package *package, uint64_t generation) {
    package->identity.generation = generation;
    kb2_resource_grant_source_t grant = {.generation = generation};
#if GPUD_CORE_TEST_DEVICE
    package->device.generation = generation;
    kb2_resource_grant_slot_source_t slot = {.slot_id = 1,
                                             .resource_type = KB2_CLOSURE_RESOURCE_DEVICE,
                                             .state = KB2_RESOURCE_GRANT_SLOT_PRESENT};
    memcpy(slot.interface_schema_digest, device_schema, 32);
    kb2_resource_grant_object_source_t object = {.slot_id = 1,
                                                 .object_id = package->device.object_id,
                                                 .granted_rights =
                                                     KB2_PCI_FUNCTION_REQUIRED_RIGHTS};
    kb2_resource_grant_handle_binding_t handle = {.object_id = object.object_id,
                                                  .role =
                                                      KB2_PCI_FUNCTION_NATIVE_HANDLE_ROLE_DEVICE,
                                                  .transfer_handle_index = 0};
    grant.slots = &slot;
    grant.slot_count = 1;
    grant.objects = &object;
    grant.object_count = 1;
    grant.handle_bindings = &handle;
    grant.handle_binding_count = 1;
    package->items[GPUD_CORE_TEST_BLOBS] =
        (struct ph_bootstrap_item){.capability = {.fd = package->device_fd,
                                                  .rights = PH_DEVICE_FUNCTION_RIGHTS,
                                                  .flags = PACHA_FD_FLAG_CLOEXEC}};
#endif
    memcpy(grant.closure_manifest_digest, package->identity.manifest_digest, 32);
    IPC_CHECK(kb2_resource_grant_encode(
                  package->grant, sizeof(package->grant), &package->sizes[1], &grant) ==
              KB2_PROTOCOL_OK);
    package->bytes[1] = package->grant;
    kb2_sha256(package->grant, package->sizes[1], package->identity.grant_digest);
    for (size_t i = 0; i < GPUD_CORE_TEST_BLOBS; ++i) {
        size_t size = (package->sizes[i] + IPC_TEST_PAGE - 1) & ~(IPC_TEST_PAGE - 1);
        long fd = pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, size, IPC_TEST_VMO_RIGHTS, 0);
        IPC_CHECK(fd >= 16 && fd < PACHA_FD_TABLE_LIMIT);
        void *mapping = ipc_test_map(fd, size, IPC_TEST_RW);
        memcpy(mapping, package->bytes[i], package->sizes[i]);
        ipc_test_unmap(mapping, size);
        package->items[i] = (struct ph_bootstrap_item){
            .size = package->sizes[i],
            .capability = {
                .fd = fd, .rights = PH_BOOTSTRAP_BLOB_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC}};
    }
}

#if GPUD_CORE_TEST_DEVICE
static uint32_t query_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
           (uint32_t)bytes[3] << 24;
}

static void query_bound_round(struct ph_ipc *ipc,
                              const struct gpud_drm_binding *admitted,
                              uint64_t correlation,
                              unsigned int test_case,
                              int closed,
                              struct gpud_gpu_channel *channel,
                              unsigned char *mapping) {
    struct gpud_drm_binding binding = *admitted;
    uint64_t session_id = binding.session_id;
    IPC_CHECK(binding.generation == ipc->generation);
    drmd_ioctl_request_t request = {.handle = binding.frontend_handle};
    if (test_case == 1 || test_case >= 5) {
        drmd_version_wire_t version = {0};
        if (test_case == 1) {
            version.name_capacity = sizeof(version.name);
            version.date_capacity = sizeof(version.date);
            version.desc_capacity = sizeof(version.desc);
        } else if (test_case == 5)
            version.name_capacity = 1;
        request.request = UINT64_C(0xc0406400);
        request.arg_size = 64;
        request.data_size = sizeof(version);
        memcpy(request.data, &version, sizeof(version));
    } else {
        uint64_t arguments[2] = {test_case == 3 ? UINT64_MAX : 5, 0};
        request.request = test_case == 4 ? UINT64_C(0x4010640d) : UINT64_C(0xc010640c);
        request.arg_size = request.data_size = sizeof(arguments);
        memcpy(request.data, arguments, sizeof(arguments));
    }
    struct gpud_drm_translation encoded;
    IPC_CHECK(!gpud_drm_ioctl_encode(&encoded, &binding, &request, PH_GPU_QUERY_OUTPUT_REGION));
    kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
                                                .opcode = KB2_GPU_OPCODE_COMMAND,
                                                .generation = ipc->generation,
                                                .correlation_id = correlation,
                                                .payload_length = encoded.command_size};
    size_t request_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + encoded.command_size;
    unsigned char *request_bytes = mapping + GPUD_GPU_REQUEST_OFFSET;
    IPC_CHECK(!kb2_protocol_message_envelope_encode(request_bytes, request_size, &envelope));
    memcpy(
        request_bytes + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, encoded.command, encoded.command_size);
    kb2_vq_segment_t segments[] = {{GPUD_GPU_REQUEST_OFFSET, request_size, 0, 0},
                                   {GPUD_GPU_REPLY_OFFSET, PH_GPU_QUERY_PAGE, 1, 1}};
    kb2_vq_chain_t chain = {.segments = segments, .capacity = 2, .count = 2};
    if (correlation % 3) {
        chain.indirect = GPUD_GPU_REQUEST_OFFSET + 3072;
        chain.indirect_length = 64;
        chain.indirect_first = correlation % 3 == 2;
        chain.indirect_descriptor = chain.indirect_first ? 1 : 0;
        segments[1].descriptor = chain.indirect_first ? 0 : 3;
    }
    kb2_vq_t *lane = &channel->lanes[GPUD_GPU_QUEUE_EXECUTION];
    int notify, ready, received_wake = 0;
    /* This single-outstanding profile arms before publication. Wait for its
     * used doorbell as well as ring completion before handing the multiplexed
     * IPC endpoint back to management; a late doorbell must not race QUIESCE. */
    IPC_CHECK(kb2_vq_arm(lane, ipc->generation, &ready) == KB2_VQ_OK && !ready);
    IPC_CHECK(kb2_vq_publish(lane, ipc->generation, &chain, &notify) == KB2_VQ_OK);
    if (notify) {
        struct ph_ipc_packet packet = {.operation = PH_GPU_QUEUE_NOTIFY,
                                       .generation = ipc->generation,
                                       .value = lane->queue.available_notification_id};
        ipc_test_send(ipc, &packet);
        if (test_case == 1)
            ipc_test_send(ipc, &packet); /* Duplicate wake, no duplicate query. */
    }
    kb2_vq_chain_t *done = NULL;
    for (;;) {
        kb2_vq_status_t status = kb2_vq_take_used(lane, ipc->generation, &done);
        if (status == KB2_VQ_OK)
            break;
        IPC_CHECK(status == KB2_VQ_EMPTY);
        int ready;
        IPC_CHECK(kb2_vq_arm(lane, ipc->generation, &ready) == KB2_VQ_OK);
        if (ready)
            continue;
        struct ph_ipc_packet packet = {0};
        IPC_CHECK(!ipc_test_receive(ipc, &packet));
        IPC_CHECK(packet.operation == PH_GPU_QUEUE_NOTIFY && packet.generation == ipc->generation &&
                  !packet.correlation && !packet.fd_count &&
                  packet.value == lane->queue.used_notification_id);
        received_wake = 1;
    }
    if (!received_wake) {
        struct ph_ipc_packet packet = {0};
        IPC_CHECK(!ipc_test_receive(ipc, &packet));
        IPC_CHECK(packet.operation == PH_GPU_QUEUE_NOTIFY && packet.generation == ipc->generation &&
                  !packet.correlation && !packet.fd_count &&
                  packet.value == lane->queue.used_notification_id);
    }
    IPC_CHECK(done == &chain && done->used_length >= KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE &&
              done->used_length <= PH_GPU_QUERY_PAGE);
    size_t reply_size = done->used_length;
    unsigned char reply[PH_GPU_QUERY_PAGE], output[GPUD_DRM_VERSION_BYTES];
    IPC_CHECK(kb2_vq_copy_response(lane, ipc->generation, done, 0, reply, reply_size) == KB2_VQ_OK);
    memcpy(output, mapping + GPUD_GPU_OUTPUT_OFFSET, sizeof(output));
    IPC_CHECK(!kb2_protocol_message_envelope_decode(reply, reply_size, &envelope));
    IPC_CHECK(envelope.protocol_id == KB2_GPU_PROTOCOL_ID &&
              envelope.opcode == KB2_GPU_OPCODE_COMMAND && !envelope.flags &&
              envelope.generation == ipc->generation && envelope.correlation_id == correlation &&
              envelope.payload_length == reply_size - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE);
    kb2_gpu_inline_completion_t completion;
    IPC_CHECK(!kb2_gpu_inline_completion_decode(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                                envelope.payload_length,
                                                session_id,
                                                &completion));
    if (closed) {
        IPC_CHECK(completion.status == KB2_GPU_STATUS_NOT_FOUND && !completion.length);
    } else if (test_case == 3 || test_case == 4) {
        IPC_CHECK(completion.status ==
                  (test_case == 3 ? KB2_GPU_STATUS_INVALID : KB2_GPU_STATUS_UNSUPPORTED));
        IPC_CHECK(!completion.length && !completion.record_schema_id);
    } else if (test_case == 2) {
        IPC_CHECK(completion.status == KB2_GPU_STATUS_OK &&
                  completion.record_schema_id == KB2_GPU_DRM_CORE_RECORD_SCALAR_U64 &&
                  completion.length == 8);
        IPC_CHECK(query_u32(completion.data) == 3 && !query_u32(completion.data + 4));
    } else {
        IPC_CHECK(completion.status == KB2_GPU_STATUS_OK && completion.length == 32 &&
                  completion.record_schema_id == KB2_GPU_DRM_CORE_RECORD_VERSION_RESULT);
        IPC_CHECK(!query_u32(completion.data + 12) && !query_u32(completion.data + 28));
        IPC_CHECK(query_u32(completion.data + 16) == strlen("virtio_gpu"));
        if (test_case == 1)
            IPC_CHECK(!memcmp(output, "virtio_gpu", strlen("virtio_gpu")));
        if (test_case == 5)
            IPC_CHECK(output[0] == 'v' && !output[1]);
        if (test_case == 6)
            for (size_t i = 0; i < sizeof(output); ++i)
                IPC_CHECK(!output[i]);
    }
    int native_result = gpud_drm_ioctl_reply(
        &request, &encoded, correlation, reply, reply_size, output, sizeof(output));
    IPC_CHECK(native_result == (closed           ? -ENOENT
                                : test_case == 3 ? -EINVAL
                                : test_case == 4 ? -EOPNOTSUPP
                                                 : 0));
    if (test_case == 1 && !closed) {
        drmd_version_wire_t version;
        memcpy(&version, request.data, sizeof(version));
        IPC_CHECK(version.name_length == strlen("virtio_gpu") &&
                  !memcmp(version.name, "virtio_gpu", version.name_length));
    }
    IPC_CHECK(kb2_vq_release(lane, ipc->generation, done) == KB2_VQ_OK);
}

static void query_round(struct ph_ipc *ipc,
                        uint64_t session_id,
                        uint64_t correlation,
                        unsigned int test_case,
                        int closed,
                        struct gpud_gpu_channel *channel,
                        unsigned char *mapping) {
    const struct gpud_drm_binding binding = {
        .generation = ipc->generation, .frontend_handle = 8, .session_id = session_id};
    query_bound_round(ipc, &binding, correlation, test_case, closed, channel, mapping);
}

static size_t control_exchange(struct ph_ipc *ipc,
                               struct gpud_gpu_channel *channel,
                               unsigned char *mapping,
                               size_t size,
                               unsigned char *reply) {
    struct gpud_gpu_rpc rpc = {.ipc = ipc, .channel = channel, .mapping = mapping};
    size_t reply_size = 0;
    IPC_CHECK(!gpud_gpu_rpc_call(&rpc, GPUD_GPU_QUEUE_CONTROL,
        mapping + GPUD_GPU_CONTROL_REQUEST_OFFSET, size,
        reply, GPUD_GPU_CONTROL_REPLY_CAPACITY, &reply_size));
    return reply_size;
}

static uint64_t session_round(struct ph_ipc *ipc,
                              struct gpud_gpu_channel *channel,
                              unsigned char *mapping,
                              uint64_t case_number,
                              uint64_t client,
                              uint64_t session,
                              uint32_t node,
                              uint32_t expected_status) {
    /* The frontend ownership phase uses control correlations below 20. */
    uint64_t correlation = 20 + case_number;
    uint32_t opcode = session ? KB2_GPU_OPCODE_SESSION_CLOSE : KB2_GPU_OPCODE_SESSION_OPEN;
    size_t payload =
        session ? KB2_GPU_SESSION_CLOSE_REQUEST_SIZE : KB2_GPU_SESSION_OPEN_REQUEST_SIZE;
    size_t size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + payload;
    unsigned char *bytes = mapping + GPUD_GPU_CONTROL_REQUEST_OFFSET;
    kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
                                                .opcode = opcode,
                                                .generation = ipc->generation,
                                                .correlation_id = correlation,
                                                .payload_length = payload};
    IPC_CHECK(!kb2_protocol_message_envelope_encode(bytes, size, &envelope));
    if (session) {
        IPC_CHECK(!kb2_gpu_session_close_encode(
            bytes + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, payload, session));
    } else {
        kb2_gpu_session_open_t request = {.client_id = client, .node_type = node};
        IPC_CHECK(!kb2_gpu_session_open_encode(
            bytes + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, payload, &request));
    }
    unsigned char reply[GPUD_GPU_CONTROL_REPLY_CAPACITY];
    size_t reply_size = control_exchange(ipc, channel, mapping, size, reply);
    IPC_CHECK(!kb2_protocol_message_envelope_decode(reply, reply_size, &envelope));
    IPC_CHECK(envelope.protocol_id == KB2_GPU_PROTOCOL_ID && envelope.opcode == opcode &&
              envelope.generation == ipc->generation && envelope.correlation_id == correlation &&
              !envelope.flags &&
              envelope.payload_length == reply_size - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE);
    kb2_gpu_session_completion_t completion;
    IPC_CHECK(!kb2_gpu_session_completion_decode(
        reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, envelope.payload_length, opcode, &completion));
    IPC_CHECK(completion.status == expected_status);
    if (session)
        IPC_CHECK(completion.session_id == session);
    else
        IPC_CHECK(expected_status ? !completion.session_id : completion.session_id != 0);
    return completion.session_id;
}

#endif

static void boot_round(kb2_controller_t *controller,
                       struct core_package *package,
                       struct gpud_native_launch *launch,
                       struct ph_ipc *ipc,
                       const void *image,
                       size_t image_size,
                       uint64_t generation) {
    uint64_t before = ipc_test_used_fds(), pair[2];
    IPC_CHECK(kb2_controller_generation(controller) == generation);
    publish_package(package, generation);
    IPC_CHECK(
        !pacha_syscall3(PACHA_IPC_SYSCALL_CHANNEL_CREATE,
                        (uintptr_t)pair,
                        PH_IPC_CHANNEL_RIGHTS | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER,
                        0));
    IPC_CHECK(!ph_ipc_init(ipc, pair[0], generation));
    const uint64_t resource_id = 100 + generation, sandbox_id = 200 + generation;
    complete(controller, KB2_ACTION_ALLOCATE_RESOURCES, resource_id);
    struct ph_sandbox_config config = {.identity = package->identity,
                                           .artifact_count = GPUD_CORE_TEST_ARTIFACTS,
                                           .resource_count = GPUD_CORE_TEST_RESOURCES,
                                           .device = package->device,
                                           .client_id = 10000 + generation,
                                           .gpu_channel_id = 20000 + generation};
    struct gpud_launch_blob blob = {
        .address = GPUD_CORE_TEST_CONFIG_ADDRESS, .bytes = &config, .size = sizeof(config)};
    struct pacha_process_fd_grant grant = {.source_fd = pair[1],
                                           .target_fd = GPUD_CORE_TEST_CHANNEL_FD,
                                           .rights = PH_IPC_CHANNEL_RIGHTS,
                                           .flags = PACHA_FD_FLAG_CLOEXEC};
    struct gpud_launch_request request = {.generation = generation,
                                          .image = image,
                                          .image_size = image_size,
                                          .load_bias = UINT64_C(0x40000000),
                                          .stack_address = IPC_TEST_STACK,
                                          .stack_size = 256 * 1024,
                                          .blobs = &blob,
                                          .blob_count = 1,
                                          .grants = &grant,
                                          .grant_count = 1};
    struct gpud_launch_resources resources = {.generation = generation,
                                              .resource_set_id = resource_id,
                                              .sandbox_id = sandbox_id,
                                              .request = &request};
    memcpy(resources.manifest_digest, package->identity.manifest_digest, 32);
    struct gpud_launch_transaction transaction = {0};
    IPC_CHECK(gpud_launch_begin(&transaction, controller, launch, ipc, &resources) ==
              KB2_STATUS_OK);
    ipc_test_close(pair[1]);
    IPC_CHECK(gpud_launch_step(&transaction) == KB2_STATUS_OK && transaction.completed);
    struct gpud_process_watch watch = {0};
    IPC_CHECK(gpud_process_watch_init(
                  &watch, controller, &launch->process, ipc, resource_id, sandbox_id) ==
              KB2_STATUS_OK);
    struct gpud_bootstrap_resources transfer_resources = {
        .generation = generation,
        .resource_set_id = resource_id,
        .sandbox_id = sandbox_id,
        .items = package->items,
        .artifact_count = GPUD_CORE_TEST_ARTIFACTS,
        .resource_handle_count = GPUD_CORE_TEST_RESOURCES};
    memcpy(transfer_resources.manifest_digest, package->identity.manifest_digest, 32);
    struct gpud_bootstrap_transfer transfer = {0};
    IPC_CHECK(gpud_bootstrap_begin(&transfer, controller, ipc, &transfer_resources) ==
              KB2_STATUS_OK);
    uint64_t deadline = ipc_test_now() + UINT64_C(180000000000);
    while (!transfer.completed) {
        kb2_status_t result = gpud_bootstrap_step(&transfer);
        IPC_CHECK(result == KB2_STATUS_OK || result == KB2_STATUS_ACTION_PENDING);
        IPC_CHECK(ipc_test_now() < deadline);
    }
    IPC_CHECK(kb2_controller_state(controller) == KB2_STATE_HANDSHAKING);
    struct gpud_lifecycle lifecycle = {0};
    IPC_CHECK(gpud_lifecycle_init(&lifecycle, &watch) == KB2_STATUS_OK);
    kb2_status_t observed;
    while ((observed = gpud_lifecycle_ready(&lifecycle)) == KB2_STATUS_ACTION_PENDING)
        wait_channel(ipc->fd, deadline);
    IPC_CHECK(observed == KB2_STATUS_OK && kb2_controller_state(controller) == KB2_STATE_RUNNING);
#if GPUD_CORE_TEST_DEVICE
    long query_fd = pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE,
                                   GPUD_GPU_CHANNEL_SIZE,
                                   PH_GPU_QUERY_RIGHTS | PACHA_FD_RIGHT_TRANSFER,
                                   0);
    IPC_CHECK(query_fd >= 16 && query_fd < PACHA_FD_TABLE_LIMIT);
    unsigned char *query_mapping = ipc_test_map(query_fd, GPUD_GPU_CHANNEL_SIZE, IPC_TEST_RW);
    struct gpud_gpu_channel query_channel = {0};
    IPC_CHECK(!ph_gpu_channel_bind(&query_channel,
                                   query_mapping,
                                   generation,
                                   config.gpu_channel_id,
                                   KB2_VQ_DRIVER,
                                   &kb2_vq_x86_64_atomics));
    struct ph_ipc_packet query_bind = {
        .operation = PH_GPU_QUEUE_BIND,
        .generation = generation,
        .value = config.gpu_channel_id,
        .fd_count = 1,
        .fds = {{.fd = query_fd, .rights = PH_GPU_QUERY_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC}}};
    ipc_test_send(ipc, &query_bind);
    session_round(ipc,
                  &query_channel,
                  query_mapping,
                  1,
                  config.client_id,
                  0,
                  KB2_GPU_NODE_PRIMARY,
                  KB2_GPU_STATUS_UNSUPPORTED);
    session_round(ipc,
                  &query_channel,
                  query_mapping,
                  2,
                  config.client_id + 1,
                  0,
                  KB2_GPU_NODE_RENDER,
                  KB2_GPU_STATUS_DENIED);
    uint64_t session_a = session_round(ipc,
                                       &query_channel,
                                       query_mapping,
                                       3,
                                       config.client_id,
                                       0,
                                       KB2_GPU_NODE_RENDER,
                                       KB2_GPU_STATUS_OK);
    uint64_t session_b = session_round(ipc,
                                       &query_channel,
                                       query_mapping,
                                       4,
                                       config.client_id,
                                       0,
                                       KB2_GPU_NODE_RENDER,
                                       KB2_GPU_STATUS_OK);
    IPC_CHECK(session_a != session_b);
    session_round(ipc,
                  &query_channel,
                  query_mapping,
                  5,
                  config.client_id,
                  0,
                  KB2_GPU_NODE_RENDER,
                  KB2_GPU_STATUS_LIMIT);
    for (unsigned int test_case = 1; test_case <= 3; ++test_case)
        query_round(ipc, session_a, 100 + test_case, test_case, 0, &query_channel, query_mapping);
    session_round(
        ipc, &query_channel, query_mapping, 6, config.client_id, session_a, 0, KB2_GPU_STATUS_OK);
    query_round(ipc, session_a, 104, 2, 1, &query_channel, query_mapping);
    for (unsigned int test_case = 4; test_case <= 6; ++test_case)
        query_round(ipc, session_b, 101 + test_case, test_case, 0, &query_channel, query_mapping);
    uint64_t session_c = session_round(ipc,
                                       &query_channel,
                                       query_mapping,
                                       7,
                                       config.client_id,
                                       0,
                                       KB2_GPU_NODE_RENDER,
                                       KB2_GPU_STATUS_OK);
    IPC_CHECK(session_c != session_a && session_c != session_b);
    session_round(ipc,
                  &query_channel,
                  query_mapping,
                  8,
                  config.client_id,
                  session_a,
                  0,
                  KB2_GPU_STATUS_NOT_FOUND);
    /* B and C intentionally remain open. The GPL device owner, not a wire
     * CLOSE from a surviving client, must reclaim both before module unload. */
    /* Drain redundant used wakes before returning management IPC ownership
     * to the controller lifecycle. Ring progress, not packet count, decided completion. */
    for (;;) {
        struct ph_ipc_packet wake = {0};
        int result = ph_ipc_receive(ipc, generation, &wake);
        if (result == -EAGAIN)
            break;
        IPC_CHECK(
            !result && wake.operation == PH_GPU_QUEUE_NOTIFY && !wake.fd_count &&
            !wake.correlation &&
            (wake.value == query_channel.queues[GPUD_GPU_QUEUE_EXECUTION].used_notification_id ||
             wake.value == query_channel.queues[GPUD_GPU_QUEUE_CONTROL].used_notification_id));
    }
    ipc_test_log("NATIVE_GPUD_GPU_QUERY=PASS LPR-encode split-virtqueue DRM-query completion\n");
#endif
    /* Device profile READY follows real driver bind and node registration.
     * No external DRM endpoint is published yet. Both profiles stop through
     * real graceful lifecycle, never a synthesized READY/STOPPED pair. */
    IPC_CHECK((generation == 1 ? kb2_controller_restart(controller)
                               : kb2_controller_stop(controller)) == KB2_STATUS_OK);
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    IPC_CHECK(kb2_action_type(action) == KB2_ACTION_QUIESCE_SANDBOX);
    const uint64_t token = kb2_action_token(action);
    while ((observed = gpud_lifecycle_quiesce(&lifecycle, token)) == KB2_STATUS_ACTION_PENDING) {
        if (lifecycle.quiesce_sent)
            wait_channel(launch->process.fd, deadline);
        else
            IPC_CHECK(ipc_test_now() < deadline);
    }
    IPC_CHECK(observed == KB2_STATUS_OK && lifecycle.completed && launch->process.terminal &&
              launch->process.exit.state == GPUD_PROCESS_EXITED && !launch->process.exit.code);
#if GPUD_CORE_TEST_DEVICE
    kb2_vq_channel_fault(&query_channel.channel);
    ipc_test_unmap(query_mapping, GPUD_GPU_CHANNEL_SIZE);
    ipc_test_close(query_fd);
#endif
    IPC_CHECK(kb2_action_type(kb2_controller_pending_action(controller)) ==
              KB2_ACTION_REVOKE_RESOURCES);
#if GPUD_CORE_TEST_DEVICE
    /* Child reports success only after driver removal and explicit route /
     * mapping drain. Parent additionally disables device DMA synchronously.
     * This normal shutdown is not evidence of forced-death recovery. */
    IPC_CHECK(!pacha_syscall2(PACHA_CAPSULE_SYSCALL_DMA_SET_ENABLED, package->device_fd, 0));
#endif
    IPC_CHECK(!ph_ipc_destroy(ipc, generation));
    for (size_t i = 0; i < GPUD_CORE_TEST_BLOBS; ++i)
        ipc_test_close(package->items[i].capability.fd);
    complete(controller, KB2_ACTION_REVOKE_RESOURCES, 0);
    action = kb2_controller_pending_action(controller);
    IPC_CHECK(kb2_action_type(action) == KB2_ACTION_REAP_SANDBOX);
    IPC_CHECK(gpud_process_action(&watch, generation, kb2_action_token(action), 9) ==
              KB2_STATUS_OK);
    IPC_CHECK(!gpud_native_launch_discard(launch, generation));
    IPC_CHECK(!gpud_lifecycle_release(&lifecycle));
    complete(controller, KB2_ACTION_RELEASE_RESOURCES, 0);
    IPC_CHECK(ipc_test_used_fds() == before);
    IPC_CHECK(gpud_bootstrap_step(&transfer) ==
              (generation == 1 ? KB2_STATUS_STALE_GENERATION : KB2_STATUS_INVALID_STATE));
    ipc_test_log("NATIVE_GPUD_CORE_ROUND=PASS controller-launch transfer package core-boot "
                 "module-ready quiesce reap\n");
}

int main(void) {
    const uint64_t before = ipc_test_used_fds();
    const unsigned char *image;
    uint32_t image_size;
    IPC_CHECK(!seed0_bootfs_open_file("/tests/gpud-core-child.elf", &image, &image_size));
    struct core_package package = {0};
    prepare_package(&package);
    kb2_controller_t *controller = create_controller(&package);
    IPC_CHECK(kb2_controller_start(controller) == KB2_STATUS_OK);
    struct gpud_native_launch launch = {0};
    struct ph_ipc ipc = {0};
    for (uint64_t generation = 1; generation <= 2; ++generation)
        boot_round(controller, &package, &launch, &ipc, image, image_size, generation);
    IPC_CHECK(kb2_controller_state(controller) == KB2_STATE_IDLE);
    kb2_controller_destroy(controller);
    IPC_CHECK(ipc_test_used_fds() == before);
    ipc_test_log(
        "NATIVE_GPUD_CORE=PASS generations=2 regular-boot smp chapter1 chapter2-core no-fd-leak\n");
    return 0;
}

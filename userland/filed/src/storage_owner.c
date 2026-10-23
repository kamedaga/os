/* SPDX-License-Identifier: MIT */
#include <filed/storage_owner.h>

#include "../../kobox2_owner/boot.h"
#include "../../kobox2_owner/bootfs.h"
#include "../../kobox2_owner/fs_channel.h"
#include <kobox2/closure.h>
#include <kobox2/pci_function_layout.h>
#include <kobox2/resource_grant.h>
#include <kobox2/sha256.h>
#include <kobox2/virtqueue_x86_64.h>
#include <pacha/capsule.h>
#include <pacha/ipc.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef FILED_INTERNAL_RING_ONLY
#include "../tests/internal_microbench_clock.h"
#endif

/* Service admission budgets for queue inventory and its share of sandbox
 * RAM. These do not describe the kernel FD table or Linux RLIMIT_NOFILE. */
enum { owner_client_admission = 1024, owner_queue_admission = 4096 };

static const uint8_t device_schema[32] = KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES;

struct owner_queue {
    struct filed_storage_owner *owner;
    struct ph_fs_channel channel;
    kb2_vq_chain_t **slots;
    uint16_t *owners;
    pacha_fd_t vmo_fd;
    void *mapping;
    size_t mapping_size;
    uint64_t bind_operation, notify_operation;
    unsigned remote_prepared, remote_bound;
};

struct owner_client_queue {
    struct owner_client_queue *next;
    struct ph_fs_channel_config config;
    uint64_t client, charge;
    int bound;
};

struct filed_storage_owner {
    storage_filed_bootstrap_t config;
    struct ph_bootfs archive;
    const unsigned char *sandbox, *manifest;
    size_t sandbox_size, manifest_size, artifact_count;
    kb2_closure_manifest_t decoded;
    struct ph_package_identity identity;
    struct ph_device_authority device;
    struct ph_bootstrap_item *items;
    kb2_controller_t *controller;
    struct gpud_native_launch launch;
    struct ph_ipc ipc;
    struct pacha_ipc_channel_pair management;
    struct gpud_process_watch watch;
    struct gpud_lifecycle lifecycle;
    struct ph_owner_boot boot;
    struct owner_queue data, credentials;
    struct owner_queue *temporary;
    uint64_t next_exchange;
    struct owner_client_queue *clients;
    uint64_t client_bytes, client_count;
    uint64_t shared_expected;
    struct ph_ipc_packet shared_attachment;
    int started, failed;
};

static int receive_shared(struct filed_storage_owner *owner, struct ph_ipc_packet *packet)
{
    if (!owner->shared_expected || owner->shared_attachment.fd_count ||
        packet->correlation != owner->shared_expected || !packet->value || packet->fd_count != 1)
        return -EPROTO;
    owner->shared_attachment = *packet;
    memset(packet, 0, sizeof(*packet));
    return 0;
}

static void *allocate(void *context, size_t size)
{ (void)context; return malloc(size); }
static void deallocate(void *context, void *pointer, size_t size)
{ (void)context; (void)size; free(pointer); }

static int make_blob(const void *bytes, size_t size, struct ph_bootstrap_item *item)
{
    if (!bytes || !size || size > SIZE_MAX - 4095) return -EINVAL;
    size_t mapped_size = (size + 4095) & ~(size_t)4095;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_READ |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    pacha_fd_t fd = pacha_vmo_create(mapped_size, rights, 0);
    if (fd < 16) return fd < 0 ? fd : -ENOMEM;
    void *mapping = pacha_mmap(fd, mapped_size, PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED, 0);
    if (!mapping) { (void)pacha_fd_close(fd); return -ENOMEM; }
    memcpy(mapping, bytes, size);
    if (size < mapped_size) memset((unsigned char *)mapping + size, 0, mapped_size - size);
    int error = pacha_munmap(mapping, mapped_size);
    if (error) { (void)pacha_fd_close(fd); return error; }
    /* The sandbox and its kobox2 adapter boot only from these BOOTFS blobs.
     * An accidental pager VMO fails FD_READ before it can be handed off. */
    const pacha_fd_t probe=pacha_fd_fcntl(fd,PACHA_FD_FCNTL_DUP,16,
        PACHA_FD_RIGHT_READ|PACHA_FD_RIGHT_CLOSE);
    unsigned char first=0;
    const int ordinary=probe>=16 && pacha_fd_read(probe,&first,1)==1 &&
        first==*(const unsigned char *)bytes;
    if (probe>=16) (void)pacha_fd_close(probe);
    if (!ordinary) {
        (void)pacha_fd_close(fd);
        return -EIO;
    }
    *item = (struct ph_bootstrap_item){.size = size,
        .capability = {.fd = (uint64_t)fd, .rights = PH_BOOTSTRAP_BLOB_RIGHTS,
            .flags = PACHA_FD_FLAG_CLOEXEC}};
    return 0;
}

static int load_artifacts(struct filed_storage_owner *owner)
{
    int error = ph_bootfs_find(&owner->archive, "/srv/kobox2/storage/sandbox.elf",
        &owner->sandbox, &owner->sandbox_size);
    if (!error) error = ph_bootfs_find(&owner->archive,
        "/srv/kobox2/storage/manifest.bin", &owner->manifest, &owner->manifest_size);
    if (error) return error;
    if (!owner->sandbox_size || kb2_closure_manifest_decode(owner->manifest,
            owner->manifest_size, &owner->decoded)) return -EBADMSG;
    owner->artifact_count = kb2_closure_manifest_artifact_count(&owner->decoded);
    if (!owner->artifact_count || owner->artifact_count > (SIZE_MAX / sizeof(*owner->items)) - 3 ||
        kb2_closure_manifest_resource_count(&owner->decoded) != 1) return -EBADMSG;
    kb2_closure_manifest_resource_t resource;
    if (kb2_closure_manifest_resource(&owner->decoded, 0, &resource) ||
        resource.slot_id != 1 || resource.type != KB2_CLOSURE_RESOURCE_DEVICE ||
        resource.minimum_count != 1 || resource.maximum_count != 1 ||
        resource.required_rights != KB2_PCI_FUNCTION_REQUIRED_RIGHTS ||
        resource.maximum_rights != KB2_PCI_FUNCTION_REQUIRED_RIGHTS ||
        resource.flags != (KB2_CLOSURE_RESOURCE_FLAG_REQUIRED | KB2_CLOSURE_RESOURCE_FLAG_SHARED) ||
        memcmp(resource.interface_schema_digest, device_schema, 32)) return -EBADMSG;
    owner->items = calloc(owner->artifact_count + 3, sizeof(*owner->items));
    if (!owner->items) return -ENOMEM;
    for (size_t i = 0; i < owner->artifact_count; ++i) {
        kb2_closure_manifest_artifact_t artifact;
        if (kb2_closure_manifest_artifact(&owner->decoded, i, &artifact) ||
            artifact.node_id != i + 1 || !artifact.namespace_name.length) return -EBADMSG;
        kb2_closure_string_t name = artifact.namespace_name;
        for (uint32_t j = 0; j < name.length; ++j) {
            unsigned char c = (unsigned char)name.data[j];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-')) return -EBADMSG;
        }
        if (!i && (name.length != 4 || memcmp(name.data, "core", 4))) return -EBADMSG;
        size_t path_size = (size_t)name.length + sizeof("/srv/kobox2/storage/modules/.ko");
        char *path = malloc(path_size);
        if (!path) return -ENOMEM;
        if (i) snprintf(path, path_size, "/srv/kobox2/storage/modules/%.*s.ko",
            (int)name.length, name.data);
        else strcpy(path, "/srv/kobox2/storage/core.so");
        const unsigned char *bytes;
        size_t size;
        error = ph_bootfs_find(&owner->archive, path, &bytes, &size);
        free(path);
        uint8_t digest[32];
        if (error) return error;
        kb2_sha256(bytes, size, digest);
        if (size != artifact.content_size || memcmp(digest, artifact.content_digest, 32))
            return -EBADMSG;
        error = make_blob(bytes, size, &owner->items[i + 2]);
        if (error) return error;
    }
    kb2_sha256(owner->manifest, owner->manifest_size, owner->identity.manifest_digest);
    return make_blob(owner->manifest, owner->manifest_size, &owner->items[0]);
}

static int configure_controller(struct filed_storage_owner *owner)
{
    kb2_closure_builder_t *builder = NULL;
    kb2_closure_t *closure = NULL;
    kb2_status_t status = kb2_closure_builder_create(allocate, deallocate, NULL,
        owner->identity.manifest_digest, 32, &builder);
    if (status != KB2_STATUS_OK) return -ENOMEM;
    for (size_t i = 0; i < owner->artifact_count && status == KB2_STATUS_OK; ++i) {
        kb2_closure_manifest_artifact_t artifact;
        if (kb2_closure_manifest_artifact(&owner->decoded, i, &artifact)) {
            status = KB2_STATUS_INVALID_ARGUMENT; break;
        }
        status = kb2_closure_builder_add_artifact(builder, artifact.node_id,
            i ? KB2_ARTIFACT_RELOCATABLE_MODULE : KB2_ARTIFACT_SHARED_PROVIDER,
            artifact.content_digest, 32, artifact.namespace_name.data, artifact.namespace_name.length);
        if (status == KB2_STATUS_OK)
            status = kb2_closure_builder_set_native_lifecycle(builder, artifact.node_id);
        if (status == KB2_STATUS_OK && (artifact.flags & KB2_CLOSURE_ARTIFACT_FLAG_ROOT))
            status = kb2_closure_builder_mark_root(builder, artifact.node_id);
    }
    for (size_t i = 0; i < kb2_closure_manifest_dependency_count(&owner->decoded) &&
            status == KB2_STATUS_OK; ++i) {
        kb2_closure_manifest_dependency_t edge;
        if (kb2_closure_manifest_dependency(&owner->decoded, i, &edge)) {
            status = KB2_STATUS_INVALID_ARGUMENT; break;
        }
        status = kb2_closure_builder_add_dependency(builder, edge.consumer_node_id, edge.provider_node_id);
    }
    if (status == KB2_STATUS_OK)
        status = kb2_closure_builder_add_resource(builder, 1, KB2_RESOURCE_DEVICE,
            device_schema, 32, 1, 1, KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
            KB2_PCI_FUNCTION_REQUIRED_RIGHTS, KB2_RESOURCE_REQUIRED | KB2_RESOURCE_SHARED);
    for (size_t i = 0; i < kb2_closure_manifest_binding_count(&owner->decoded) &&
            status == KB2_STATUS_OK; ++i) {
        kb2_closure_manifest_binding_t binding;
        if (kb2_closure_manifest_binding(&owner->decoded, i, &binding) || binding.slot_id != 1) {
            status = KB2_STATUS_INVALID_ARGUMENT; break;
        }
        status = kb2_closure_builder_bind_resource(builder, binding.slot_id, binding.node_id);
    }
    if (status == KB2_STATUS_OK) status = kb2_closure_builder_seal(builder, &closure);
    if (status == KB2_STATUS_OK) status = kb2_controller_set_closure(owner->controller, closure);
    kb2_closure_destroy(closure);
    kb2_closure_builder_destroy(builder);
    if (status != KB2_STATUS_OK) return -EINVAL;
    uint8_t capability_digest[32], channel_digest[32];
    static const char channel_contract[] = PH_OWNER_CHANNEL_CONTRACT;
    kb2_sha256(device_schema, 32, capability_digest);
    kb2_sha256(channel_contract, sizeof(channel_contract) - 1, channel_digest);
    const uint8_t *const digests[] = {owner->identity.manifest_digest, capability_digest, channel_digest};
    /* Queue depth bounds outstanding requests; the service RAM budget can
     * reject admission earlier. Kernel capabilities grow independently. */
    if (owner->config.queue_size > UINT64_MAX / owner_queue_admission) return -EOVERFLOW;
    uint64_t limits[] = {owner->config.sandbox_ram_bytes, owner_queue_admission,
        owner_queue_admission, owner_queue_admission * owner->config.queue_size};
    for (unsigned kind = KB2_DIGEST_PROFILE; kind <= KB2_DIGEST_CHANNEL_SET; ++kind)
        if (kb2_controller_set_digest(owner->controller, kind,
                digests[kind - KB2_DIGEST_PROFILE], 32) != KB2_STATUS_OK) return -EINVAL;
    for (unsigned kind = 0; kind <= KB2_LIMIT_OUTSTANDING_REQUEST_COUNT; ++kind)
        if (kb2_controller_set_limit(owner->controller, kind, limits[kind]) != KB2_STATUS_OK) return -EINVAL;
    return 0;
}

static int bind_resources(struct filed_storage_owner *owner)
{
    uint64_t generation = kb2_controller_generation(owner->controller);
    owner->identity.generation = generation;
    owner->device.generation = generation;
    kb2_resource_grant_slot_source_t slot = {.slot_id = 1,
        .resource_type = KB2_CLOSURE_RESOURCE_DEVICE, .state = KB2_RESOURCE_GRANT_SLOT_PRESENT};
    memcpy(slot.interface_schema_digest, device_schema, 32);
    kb2_resource_grant_object_source_t object = {.slot_id = 1, .object_id = 1,
        .granted_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS};
    kb2_resource_grant_handle_binding_t handle = {.object_id = 1,
        .role = KB2_PCI_FUNCTION_NATIVE_HANDLE_ROLE_DEVICE, .transfer_handle_index = 0};
    kb2_resource_grant_source_t source = {.generation = generation, .slots = &slot, .slot_count = 1,
        .objects = &object, .object_count = 1, .handle_bindings = &handle, .handle_binding_count = 1};
    memcpy(source.closure_manifest_digest, owner->identity.manifest_digest, 32);
    size_t capacity, used;
    if (kb2_resource_grant_encoded_size(&source, &capacity)) return -EINVAL;
    uint8_t *grant = malloc(capacity);
    if (!grant) return -ENOMEM;
    int error = kb2_resource_grant_encode(grant, capacity, &used, &source) ? -EINVAL : 0;
    if (!error) {
        kb2_sha256(grant, used, owner->identity.grant_digest);
        error = make_blob(grant, used, &owner->items[1]);
    }
    free(grant);
    if (error) return error;
    owner->items[owner->artifact_count + 2] = (struct ph_bootstrap_item){
        .capability = {.fd = owner->config.device_fd, .rights = PH_DEVICE_FUNCTION_RIGHTS,
            .flags = PACHA_FD_FLAG_CLOEXEC}};
    const kb2_action_t *action = kb2_controller_pending_action(owner->controller);
    if (!action || kb2_action_type(action) != KB2_ACTION_ALLOCATE_RESOURCES) return -EINVAL;
    return kb2_controller_complete_action(owner->controller, generation, kb2_action_token(action),
        KB2_STATUS_OK, 1, 0) == KB2_STATUS_OK ? 0 : -EIO;
}

static int wait_packet(struct filed_storage_owner *owner, struct ph_ipc_packet *packet)
{
    for (;;) {
#ifdef FILED_INTERNAL_RING_ONLY
        uint64_t start = filed_bench_collect ? filed_bench_tsc() : 0;
#endif
        int error = ph_ipc_receive(&owner->ipc, owner->identity.generation, packet);
#ifdef FILED_INTERNAL_RING_ONLY
        if (filed_bench_collect) {
            filed_bench_wait_cycles[0] += filed_bench_tsc() - start;
            ++filed_bench_wait_counts[0];
            filed_bench_wait_counts[1] += error == -EAGAIN;
        }
#endif
        if (error != -EAGAIN) return error;
        struct pacha_pollfd fds[2] = {
            {.fd = owner->ipc.fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
            {.fd = owner->launch.process.fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP}};
#ifdef FILED_INTERNAL_RING_ONLY
        start = filed_bench_collect ? filed_bench_tsc() : 0;
#endif
        long waited = pacha_fd_wait_many(fds, 2, PACHA_FD_WAIT_FOREVER);
#ifdef FILED_INTERNAL_RING_ONLY
        if (filed_bench_collect) {
            filed_bench_wait_cycles[1] += filed_bench_tsc() - start;
            ++filed_bench_wait_counts[2];
        }
#endif
        /* Drain a final Linux failure report before treating process exit as
         * EOF; a successful send does not prove storage readiness. */
        if (waited > 0 && (fds[0].revents & PACHA_FD_EVENT_READABLE)) continue;
        return -EPIPE;
    }
}

static int send_packet(struct filed_storage_owner *owner, struct ph_ipc_packet *packet)
{
    for (;;) {
        int error = ph_ipc_send(&owner->ipc, packet);
        if (error != -EAGAIN) return error;
        struct pacha_pollfd fds[2] = {
            {.fd = owner->ipc.fd, .events = PACHA_FD_EVENT_WRITABLE | PACHA_FD_EVENT_HANGUP},
            {.fd = owner->launch.process.fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP}};
        long waited = pacha_fd_wait_many(fds, 2, PACHA_FD_WAIT_FOREVER);
        if (waited <= 0 || fds[1].revents || (fds[0].revents & PACHA_FD_EVENT_HANGUP)) return -EPIPE;
    }
}

int filed_storage_owner_client_config(struct filed_storage_owner *owner, uint64_t client, uint64_t minimum,
    struct ph_fs_channel_config *config)
{
    if (!owner || !config || !owner->started || owner->failed) return -EPIPE;
    if (!client) return -EINVAL;
    if (owner->next_exchange == UINT64_MAX) return -EOVERFLOW;
    uint64_t budget = owner->config.sandbox_ram_bytes / 4;
    /* The owner's large control arena is not a per-process reservation.
     * Derive idle client capacity from RAM and the service client admission budget;
     * larger operations request their own exact authenticated lease below. */
    uint64_t base_arena = (budget / (2 * owner_client_admission)) & ~4095ull;
    if (base_arena < 4096) base_arena = 4096;
    if (base_arena > owner->config.queue_arena_bytes) base_arena = owner->config.queue_arena_bytes;
    *config = (struct ph_fs_channel_config){.generation = owner->identity.generation,
        .channel_id = owner->next_exchange + 1,
        .arena_bytes = minimum > base_arena ? minimum : base_arena,
        .queue_size = (uint32_t)owner->config.queue_size};
    size_t bytes,offset;
    if (ph_fs_channel_layout(config,&bytes,&offset)) return -EOVERFLOW;
    /* Transport gets one quarter of configured sandbox RAM. Charge shared
     * backing, a worst-case immutable Linux input snapshot, and private ring
     * inventory together; keep the rest for VFS/cache/runtime. This is an
     * aggregate admission budget, never a pathname or file-size restriction.
     * Two retained filed capabilities per lease get half the real FD table;
     * the other half remains available to control/exec/device operations. */
    uint64_t inventory = 4096 + config->queue_size *
        (sizeof(kb2_vq_chain_t *) + sizeof(uint16_t) + sizeof(kb2_vq_segment_t) +
         (sizeof(struct ph_fs_pending) + 8) / 2);
    if (config->arena_bytes > UINT64_MAX - bytes ||
        inventory > UINT64_MAX - bytes - config->arena_bytes) return -EOVERFLOW;
    uint64_t charge = bytes + config->arena_bytes + inventory;
    if (charge > budget) return -ENOMEM;
    if (owner->client_bytes > budget - charge || owner->client_count >= owner_client_admission)
        return -EAGAIN;
    struct owner_client_queue *reserved = calloc(1, sizeof(*reserved));
    if (!reserved) return -ENOMEM;
    *reserved = (struct owner_client_queue){.config = *config, .client = client,
        .charge = charge, .next = owner->clients};
    /* PREPARE may have been observed even when its send fails. Keep this
     * reservation and its charge until ACK or terminal sandbox teardown. */
    owner->clients = reserved;
    owner->client_bytes += charge;
    ++owner->client_count;
    ++owner->next_exchange;
    /* Reserve authority in ID order, before handing config to a client.
     * Different clients can subsequently attach in any order without reviving
     * a retired ID or making their scheduling part of authentication. */
    struct ph_ipc_packet packet = {.operation = PH_FS_CLIENT_PREPARE,
        .generation = config->generation, .correlation = config->channel_id, .value = client};
    int error = send_packet(owner, &packet);
    if (error) owner->failed = 1;
    return error;
}

static int client_ack(struct filed_storage_owner *owner, uint64_t id, uint64_t operation)
{
    for (;;) {
        struct ph_ipc_packet packet = {0};
        int error = wait_packet(owner, &packet);
        int acknowledged = packet.operation == operation && packet.correlation == id &&
            !packet.value && !packet.fd_count;
        /* Used doorbells can outlive a synchronously collected owner call.
         * Drain only known base-lane hints, never swallow a different ACK. */
        int stale = (packet.operation == PH_FS_QUEUE_NOTIFY || packet.operation == PH_FS_OWNER_NOTIFY) &&
            !packet.correlation && packet.value == 2 && !packet.fd_count;
        if (!error && !acknowledged && !stale) error = -EPROTO;
        int closed = ph_ipc_packet_release(&packet);
        if (closed) {
            owner->ipc.rejected = packet;
            owner->ipc.admitted = 0;
        }
        if (error || closed) return error ? error : closed;
        if (acknowledged) return 0;
    }
}

int filed_storage_owner_client_bind(struct filed_storage_owner *owner,
    const struct ph_fs_channel_config *config, pacha_fd_t vmo_fd, pacha_fd_t doorbell_fd)
{
    if (!owner || !owner->started || owner->failed) return -EPIPE;
    if (!config || config->generation != owner->identity.generation ||
        config->channel_id <= 2 || config->channel_id > owner->next_exchange ||
        config->queue_size != owner->config.queue_size ||
        !config->arena_bytes || vmo_fd < 16 ||
        doorbell_fd < 16 || vmo_fd == doorbell_fd) return -EINVAL;
    struct owner_client_queue *reserved = owner->clients;
    while (reserved && reserved->config.channel_id != config->channel_id) reserved = reserved->next;
    if (!reserved || reserved->config.generation != config->generation ||
        reserved->config.arena_bytes != config->arena_bytes ||
        reserved->config.queue_size != config->queue_size) return -EINVAL;
    if (reserved->bound) return -EEXIST;
    reserved->bound = 1;
    struct ph_ipc_packet packet = {.operation = PH_FS_CLIENT_BIND,
        .generation = config->generation, .correlation = config->channel_id,
        .value = config->arena_bytes, .fd_count = 2,
        .fds = {{.fd = (uint64_t)vmo_fd,
            .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE,
            .flags = PACHA_FD_FLAG_CLOEXEC}, {.fd = (uint64_t)doorbell_fd,
            .rights = PH_FS_CLIENT_DOORBELL_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC}}};
    int error = send_packet(owner, &packet);
    if (!error) error = client_ack(owner, config->channel_id, PH_FS_CLIENT_BOUND);
    /* A failed publication/ACK is ambiguous. Stop new owner work; only
     * terminal sandbox teardown can release the caller's uncertain lease. */
    if (error) owner->failed = 1;
    return error;
}

int filed_storage_owner_client_release(struct filed_storage_owner *owner, uint64_t id)
{
    if (!owner || !owner->started || owner->failed) return -EPIPE;
    if (id <= 2 || id > owner->next_exchange) return -EINVAL;
    struct owner_client_queue **cursor = &owner->clients;
    while (*cursor && (*cursor)->config.channel_id != id) cursor = &(*cursor)->next;
    if (!*cursor) return -ENOENT;
    struct ph_ipc_packet packet = {.operation = PH_FS_CLIENT_RELEASE,
        .generation = owner->identity.generation, .correlation = id};
    int error = send_packet(owner, &packet);
    if (!error) error = client_ack(owner, id, PH_FS_CLIENT_RELEASED);
    if (error) owner->failed = 1;
    else {
        struct owner_client_queue *released = *cursor;
        *cursor = released->next;
        owner->client_bytes -= released->charge;
        --owner->client_count;
        free(released);
    }
    return error;
}

static int open_queue(struct filed_storage_owner *owner, struct owner_queue *queue,
    uint64_t id, uint64_t bind_operation, uint64_t notify_operation, uint64_t arena_bytes,
    uint64_t prepare_operation, uint64_t client)
{
    queue->owner = owner;
    queue->bind_operation = bind_operation;
    queue->notify_operation = notify_operation;
    struct ph_fs_channel_config config = {.generation = owner->identity.generation,
        .channel_id = id, .arena_bytes = arena_bytes,
        .queue_size = (uint32_t)owner->config.queue_size};
    size_t arena_offset;
    int error = ph_fs_channel_layout(&config, &queue->mapping_size, &arena_offset);
    if (error) return error;
    queue->slots = calloc(config.queue_size, sizeof(*queue->slots));
    queue->owners = calloc(config.queue_size, sizeof(*queue->owners));
    if (!queue->slots || !queue->owners) return -ENOMEM;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    queue->vmo_fd = pacha_vmo_create(queue->mapping_size, rights, 0);
    if (queue->vmo_fd < 16) return queue->vmo_fd < 0 ? queue->vmo_fd : -ENOMEM;
    queue->mapping = pacha_mmap(queue->vmo_fd, queue->mapping_size,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!queue->mapping) return -ENOMEM;
    memset(queue->mapping, 0, queue->mapping_size);
    error = ph_fs_channel_bind(&queue->channel, queue->mapping, queue->mapping_size,
        &config, KB2_VQ_DRIVER, &kb2_vq_x86_64_atomics, queue->slots, queue->owners);
    if (error) return error;
    if (prepare_operation) {
        struct ph_ipc_packet prepare = {.operation = prepare_operation,
            .generation = owner->identity.generation, .correlation = id, .value = client};
        error = send_packet(owner, &prepare);
        if (error) return error;
        queue->remote_prepared = 1;
    }
    struct ph_ipc_packet packet = {.operation = bind_operation,
        .generation = owner->identity.generation,
        .correlation = bind_operation == PH_FS_EXCHANGE_BIND ? id : 0,
        .value = bind_operation == PH_FS_EXCHANGE_BIND ? arena_bytes : id, .fd_count = 1,
        .fds = {{.fd = (uint64_t)queue->vmo_fd,
            .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_MAP_READ |
                PACHA_FD_RIGHT_MAP_WRITE, .flags = PACHA_FD_FLAG_CLOEXEC}}};
    error = send_packet(owner, &packet);
    if (!error) queue->remote_bound = 1;
    /* Binding is ordered on the management channel. The caller's subsequent
     * owner HELLO, not enqueue success, proves the queue has been accepted. */
    return error;
}

static int queue_notify(void *context, uint32_t notification)
{
    struct owner_queue *queue = context;
    struct ph_ipc_packet packet = {.operation = queue->notify_operation,
        .generation = queue->owner->identity.generation,
        .correlation = queue->notify_operation == PH_FS_EXCHANGE_NOTIFY ?
            queue->channel.config.channel_id : 0,
        .value = notification};
    return send_packet(queue->owner, &packet);
}

static int queue_wait(void *context, uint32_t notification)
{
    struct owner_queue *queue = context;
    /* The boot transaction closes our copy of the sandbox endpoint before
     * admitting data calls. RECV_WAIT then reports peer death directly; an
     * extra process poll would only add a readiness wake and another RECV.
     * Startup/teardown retain wait_packet's process-watch multiplexing. */
    if (queue->owner->management.b >= 16) return -EPROTO;
    for (;;) {
        struct ph_ipc_packet packet = {0};
#ifdef FILED_INTERNAL_RING_ONLY
        uint64_t start = filed_bench_collect ? filed_bench_tsc() : 0;
#endif
        int error = ph_ipc_receive_wait(&queue->owner->ipc,
            queue->owner->identity.generation, &packet, PACHA_FD_WAIT_FOREVER);
#ifdef FILED_INTERNAL_RING_ONLY
        if (filed_bench_collect) {
            filed_bench_wait_cycles[0] += filed_bench_tsc() - start;
            ++filed_bench_wait_counts[0];
            filed_bench_wait_counts[1] += error == -EAGAIN;
        }
#endif
        int known_lane = (packet.operation == PH_FS_QUEUE_NOTIFY ||
            packet.operation == PH_FS_OWNER_NOTIFY) && !packet.correlation;
        if (!error && packet.operation == PH_FS_SHARED_ATTACHMENT) {
            error = receive_shared(queue->owner, &packet);
            if (!error) continue;
        }
        int temporary_lane = packet.operation == PH_FS_EXCHANGE_NOTIFY &&
            packet.correlation == queue->channel.config.channel_id &&
            queue->notify_operation == PH_FS_EXCHANGE_NOTIFY;
        if (!error && ((!known_lane && !temporary_lane) ||
                packet.value != notification || packet.fd_count))
            error = -EPROTO;
        int own_lane = packet.operation == queue->notify_operation &&
            (queue->notify_operation != PH_FS_EXCHANGE_NOTIFY || temporary_lane);
        int closed = ph_ipc_packet_release(&packet);
        if (closed) {
            /* Unexpected ancillary capabilities still belong to us after a
             * failed close. Keep them in endpoint cleanup inventory instead
             * of dropping the only record when this stack packet expires. */
            queue->owner->ipc.rejected = packet;
            queue->owner->ipc.admitted = 0;
        }
        if (error || closed) return error ? error : closed;
        /* The ring can complete before its advisory doorbell is consumed.
         * Drain a stale other-lane wake; only the ring proves completion. */
        if (own_lane) return 0;
    }
}

int filed_storage_owner_create(pacha_fd_t bootfs_fd, uint64_t bootfs_size,
    const storage_filed_bootstrap_t *bootstrap, struct filed_storage_owner **out)
{
    if (!out || !bootstrap || bootfs_fd < 16 || bootstrap->device_fd < 16 ||
        !storage_root_uuid_valid(bootstrap->root_uuid) || !bootstrap->sandbox_ram_bytes ||
        bootstrap->queue_size > UINT32_MAX || !bootstrap->queue_arena_bytes) return -EINVAL;
    *out = NULL;
    struct filed_storage_owner *owner = calloc(1, sizeof(*owner));
    if (!owner) return -ENOMEM;
    owner->config = *bootstrap;
    owner->next_exchange = 2;
    owner->management = (struct pacha_ipc_channel_pair){.a = -1, .b = -1};
    struct pacha_capsule_info info = {0};
    int error = pacha_capsule_query((pacha_fd_t)bootstrap->device_fd, &info);
    uint64_t required = PH_DEVICE_FUNCTION_RIGHTS | PACHA_FD_RIGHT_TRANSFER;
    if (!error && (info.kind != PACHA_CAPSULE_KIND_DEVICE || !info.device ||
            (info.rights & required) != required || !(info.flags & PACHA_CAPSULE_DMA_TRANSLATED) ||
            (info.flags & PACHA_CAPSULE_DMA_QUARANTINED))) error = -EACCES;
    owner->device = (struct ph_device_authority){.object_id = 1,
        .native_device = info.device, .slot_id = 1, .node_id = 1};
    if (!error) error = ph_bootfs_open(&owner->archive, bootfs_fd, bootfs_size);
    if (!error) error = load_artifacts(owner);
    if (!error && kb2_controller_create(allocate, deallocate, NULL,
            &owner->controller) != KB2_STATUS_OK) error = -ENOMEM;
    if (!error) error = configure_controller(owner);
    if (error) { (void)filed_storage_owner_destroy(owner); return error; }
    *out = owner;
    return 0;
}

int filed_storage_owner_start(struct filed_storage_owner *owner)
{
    if (!owner || owner->started || owner->failed || owner->boot.phase) return -EINVAL;
    kb2_status_t controller_status = kb2_controller_start(owner->controller);
    int error = controller_status == KB2_STATUS_OK ? 0 : -EIO;
    if (error) fprintf(stderr, "[filed-storage] controller start status=%u state=%u\n",
        (unsigned)controller_status, (unsigned)kb2_controller_state(owner->controller));
    if (!error) {
        error = bind_resources(owner);
        if (error) fprintf(stderr, "[filed-storage] resource bind status=%d state=%u\n",
            error, (unsigned)kb2_controller_state(owner->controller));
    }
    struct ph_sandbox_config sandbox = {.size = sizeof(struct ph_sandbox_config),
        .identity = owner->identity, .artifact_count = owner->artifact_count,
        .resource_count = 1, .device = owner->device,
        .storage_devices_address = PH_SANDBOX_STORAGE_DEVICES_ADDRESS,
        .storage_device_count = 1,
        .fs_channel_id = 1, .fs_owner_channel_id = 2,
        .fs_arena_bytes = owner->config.queue_arena_bytes,
        .ram_bytes = owner->config.sandbox_ram_bytes,
        .fs_queue_size = (uint32_t)owner->config.queue_size};
    memcpy(sandbox.root_uuid, owner->config.root_uuid, 16);
    const struct gpud_launch_blob storage_devices = {.bytes = &owner->device,
        .size = sizeof(owner->device), .address = PH_SANDBOX_STORAGE_DEVICES_ADDRESS};
    struct ph_owner_boot_config start = {.controller = owner->controller,
        .launch = &owner->launch, .ipc = &owner->ipc, .management = &owner->management,
        .watch = &owner->watch, .lifecycle = &owner->lifecycle,
        .resource_set_id = 1, .sandbox_id = 1, .sandbox_config = &sandbox,
        .image = owner->sandbox, .image_size = owner->sandbox_size, .items = owner->items,
        .artifact_count = owner->artifact_count, .resource_count = 1, .log_endpoint_fd = -1};
    start.extra_blobs = &storage_devices;
    start.extra_blob_count = 1;
    if (!error) {
        kb2_status_t status = ph_owner_boot_begin(&owner->boot, &start);
        if (status != KB2_STATUS_OK) {
            fprintf(stderr, "[filed-storage] start begin status=%u phase=%u native=%d\n",
                (unsigned)status, (unsigned)owner->boot.phase, owner->boot.native_error);
            error = -EIO;
        }
    }
    while (!error && owner->boot.phase != PH_OWNER_BOOT_COMPLETE) {
        kb2_status_t status = ph_owner_boot_step(&owner->boot);
        if (status != KB2_STATUS_OK && status != KB2_STATUS_ACTION_PENDING) {
            fprintf(stderr, "[filed-storage] start step status=%u phase=%u native=%d\n",
                (unsigned)status, (unsigned)owner->boot.phase, owner->boot.native_error);
            error = -EIO;
        }
    }
    while (!error) {
        kb2_status_t status = gpud_lifecycle_ready(&owner->lifecycle);
        if (status == KB2_STATUS_OK) break;
        if (status != KB2_STATUS_ACTION_PENDING) {
            fprintf(stderr, "[filed-storage] Linux ready status=%u phase=%u native=%d\n",
                (unsigned)status, (unsigned)owner->boot.phase, owner->boot.native_error);
            error = -EIO; break;
        }
        struct pacha_pollfd fds[2] = {
            {.fd = owner->ipc.fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
            {.fd = owner->launch.process.fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP}};
        long waited = pacha_fd_wait_many(fds, 2, PACHA_FD_WAIT_FOREVER);
        if (waited <= 0 || (!(fds[0].revents & PACHA_FD_EVENT_READABLE) && fds[1].revents))
            error = -EPIPE;
    }
    if (!error) error = open_queue(owner, &owner->data, 1, PH_FS_QUEUE_BIND,
        PH_FS_QUEUE_NOTIFY, owner->config.queue_arena_bytes, 0, 0);
    if (!error) error = open_queue(owner, &owner->credentials, 2, PH_FS_OWNER_BIND,
        PH_FS_OWNER_NOTIFY, owner->config.queue_arena_bytes, 0, 0);
    if (error) owner->failed = 1;
    else owner->started = 1;
    return error;
}

static int close_queue(struct owner_queue *queue);

static int release_exchange(struct owner_queue *queue)
{
    struct filed_storage_owner *owner = queue->owner;
    uint64_t id = queue->channel.config.channel_id;
    struct ph_ipc_packet release = {.operation = PH_FS_EXCHANGE_RELEASE,
        .generation = owner->identity.generation, .correlation = id};
    int error = send_packet(owner, &release);
    while (!error) {
        struct ph_ipc_packet packet = {0};
        error = wait_packet(owner, &packet);
        int acknowledgement = packet.operation == PH_FS_EXCHANGE_RELEASED &&
            packet.correlation == id && !packet.value && !packet.fd_count;
        int stale_wake = packet.value == 2 && !packet.fd_count &&
            (((packet.operation == PH_FS_QUEUE_NOTIFY || packet.operation == PH_FS_OWNER_NOTIFY) &&
                !packet.correlation) || (packet.operation == PH_FS_EXCHANGE_NOTIFY &&
                packet.correlation == id));
        if (!error && !acknowledgement && !stale_wake) error = -EPROTO;
        int closed = ph_ipc_packet_release(&packet);
        if (!error) error = closed;
        if (!error && acknowledgement) {
            queue->remote_prepared = queue->remote_bound = 0;
            return 0;
        }
    }
    return error;
}

static int storage_call(struct filed_storage_owner *owner,
    const uint8_t *request, size_t size, uint8_t *response, size_t capacity, size_t *used,
    int control,void *data,kb2_fs_response_t *decoded_response)
{
    if (!owner || !owner->started || owner->failed) return -EPIPE;
    kb2_fs_request_t decoded;
    if (kb2_fs_request_decode(request, size, size, &decoded)) return -EPROTO;
    struct owner_queue *queue = control ? &owner->credentials : &owner->data;
    if (size > SIZE_MAX - capacity) return -EOVERFLOW;
    int exchange = size + capacity > owner->config.queue_arena_bytes;
    if (exchange) {
        if (owner->temporary || owner->next_exchange == UINT64_MAX) return -EOVERFLOW;
        owner->temporary = calloc(1, sizeof(*owner->temporary));
        if (!owner->temporary) return -ENOMEM;
        uint64_t prepare = queue == &owner->credentials ?
            PH_FS_EXCHANGE_OWNER_PREPARE : PH_FS_EXCHANGE_PREPARE;
        queue = owner->temporary;
        int error = open_queue(owner, queue, ++owner->next_exchange,
            PH_FS_EXCHANGE_BIND, PH_FS_EXCHANGE_NOTIFY, size + capacity, prepare, decoded.client);
        if (error) {
            /* Published backing objects remain pinned until retirement, even
             * when a later management send fails before the first request. */
            if (queue->remote_prepared || queue->remote_bound) owner->failed = 1;
            else if (!close_queue(queue)) { free(queue); owner->temporary = NULL; }
            else owner->failed = 1;
            return error;
        }
    }
    struct ph_fs_channel_wait wait = {.context = queue, .notify = queue_notify, .wait = queue_wait};
    if (decoded.opcode == KB2_FILESYSTEM_OP_SHARED_FILE_VMO) {
        if (!control || owner->shared_expected || owner->shared_attachment.fd_count) return -EPROTO;
        owner->shared_expected = decoded.sequence;
    }
    int error = decoded_response ? ph_fs_channel_call_split(&queue->channel,
        request,size,decoded_response,data,capacity-KB2_FILESYSTEM_RESPONSE_SIZE,
        used,&wait) : ph_fs_channel_call(&queue->channel, request, size,
        response, capacity, used, &wait);
    if (!error && exchange) error = release_exchange(queue);
    if (!error && exchange) error = close_queue(queue);
    if (!error && exchange) { free(queue); owner->temporary = NULL; }
    if (error) owner->failed = 1;
    return error;
}

int filed_storage_owner_take_shared(struct filed_storage_owner *owner,
    uint64_t id, uint64_t sequence, uint64_t length, pacha_fd_t *fd)
{
    if (!owner || !fd || !id || owner->shared_expected != sequence ||
        owner->management.b >= 16) return -EPROTO;
    while (!owner->shared_attachment.fd_count) {
        struct ph_ipc_packet packet = {0};
        // Data admission has closed our copy of the sandbox endpoint. Use
        // the same peer-death-aware blocking receive as queue_wait, including
        // when the ring reply raced ahead of the ancillary capability.
        int error = ph_ipc_receive_wait(&owner->ipc, owner->identity.generation,
            &packet, PACHA_FD_WAIT_FOREVER);
        if (!error && packet.operation == PH_FS_SHARED_ATTACHMENT)
            error = receive_shared(owner, &packet);
        else if (!error && (packet.fd_count || packet.correlation ||
            (packet.operation != PH_FS_OWNER_NOTIFY && packet.operation != PH_FS_QUEUE_NOTIFY)))
            error = -EPROTO;
        int closed = ph_ipc_packet_release(&packet);
        if (closed) { owner->ipc.rejected = packet; owner->ipc.admitted = 0; }
        if (error || closed) { owner->failed = 1; return error ? error : closed; }
    }
    struct ph_ipc_packet *packet = &owner->shared_attachment;
    const uint64_t base = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ;
    const uint64_t allowed = base | PACHA_FD_RIGHT_MAP_WRITE | PACHA_FD_RIGHT_MAP_EXEC;
    struct pacha_fd_info info;
    if (packet->value != id || packet->correlation != sequence ||
        packet->fds[0].rights & ~allowed || (packet->fds[0].rights & base) != base ||
        packet->fds[0].flags != PACHA_FD_FLAG_CLOEXEC ||
        pacha_fd_get_info(packet->fds[0].fd, &info) || info.kind != PACHA_FD_KIND_VMO ||
        info.size != length || info.rights != packet->fds[0].rights || info.flags != PACHA_FD_FLAG_CLOEXEC) {
        owner->failed = 1;
        return -EPROTO;
    }
    *fd = packet->fds[0].fd;
    memset(packet, 0, sizeof(*packet));
    owner->shared_expected = 0;
    return 0;
}

int filed_storage_owner_cancel_shared(struct filed_storage_owner *owner)
{
    if (!owner) return -EINVAL;
    int error = ph_ipc_packet_release(&owner->shared_attachment);
    if (!error) owner->shared_expected = 0;
    return error;
}

int filed_storage_owner_call(struct filed_storage_owner *owner,
    const uint8_t *request, size_t size, uint8_t *response, size_t capacity, size_t *used)
{
    return storage_call(owner, request, size, response, capacity, used, 0,NULL,NULL);
}

int filed_storage_owner_control_call(struct filed_storage_owner *owner,
    const uint8_t *request, size_t size, uint8_t *response, size_t capacity, size_t *used)
{
    return storage_call(owner, request, size, response, capacity, used, 1,NULL,NULL);
}

int filed_storage_owner_call_split(struct filed_storage_owner *owner,
    const uint8_t *request,size_t size,kb2_fs_response_t *response,
    void *data,size_t capacity,size_t *used)
{
    if(!response || (capacity && !data) || capacity>SIZE_MAX-KB2_FILESYSTEM_RESPONSE_SIZE)
        return -EINVAL;
    return storage_call(owner,request,size,NULL,KB2_FILESYSTEM_RESPONSE_SIZE+capacity,
        used,0,data,response);
}
int filed_storage_owner_control_call_split(struct filed_storage_owner *owner,
    const uint8_t *request,size_t size,kb2_fs_response_t *response,
    void *data,size_t capacity,size_t *used)
{
    if(!response || (capacity && !data) || capacity>SIZE_MAX-KB2_FILESYSTEM_RESPONSE_SIZE)
        return -EINVAL;
    return storage_call(owner,request,size,NULL,KB2_FILESYSTEM_RESPONSE_SIZE+capacity,
        used,1,data,response);
}

static int close_queue(struct owner_queue *queue)
{
    int error = 0;
    if (queue->mapping) {
        error = pacha_munmap(queue->mapping, queue->mapping_size);
        if (error) return error;
        queue->mapping = NULL;
    }
    if (queue->vmo_fd >= 16) {
        error = pacha_fd_close(queue->vmo_fd);
        if (error) return error;
        queue->vmo_fd = 0;
    }
    free(queue->slots);
    free(queue->owners);
    queue->slots = NULL;
    queue->owners = NULL;
    return 0;
}

static int wait_process(struct filed_storage_owner *owner)
{
    struct pacha_pollfd event = {.fd = owner->launch.process.fd,
        .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
    return pacha_fd_wait_many(&event, 1, PACHA_FD_WAIT_FOREVER) > 0 ? 0 : -EIO;
}

static int retire_owner(struct filed_storage_owner *owner)
{
    if (owner->launch.process.fd < 16 && owner->launch.thread_fd < 16) return 0;
    uint64_t generation = owner->identity.generation;
    if (!owner->watch.controller) {
        /* No resource was transferred before WATCH. This is the common
         * launch rollback path, not post-delegation process-only teardown. */
        int error;
        while ((error = gpud_native_launch_abort(&owner->launch, generation)) == -EAGAIN) {
            error = wait_process(owner);
            if (error) return error;
        }
        return error;
    }
    owner->started = 0;
    const kb2_action_t *action = kb2_controller_pending_action(owner->controller);
    if (owner->failed) {
        if (action && kb2_action_type(action) == KB2_ACTION_TRANSFER_RESOURCES) {
            if (kb2_controller_complete_action(owner->controller, generation,
                    kb2_action_token(action), KB2_STATUS_HOST_FAILURE, 0, 0) != KB2_STATUS_HOST_FAILURE)
                return -EIO;
        } else if (!action && (kb2_controller_state(owner->controller) == KB2_STATE_RUNNING ||
                kb2_controller_state(owner->controller) == KB2_STATE_HANDSHAKING)) {
            if (kb2_controller_report_fault(owner->controller, generation,
                    KB2_FAULT_PROTOCOL, EPIPE) != KB2_STATUS_OK) return -EIO;
        }
    }
    if (kb2_controller_stop(owner->controller) != KB2_STATUS_OK) return -EIO;
    for (;;) {
        action = kb2_controller_pending_action(owner->controller);
        if (!action) return kb2_controller_state(owner->controller) == KB2_STATE_IDLE ? 0 : -EIO;
        uint64_t token = kb2_action_token(action);
        kb2_action_type_t type = kb2_action_type(action);
        if (type == KB2_ACTION_QUIESCE_SANDBOX) {
            kb2_status_t status = gpud_lifecycle_quiesce(&owner->lifecycle, token);
            if (status == KB2_STATUS_ACTION_PENDING) {
                int error = wait_process(owner);
                if (error) return error;
            } else if (status != KB2_STATUS_OK) {
                owner->failed = 1;
                if (kb2_controller_stop(owner->controller) != KB2_STATUS_OK) return -EIO;
            }
            continue;
        }
        if (type == KB2_ACTION_TERMINATE_SANDBOX || type == KB2_ACTION_REAP_SANDBOX) {
            kb2_status_t status = gpud_process_action(&owner->watch, generation, token, 9);
            if (status == KB2_STATUS_ACTION_PENDING) {
                int error = wait_process(owner);
                if (error) return error;
                continue;
            }
            if (status != KB2_STATUS_OK) return -EIO;
            if (type == KB2_ACTION_REAP_SANDBOX) {
                int error = gpud_native_launch_discard(&owner->launch, generation);
                if (!error) error = gpud_lifecycle_release(&owner->lifecycle);
                if (!error) error = ph_ipc_destroy(&owner->ipc, generation);
                if (error) return error;
            }
            continue;
        }
        if (type == KB2_ACTION_REVOKE_RESOURCES) {
            int error = pacha_capsule_dma_set_enabled((pacha_fd_t)owner->config.device_fd, 0);
            if (error) return error;
            if (owner->data.channel.identity.generation)
                kb2_vq_channel_fault(&owner->data.channel.channel);
            if (owner->credentials.channel.identity.generation)
                kb2_vq_channel_fault(&owner->credentials.channel.channel);
            if (owner->temporary && owner->temporary->channel.identity.generation)
                kb2_vq_channel_fault(&owner->temporary->channel.channel);
        } else if (type == KB2_ACTION_RELEASE_RESOURCES) {
            if (owner->items[1].capability.fd >= 16) {
                int error = pacha_fd_close((pacha_fd_t)owner->items[1].capability.fd);
                if (error) return error;
                owner->items[1] = (struct ph_bootstrap_item){0};
            }
            owner->device.generation = 0;
        } else return -EOPNOTSUPP;
        if (kb2_controller_complete_action(owner->controller, generation, token,
                KB2_STATUS_OK, 0, 0) != KB2_STATUS_OK) return -EIO;
    }
}

int filed_storage_owner_destroy(struct filed_storage_owner *owner)
{
    if (!owner) return 0;
    /* No transparent restart is possible: Linux owns offsets, mounts and the
     * page cache. Keep every backing object pinned until remote retirement. */
    int error = retire_owner(owner);
    if (error) return error;
    error = filed_storage_owner_cancel_shared(owner);
    if (error) return error;
    error = close_queue(&owner->data);
    if (!error) error = close_queue(&owner->credentials);
    if (!error && owner->temporary) error = close_queue(owner->temporary);
    if (error) return error;
    free(owner->temporary);
    owner->temporary = NULL;
    for (size_t i = 0; owner->items && i < owner->artifact_count + 2; ++i) {
        uint64_t fd = owner->items[i].capability.fd;
        if (fd >= 16) {
            error = pacha_fd_close((pacha_fd_t)fd);
            if (error) return error;
            owner->items[i].capability.fd = 0;
        }
    }
    error = ph_bootfs_close(&owner->archive);
    if (error) return error;
    kb2_controller_destroy(owner->controller);
    while (owner->clients) {
        struct owner_client_queue *next = owner->clients->next;
        free(owner->clients);
        owner->clients = next;
    }
    free(owner->items);
    free(owner);
    return 0;
}

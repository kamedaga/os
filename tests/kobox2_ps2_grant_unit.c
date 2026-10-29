/* SPDX-License-Identifier: MIT */
#include "../userland/kobox2_adapter/ps2_native.h"
#include <kobox2/platform_device_layout.h>
#include <kobox2/sha256.h>
#include <pacha/capsule.h>
#include <assert.h>
#include <errno.h>
#include <string.h>

static struct pacha_fd_info fd_info;
static struct pacha_capsule_info capsule_info;
static unsigned queries;

long pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t out)
{
    assert(nr == PACHA_FD_SYSCALL_GET_INFO && fd == 100);
    *(struct pacha_fd_info *)(uintptr_t)out = fd_info;
    return 0;
}
int pacha_capsule_query(pacha_fd_t fd, struct pacha_capsule_info *out)
{
    assert(fd == 100); queries++;
    *out = capsule_info;
    return 0;
}

int main(void)
{
    static uint8_t manifest[4096], grant[4096];
    static struct ph_package package;
    static struct ph_bootstrap_receiver bundle;
    static const uint8_t schema[32] = KB2_PLATFORM_DEVICE_SCHEMA_SHA256_BYTES;
    kb2_closure_manifest_artifact_t artifact = {.node_id = 1,
        .kind = KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER,
        .flags = KB2_CLOSURE_ARTIFACT_FLAG_ROOT | KB2_CLOSURE_ARTIFACT_FLAG_NATIVE_LINUX,
        .content_size = 64, .namespace_name = {.data = "core", .length = 4}};
    memset(artifact.content_digest, 1, 32);
    kb2_closure_manifest_resource_t resource = {.slot_id = 1,
        .type = KB2_CLOSURE_RESOURCE_DEVICE, .minimum_count = 1, .maximum_count = 1,
        .required_rights = 7, .maximum_rights = 7, .flags = KB2_CLOSURE_RESOURCE_FLAG_REQUIRED};
    memcpy(resource.interface_schema_digest, schema, 32);
    kb2_closure_manifest_binding_t binding = {.slot_id = 1, .node_id = 1};
    kb2_closure_manifest_source_t source = {.artifacts = &artifact, .artifact_count = 1,
        .resources = &resource, .resource_count = 1, .bindings = &binding, .binding_count = 1};
    size_t size;
    assert(!kb2_closure_manifest_encode(manifest, sizeof(manifest), &size, &source));
    assert(!kb2_closure_manifest_decode(manifest, size, &package.common.manifest));
    kb2_resource_grant_slot_source_t slot = {.slot_id = 1, .resource_type = KB2_CLOSURE_RESOURCE_DEVICE,
        .state = KB2_RESOURCE_GRANT_SLOT_PRESENT};
    memcpy(slot.interface_schema_digest, schema, 32);
    kb2_resource_grant_object_source_t object = {.slot_id = 1, .object_id = 1, .granted_rights = 7};
    kb2_resource_grant_handle_binding_t handle = {.object_id = 1, .role = 0};
    kb2_resource_grant_source_t grants = {.generation = 7, .slots = &slot, .slot_count = 1,
        .objects = &object, .object_count = 1, .handle_bindings = &handle, .handle_binding_count = 1};
    kb2_sha256(manifest, size, grants.closure_manifest_digest);
    assert(!kb2_resource_grant_encode(grant, sizeof(grant), &size, &grants));
    assert(!kb2_resource_grant_decode(grant, size, &package.common.grant));
    bundle = (struct ph_bootstrap_receiver){.generation = 7, .artifact_count = 1,
        .resource_count = 1, .received = 4, .complete = 1};
    bundle.items[3].capability = (struct pacha_ipc_fd){.fd = 100,
        .rights = PH_PLATFORM_CONTROLLER_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC};
    struct ph_platform_authority authority = {.generation = 7, .object_id = 1,
        .platform_id = 1, .slot_id = 1, .node_id = 1};
    fd_info = (struct pacha_fd_info){.kind = PACHA_FD_KIND_PLATFORM_DEVICE,
        .rights = PH_PLATFORM_CONTROLLER_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC};
    capsule_info = (struct pacha_capsule_info){.kind = PACHA_CAPSULE_KIND_PLATFORM_DEVICE,
        .device = 1, .index = PACHA_PLATFORM_DEVICE_I8042, .rights = PH_PLATFORM_CONTROLLER_RIGHTS,
        .flags = PACHA_PLATFORM_FADT_8042 | PACHA_PLATFORM_CONTROLLER_WRITE};
    fd_info.kind = PACHA_FD_KIND_PROCESS;
    assert(ph_ps2_grant_take(&bundle, &package, &authority) == -EACCES && !queries);
    fd_info.kind = PACHA_FD_KIND_PLATFORM_DEVICE;
    fd_info.rights &= ~PACHA_FD_RIGHT_CONTROLLER_WRITE;
    assert(ph_ps2_grant_take(&bundle, &package, &authority) == -EACCES && !queries);
    fd_info.rights = PH_PLATFORM_CONTROLLER_RIGHTS;
    capsule_info.device = 2;
    assert(ph_ps2_grant_take(&bundle, &package, &authority) == -EACCES);
    assert(bundle.items[3].capability.fd == 100);
    capsule_info.device = 1;
    assert(!ph_ps2_grant_take(&bundle, &package, &authority));
    assert(bundle.items[3].capability.fd == PH_IPC_NO_FD);
    assert(ph_ps2_grant_take(&bundle, &package, &authority) == -EINVAL);
    return 0;
}

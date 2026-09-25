/* SPDX-License-Identifier: MIT */
/* Verified-package output and native syscall oracle, not hardware evidence. */
#include "../userland/kobox2_adapter/device_grant.h"
#include <kobox2/pci_function_layout.h>
#include <kobox2/sha256.h>
#include <pacha/capsule.h>
#include <pacha/syscall.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct pacha_fd_info info;
static struct pacha_capsule_info capsule;
static long info_result, query_result, close_result;
static unsigned int calls;
static struct ph_package package;
static struct ph_bootstrap_receiver bundle;
static struct ph_device_authority authority;
static uint8_t manifest_bytes[4096], grant_bytes[4096];

long pacha_syscall1(uint64_t nr, uint64_t fd) {
    ++calls;
    assert(nr == PACHA_FD_SYSCALL_CLOSE && fd == 100);
    return close_result;
}

long pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t output) {
    ++calls;
    assert(nr == PACHA_FD_SYSCALL_GET_INFO && fd == 100);
    *(struct pacha_fd_info *)(uintptr_t)output = info;
    return info_result;
}

long pacha_syscall3(uint64_t nr, uint64_t fd, uint64_t output, uint64_t words) {
    ++calls;
    assert(nr == PACHA_CAPSULE_SYSCALL_QUERY && fd == 100 && words == 11);
    *(struct pacha_capsule_info *)(uintptr_t)output = capsule;
    return query_result;
}

static void prepare(void) {
    static const struct kobox_package_operations operations = {0};
    static const uint8_t schema[32] = KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES;
    kb2_closure_manifest_artifact_t artifact = {.node_id = 1,
        .kind = KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER,
        .flags = KB2_CLOSURE_ARTIFACT_FLAG_NATIVE_LINUX | KB2_CLOSURE_ARTIFACT_FLAG_ROOT,
        .content_size = 64, .namespace_name = {.data = "core", .length = 4}};
    memset(artifact.content_digest, 1, 32);
    kb2_closure_manifest_resource_t resource = {.slot_id = 1,
        .type = KB2_CLOSURE_RESOURCE_DEVICE, .minimum_count = 1, .maximum_count = 1,
        .required_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
        .maximum_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
        .flags = KB2_CLOSURE_RESOURCE_FLAG_REQUIRED};
    memcpy(resource.interface_schema_digest, schema, 32);
    kb2_closure_manifest_binding_t binding = {.slot_id = 1, .node_id = 1};
    kb2_closure_manifest_source_t source = {.artifacts = &artifact, .artifact_count = 1,
        .resources = &resource, .resource_count = 1, .bindings = &binding, .binding_count = 1};
    size_t size;
    package = (struct ph_package){0};
    assert(kb2_closure_manifest_encode(manifest_bytes, sizeof(manifest_bytes), &size, &source) == KB2_PROTOCOL_OK);
    assert(kb2_closure_manifest_decode(manifest_bytes, size, &package.common.manifest) == KB2_PROTOCOL_OK);
    kb2_resource_grant_slot_source_t slot = {.slot_id = 1, .resource_type = KB2_CLOSURE_RESOURCE_DEVICE,
        .state = KB2_RESOURCE_GRANT_SLOT_PRESENT};
    memcpy(slot.interface_schema_digest, schema, 32);
    kb2_resource_grant_object_source_t object = {.slot_id = 1, .object_id = 23,
        .granted_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS};
    kb2_resource_grant_handle_binding_t handle = {.object_id = 23,
        .role = KB2_PCI_FUNCTION_NATIVE_HANDLE_ROLE_DEVICE, .transfer_handle_index = 0};
    kb2_resource_grant_source_t grant = {.generation = 7, .slots = &slot, .slot_count = 1,
        .objects = &object, .object_count = 1, .handle_bindings = &handle, .handle_binding_count = 1};
    kb2_sha256(manifest_bytes, size, grant.closure_manifest_digest);
    assert(kb2_resource_grant_encode(grant_bytes, sizeof(grant_bytes), &size, &grant) == KB2_PROTOCOL_OK);
    assert(kb2_resource_grant_decode(grant_bytes, size, &package.common.grant) == KB2_PROTOCOL_OK);
    assert(kb2_resource_grant_validate_manifest(&package.common.grant, &package.common.manifest) == KB2_PROTOCOL_OK);
    package.common.operations = &operations;
    package.common.artifact_count = 1;
    bundle = (struct ph_bootstrap_receiver){.generation = 7, .artifact_count = 1,
        .resource_count = 1, .received = 4, .complete = 1};
    bundle.items[3].capability = (struct pacha_ipc_fd){.fd = 100,
        .rights = PH_DEVICE_FUNCTION_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC};
    authority = (struct ph_device_authority){.generation = 7, .object_id = 23,
        .native_device = 42, .slot_id = 1, .node_id = 1};
    info = (struct pacha_fd_info){.rights = PH_DEVICE_FUNCTION_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC};
    capsule = (struct pacha_capsule_info){.fd = 100, .kind = PACHA_CAPSULE_KIND_DEVICE,
        .device = 42, .rights = PH_DEVICE_FUNCTION_RIGHTS, .flags = PACHA_CAPSULE_DMA_TRANSLATED};
    info_result = close_result = 0;
    query_result = 11;
    calls = 0;
}

static void rejected(int error, unsigned int expected_calls) {
    struct ph_device_grant output = {0}, before = output;
    struct ph_bootstrap_receiver staged = bundle;
    assert(ph_device_grant_take(&output, &bundle, &package, &authority) == error);
    assert(!memcmp(&output, &before, sizeof(output)) && !memcmp(&bundle, &staged, sizeof(bundle)));
    assert(calls == expected_calls);
}

int main(void) {
    prepare(); authority.node_id = 2; rejected(-EACCES, 0);
    prepare(); authority.object_id = 24; rejected(-EACCES, 0);
    prepare(); authority.generation = 8; rejected(-ESTALE, 0);
    prepare(); bundle.complete = 0; rejected(-EINVAL, 0);
    prepare(); bundle.resource_count = 0; --bundle.received; rejected(-EINVAL, 0);
    prepare(); bundle.items[3].capability.fd = PH_IPC_NO_FD; rejected(-EACCES, 0);
    prepare(); bundle.items[3].capability.rights |= PACHA_FD_RIGHT_TRANSFER; rejected(-EACCES, 0);
    prepare(); info.rights |= PACHA_FD_RIGHT_TRANSFER; rejected(-EACCES, 1);
    prepare(); info.flags = 0; rejected(-EACCES, 1);
    prepare(); info_result = PACHA_SYSCALL_ERR_NOT_READY; rejected(-EAGAIN, 1);
    prepare(); capsule.device = 43; rejected(-EACCES, 2);
    prepare(); capsule.kind = PACHA_CAPSULE_KIND_IRQ; rejected(-EACCES, 2);
    prepare(); capsule.rights &= ~PACHA_FD_RIGHT_DERIVE_IRQ; rejected(-EACCES, 2);
    prepare(); capsule.flags |= PACHA_CAPSULE_DMA_QUARANTINED; rejected(-ENODEV, 2);
    prepare(); capsule.flags = 0; rejected(-ENODEV, 2);
    prepare(); query_result = 0; rejected(-EPROTO, 2);
    prepare(); query_result = PACHA_SYSCALL_ERR_ALLOC; rejected(-ENOMEM, 2);
    prepare();
    struct ph_device_grant output = {0};
    assert(!ph_device_grant_take(&output, &bundle, &package, &authority));
    assert(output.fd == 100 && output.object_id == 23 && output.native_device == 42 && output.generation == 7);
    assert(bundle.items[3].capability.fd == PH_IPC_NO_FD && calls == 2);
    assert(ph_device_grant_take(&output, &bundle, &package, &authority) == -EBUSY);
    assert(ph_device_grant_close(&output, 6) == -ESTALE && calls == 2);
    close_result = PACHA_SYSCALL_ERR_NOT_READY;
    assert(ph_device_grant_close(&output, 7) == -EAGAIN && output.fd == 100);
    close_result = 0;
    assert(!ph_device_grant_close(&output, 7) && !output.fd && output.generation == 7);
    assert(ph_device_grant_take(&output, &bundle, &package, &authority) == -ESTALE);
    assert(!ph_device_grant_close(&output, 7) && calls == 4);
    puts("kobox2 device grant import: PASS identity rights ownership failure-retention");
    return 0;
}

/* SPDX-License-Identifier: MIT */
#include "device_grant.h"
#include <kobox2/pci_function_layout.h>
#include <pacha/capsule.h>
#include <pacha/ipc.h>
#include <pacha/syscall.h>
#include <errno.h>
#include <string.h>

static int native_error(long result) {
    switch (result) {
    case 0: return 0;
    case PACHA_SYSCALL_ERR_NOT_READY: return -EAGAIN;
    case PACHA_SYSCALL_ERR_CLOSED: return -EBADF;
    case PACHA_SYSCALL_ERR_ALLOC: return -ENOMEM;
    case PACHA_SYSCALL_ERR_INVALID: return -EACCES;
    default: return -EPROTO;
    }
}

static int visible(const kb2_closure_manifest_t *manifest, const struct ph_device_authority *authority) {
    for (size_t i = 0; i < kb2_closure_manifest_binding_count(manifest); ++i) {
        kb2_closure_manifest_binding_t binding;
        if (kb2_closure_manifest_binding(manifest, i, &binding) != KB2_PROTOCOL_OK) return 0;
        if (binding.slot_id == authority->slot_id && binding.node_id == authority->node_id) return 1;
    }
    return 0;
}

static int select_handle(const kb2_resource_grant_t *grant,
    const struct ph_device_authority *authority, uint32_t *index) {
    static const uint8_t schema[32] = KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES;
    for (size_t i = 0; i < kb2_resource_grant_slot_count(grant); ++i) {
        kb2_resource_grant_slot_t slot;
        if (kb2_resource_grant_slot(grant, i, &slot) != KB2_PROTOCOL_OK) return -EINVAL;
        if (slot.slot_id != authority->slot_id) continue;
        if (slot.resource_type != KB2_CLOSURE_RESOURCE_DEVICE ||
            memcmp(slot.interface_schema_digest, schema, sizeof(schema)) ||
            slot.state != KB2_RESOURCE_GRANT_SLOT_PRESENT || slot.object_count != 1) return -EACCES;
        kb2_resource_grant_object_t object;
        if (kb2_resource_grant_object(grant, slot.object_start, &object) != KB2_PROTOCOL_OK) return -EINVAL;
        if (object.object_id != authority->object_id || object.slot_id != authority->slot_id ||
            object.granted_rights != KB2_PCI_FUNCTION_REQUIRED_RIGHTS || object.handle_count != 1) return -EACCES;
        kb2_resource_grant_handle_binding_t binding;
        if (kb2_resource_grant_handle_binding(grant, object.handle_start, &binding) != KB2_PROTOCOL_OK)
            return -EINVAL;
        if (binding.object_id != authority->object_id || binding.role != KB2_PCI_FUNCTION_NATIVE_HANDLE_ROLE_DEVICE)
            return -EACCES;
        *index = binding.transfer_handle_index;
        return 0;
    }
    return -ENOENT;
}

int ph_device_grant_take(struct ph_device_grant *out, struct ph_bootstrap_receiver *bundle,
    const struct ph_package *package, const struct ph_device_authority *authority) {
    if (!out || !bundle || !package || !authority || !authority->generation ||
        !authority->object_id || !authority->native_device || !authority->slot_id || !authority->node_id)
        return -EINVAL;
    if (out->fd) return -EBUSY;
    if (out->generation >= authority->generation || bundle->generation != authority->generation ||
        package->common.grant.generation != authority->generation) return -ESTALE;
    if (!package->common.operations || !bundle->complete || bundle->failed ||
        !bundle->artifact_count || bundle->artifact_count > PH_BOOTSTRAP_MAX_ARTIFACTS ||
        bundle->artifact_count != package->common.artifact_count ||
        bundle->resource_count > PH_BOOTSTRAP_MAX_RESOURCES ||
        bundle->received != 2 + bundle->artifact_count + bundle->resource_count) return -EINVAL;
    if (!visible(&package->common.manifest, authority)) return -EACCES;
    uint32_t index;
    int result = select_handle(&package->common.grant, authority, &index);
    if (result) return result;
    if (index >= bundle->resource_count) return -EINVAL;
    struct ph_bootstrap_item *item = &bundle->items[2 + bundle->artifact_count + index];
    uint64_t fd = item->capability.fd;
    if (fd < 16 || fd >= PACHA_FD_TABLE_LIMIT || item->size ||
        item->capability.rights != PH_DEVICE_FUNCTION_RIGHTS ||
        item->capability.flags != PACHA_FD_FLAG_CLOEXEC || item->capability.transfer_flags) return -EACCES;
    struct pacha_fd_info info = {0};
    result = native_error(pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, fd, (uintptr_t)&info));
    if (result) return result;
    if (info.rights != PH_DEVICE_FUNCTION_RIGHTS || info.flags != PACHA_FD_FLAG_CLOEXEC) return -EACCES;
    struct pacha_capsule_info capsule = {0};
    long words = pacha_syscall3(PACHA_CAPSULE_SYSCALL_QUERY, fd, (uintptr_t)&capsule, 11);
    if (words != 11) return words ? native_error(words) : -EPROTO;
    if (capsule.kind != PACHA_CAPSULE_KIND_DEVICE || capsule.device != authority->native_device ||
        capsule.rights != PH_DEVICE_FUNCTION_RIGHTS) return -EACCES;
    if (capsule.flags & PACHA_CAPSULE_DMA_QUARANTINED || !(capsule.flags & PACHA_CAPSULE_DMA_TRANSLATED))
        return -ENODEV;
    *out = (struct ph_device_grant){.generation = authority->generation, .object_id = authority->object_id,
        .native_device = capsule.device, .fd = (int)fd};
    item->capability.fd = PH_IPC_NO_FD;
    return 0;
}

int ph_device_grant_close(struct ph_device_grant *grant, uint64_t generation) {
    if (!grant || !generation) return -EINVAL;
    if (grant->generation != generation) return -ESTALE;
    if (!grant->fd) return 0;
    int result = native_error(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, grant->fd));
    if (!result) grant->fd = 0;
    return result;
}

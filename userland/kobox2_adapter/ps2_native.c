/* SPDX-License-Identifier: MIT */
#include "ps2_native.h"
#include "host.h"
#include "boot/ps2_port.h"
#include <kobox2/platform_device_layout.h>
#include <pacha/capsule.h>
#include <errno.h>
#include <string.h>

static pacha_fd_t controller_fd = -1;
static struct pacha_capsule_irq ps2_irq;
static pacha_fd_t stop_fd = -1;
static struct ph_task *collector;
static atomic_bool stopping;

int ph_ps2_grant_take(struct ph_bootstrap_receiver *bundle,
    const struct ph_package *package, const struct ph_platform_authority *authority)
{
    static const uint8_t schema[32] = KB2_PLATFORM_DEVICE_SCHEMA_SHA256_BYTES;
    if (!bundle || !package || !authority || controller_fd >= 16 ||
        !bundle->complete || bundle->failed || bundle->resource_count != 1 ||
        authority->generation != bundle->generation || !authority->generation ||
        !authority->platform_id || authority->object_id != 1 ||
        authority->slot_id != 1 || authority->node_id != 1 ||
        package->common.grant.generation != authority->generation ||
        kb2_resource_grant_slot_count(&package->common.grant) != 1)
        return -EINVAL;
    kb2_closure_manifest_resource_t resource;
    kb2_resource_grant_slot_t slot;
    kb2_resource_grant_object_t object;
    kb2_resource_grant_handle_binding_t binding;
    if (kb2_closure_manifest_resource_count(&package->common.manifest) != 1 ||
        kb2_closure_manifest_resource(&package->common.manifest, 0, &resource) ||
        resource.slot_id != 1 || resource.type != KB2_CLOSURE_RESOURCE_DEVICE ||
        resource.minimum_count != 1 || resource.maximum_count != 1 ||
        resource.required_rights != KB2_PLATFORM_DEVICE_REQUIRED_RIGHTS ||
        resource.maximum_rights != KB2_PLATFORM_DEVICE_REQUIRED_RIGHTS ||
        memcmp(resource.interface_schema_digest, schema, 32) ||
        kb2_resource_grant_slot(&package->common.grant, 0, &slot) ||
        slot.slot_id != 1 || slot.object_count != 1 ||
        slot.resource_type != KB2_CLOSURE_RESOURCE_DEVICE || slot.state != KB2_RESOURCE_GRANT_SLOT_PRESENT ||
        memcmp(slot.interface_schema_digest, schema, 32) ||
        kb2_resource_grant_object(&package->common.grant, slot.object_start, &object) ||
        object.object_id != 1 || object.slot_id != 1 || object.handle_count != 1 ||
        object.granted_rights != KB2_PLATFORM_DEVICE_REQUIRED_RIGHTS ||
        kb2_resource_grant_handle_binding(&package->common.grant, object.handle_start, &binding) ||
        binding.object_id != 1 || binding.role != KB2_PLATFORM_DEVICE_NATIVE_HANDLE_ROLE_DEVICE ||
        binding.transfer_handle_index != 0)
        return -EACCES;
    int visible = 0;
    for (size_t i = 0; i < kb2_closure_manifest_binding_count(&package->common.manifest); i++) {
        kb2_closure_manifest_binding_t binding;
        if (kb2_closure_manifest_binding(&package->common.manifest, i, &binding)) return -EINVAL;
        if (binding.slot_id == authority->slot_id && binding.node_id == authority->node_id) visible = 1;
    }
    if (!visible) return -EACCES;
    struct ph_bootstrap_item *item = &bundle->items[2 + bundle->artifact_count];
    if (item->size || item->capability.rights != PH_PLATFORM_CONTROLLER_RIGHTS ||
        item->capability.flags != PACHA_FD_FLAG_CLOEXEC || item->capability.transfer_flags)
        return -EACCES;
    struct pacha_fd_info fd_info;
    if (pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, item->capability.fd, (uintptr_t)&fd_info) ||
        fd_info.kind != PACHA_FD_KIND_PLATFORM_DEVICE || fd_info.rights != PH_PLATFORM_CONTROLLER_RIGHTS ||
        fd_info.flags != PACHA_FD_FLAG_CLOEXEC)
        return -EACCES;
    struct pacha_capsule_info info = {0};
    int result = pacha_capsule_query((pacha_fd_t)item->capability.fd, &info);
    if (result || info.kind != PACHA_CAPSULE_KIND_PLATFORM_DEVICE ||
        info.device != authority->platform_id || info.index != PACHA_PLATFORM_DEVICE_I8042 ||
        !(info.flags & PACHA_PLATFORM_FADT_8042) || !(info.flags & PACHA_PLATFORM_CONTROLLER_WRITE) ||
        info.rights != PH_PLATFORM_CONTROLLER_RIGHTS)
        return result ? result : -EACCES;
    controller_fd = (pacha_fd_t)item->capability.fd;
    item->capability.fd = PH_IPC_NO_FD;
    return 0;
}

static int read_port(void *context, uint16_t port, uint8_t *value)
{
    (void)context;
    uint32_t data;
    int result = pacha_capsule_io_read(controller_fd, port, 1, &data);
    if (!result) *value = (uint8_t)data;
    return result;
}

static int write_port(void *context, uint16_t port, uint8_t value)
{
    (void)context;
    return pacha_capsule_io_write(controller_fd, port, 1, value);
}

static int mask_irq(void *context, int masked)
{
    (void)context;
    return pacha_capsule_irq_set_mask(ps2_irq.fd, (unsigned)masked);
}

static void *collect_irqs(void *context)
{
    (void)context;
    while (!atomic_load_explicit(&stopping, memory_order_acquire)) {
        struct pacha_pollfd fds[2] = {
            {.fd = ps2_irq.fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
            {.fd = stop_fd, .events = PACHA_FD_EVENT_READABLE},
        };
        PH_CHECK(pacha_syscall3(PACHA_FD_SYSCALL_WAIT_MANY,
            (uintptr_t)fds, 2, PACHA_FD_WAIT_FOREVER) > 0);
        if (atomic_load_explicit(&stopping, memory_order_acquire)) break;
        PH_CHECK(!(fds[0].revents & PACHA_FD_EVENT_HANGUP));
        if (fds[0].revents & PACHA_FD_EVENT_READABLE) {
            uint64_t next = ps2_irq.count;
            int result = pacha_capsule_irq_poll(ps2_irq.fd, ps2_irq.count, &next);
            if (!result && next != ps2_irq.count) {
                ps2_irq.count = next;
                PH_OK(ph_task_ops.cpu_notify(0, KOBOX_LINUX_TASK_DEVICE_IRQ));
            } else {
                PH_CHECK(result == -PACHA_SYSCALL_ERR_NOT_READY);
            }
        }
    }
    return NULL;
}

static int quiesce_irq(void *context)
{
    (void)context;
    int result = mask_irq(NULL, 1);
    if (result) return result;
    if (collector) {
        atomic_store_explicit(&stopping, true, memory_order_release);
        const uint64_t one = 1;
        if (pacha_syscall3(PACHA_FD_SYSCALL_WRITE, stop_fd,
                (uintptr_t)&one, sizeof(one)) != sizeof(one)) return -EIO;
        result = ph_thread_join(collector);
        if (result) return result;
        collector = NULL;
    }
    return pacha_capsule_irq_quiesce(ps2_irq.fd);
}

int ph_ps2_prepare(void *core)
{
    if (!core || controller_fd < 16) return -EINVAL;
    int result = pacha_capsule_device_derive_irq(controller_fd, PACHA_CAPSULE_IRQ_AUTO, 0, 0, &ps2_irq);
    if (result) return result;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL;
    stop_fd = (pacha_fd_t)pacha_syscall3(PACHA_FD_SYSCALL_EVENTFD_CREATE,
        0, rights, PACHA_FD_FLAG_NONBLOCK);
    if (stop_fd < 16) return -ENOMEM;
    int (*configure)(const struct kobox_linux_ps2_host *);
    void *symbol = ph_image_lookup(core, "kobox_linux_ps2_configure");
    if (!symbol) return -ENOENT;
    memcpy(&configure, &symbol, sizeof(configure));
    const struct kobox_linux_ps2_host host = {.size = sizeof(host),
        .read = read_port, .write = write_port, .mask = mask_irq, .quiesce = quiesce_irq};
    result = configure(&host);
    if (result) return result;
    collector = ph_thread_create(collect_irqs, NULL);
    return collector ? 0 : -ENOMEM;
}

/* SPDX-License-Identifier: MIT */
#include "kobox2_nic.h"
#include "kobox2_frame_client.h"
#include "kobox2_package.h"
#include "link.h"
#include "netd_internal.h"
#include "status_file.h"
#include "boot/module_launch.h"
#include "../../gpud/bootstrap.h"
#include "../../gpud/launch.h"
#include "../../gpud/lifecycle.h"
#include "../../gpud/process.h"
#include "../../kobox2_adapter/sandbox_config.h"

#include <pacha/capsule.h>
#include <pacha/ipc.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NETD_RESOURCE_SET_ID = 1, NETD_SANDBOX_ID = 1 };

struct nic_generation {
    uint64_t id;
    struct pacha_ipc_channel_pair management;
    struct ph_ipc ipc;
    struct gpud_native_launch launch;
    struct gpud_process_watch watch;
    struct gpud_lifecycle lifecycle;
    struct netd_kobox2_frame_client frames;
};

static struct {
    struct netd_kobox2_package package;
    kb2_controller_t *controller;
    struct nic_generation generation;
    uint8_t initial_mac[6];
    uint32_t initial_mtu;
    int started;
    int failed;
} nic;
static struct netd_nic_diagnostic diagnostic;
static const struct netd_boot_config *status_config;

const struct netd_nic_diagnostic *netd_kobox2_nic_diagnostic(void)
{ return &diagnostic; }

static void nic_progress(unsigned step, const char *phase)
{
    diagnostic.step = step;
    diagnostic.detail = 0;
    netd_status_file_progress(status_config, phase);
}

static int nic_failure(unsigned step, int status, int detail,
    unsigned loaded, unsigned pci_bound)
{
    diagnostic = (struct netd_nic_diagnostic){.step = step,
        .detail = detail, .loaded = loaded, .pci_bound = pci_bound};
    return status;
}

static void nic_poll(void *context);

static void *controller_allocate(void *context, size_t size)
{ (void)context; return malloc(size); }

static void controller_deallocate(void *context, void *pointer, size_t size)
{ (void)context; (void)size; free(pointer); }

static int wait_readable(int fd, int process_fd)
{
    struct pacha_pollfd events[2] = {
        {.fd = fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
        {.fd = process_fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
    };
    long result = pacha_fd_wait_many(events, 2, PACHA_FD_WAIT_FOREVER);
    /* A sandbox can publish a failure packet immediately before exit. Read
     * that packet first when both descriptors become ready together. */
    if (result > 0 && (events[0].revents & PACHA_FD_EVENT_READABLE)) return 0;
    if (events[1].revents) return -EPIPE;
    return -EIO;
}

static int wait_process(int process_fd)
{
    struct pacha_pollfd event = {.fd = process_fd,
        .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
    long result = pacha_fd_wait_many(&event, 1, PACHA_FD_WAIT_FOREVER);
    return result > 0 && event.revents ? 0 : -EIO;
}

static int complete_action(kb2_controller_t *controller,
    kb2_action_type_t type, uint64_t resource, uint64_t sandbox)
{
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    if (!action || kb2_action_type(action) != type) return -EINVAL;
    return kb2_controller_complete_action(controller, kb2_action_generation(action),
        kb2_action_token(action), KB2_STATUS_OK, resource, sandbox) == KB2_STATUS_OK ?
        0 : -EIO;
}

static int nic_send(void *context, const void *frame, size_t length)
{
    struct nic_generation *current = context;
    int result = netd_kobox2_frame_write(&current->frames, &current->ipc,
        current->launch.process.fd, frame, length);
    if (result == -EPIPE) (void)netd_link_set_carrier(current->id, 0);
    return result;
}

static void nic_sandbox_progress(const struct gpud_lifecycle *lifecycle)
{
    const unsigned phase = lifecycle->progress_phase;
    const unsigned index = lifecycle->progress_module_index;
    char message[80];
    switch (phase) {
    case KOBOX_MODULE_PROGRESS_PCI_PREPARE:
        (void)snprintf(message, sizeof(message), "Linux PCI bridge, DMA and IRQ setup");
        break;
    case KOBOX_MODULE_PROGRESS_PCI_READY:
        (void)snprintf(message, sizeof(message), "Linux PCI bridge, DMA and IRQ ready");
        break;
    case KOBOX_MODULE_PROGRESS_MODULE_BEGIN:
    case KOBOX_MODULE_PROGRESS_MODULE_FAILED: {
        kb2_closure_manifest_artifact_t artifact;
        if (index + 1 >= nic.package.artifact_count ||
            kb2_closure_manifest_artifact(&nic.package.decoded, index + 1,
                &artifact) != KB2_PROTOCOL_OK) {
            (void)snprintf(message, sizeof(message),
                "Linux module index=%u phase=%u", index, phase);
            break;
        }
        const int name_length = (int)(artifact.namespace_name.length < 28 ?
            artifact.namespace_name.length : 28);
        if (phase == KOBOX_MODULE_PROGRESS_MODULE_BEGIN)
            (void)snprintf(message, sizeof(message),
                "Linux init_module %u/%zu %.*s",
                index + 1, nic.package.artifact_count - 1,
                name_length, artifact.namespace_name.data);
        else
            (void)snprintf(message, sizeof(message),
                "Linux init_module %u/%zu %.*s failed=%d",
                index + 1, nic.package.artifact_count - 1,
                name_length, artifact.namespace_name.data,
                lifecycle->progress_status);
        break;
    }
    case KOBOX_MODULE_PROGRESS_MODULES_LOADED:
        diagnostic.loaded = index;
        (void)snprintf(message, sizeof(message),
            "Linux modules loaded=%u", index);
        break;
    case KOBOX_MODULE_PROGRESS_PROBE_WAIT:
        (void)snprintf(message, sizeof(message), "Linux wait_for_device_probe");
        break;
    case KOBOX_MODULE_PROGRESS_PROBE_BOUND:
        diagnostic.pci_bound = 1;
        (void)snprintf(message, sizeof(message), "Linux PCI driver bound");
        break;
    case KOBOX_MODULE_PROGRESS_NET_OPEN:
        (void)snprintf(message, sizeof(message), "Linux net port setup begin");
        break;
    case KOBOX_MODULE_PROGRESS_NET_ALLOCATED:
        (void)snprintf(message, sizeof(message), "Linux net frame buffers allocated");
        break;
    case KOBOX_MODULE_PROGRESS_NET_RTNL_WAIT:
        (void)snprintf(message, sizeof(message), "Linux waiting for RTNL lock");
        break;
    case KOBOX_MODULE_PROGRESS_NET_RTNL_HELD:
        (void)snprintf(message, sizeof(message), "Linux RTNL lock acquired");
        break;
    case KOBOX_MODULE_PROGRESS_NET_DEVICE_FOUND:
        (void)snprintf(message, sizeof(message), "Linux PCI net_device found");
        break;
    case KOBOX_MODULE_PROGRESS_NET_DRIVER_OPEN:
        (void)snprintf(message, sizeof(message), "Linux calling NIC driver ndo_open");
        break;
    case KOBOX_MODULE_PROGRESS_NET_DRIVER_OPENED:
        (void)snprintf(message, sizeof(message),
            "Linux NIC driver ndo_open returned=%d",
            lifecycle->progress_status);
        break;
    case KOBOX_MODULE_PROGRESS_NET_DMA_MAPS:
        (void)snprintf(message, sizeof(message),
            "Linux NIC open DMA maps attempted=%u published=%d",
            index, lifecycle->progress_status);
        break;
    case KOBOX_MODULE_PROGRESS_NET_DMA_FAILURE:
        (void)snprintf(message, sizeof(message),
            "Linux NIC open DMA map failures=%u last_error=%d",
            index, lifecycle->progress_status);
        break;
    case KOBOX_MODULE_PROGRESS_NET_DMA_FAILURE_DETAIL:
        (void)snprintf(message, sizeof(message),
            "Linux NIC last DMA failure bytes=%u error=%d",
            index, lifecycle->progress_status);
        break;
    case KOBOX_MODULE_PROGRESS_NET_FREE_PAGES:
        (void)snprintf(message, sizeof(message),
            "Linux RAM free pages before=%d after=%u",
            lifecycle->progress_status, index);
        break;
    case KOBOX_MODULE_PROGRESS_NET_PACKET_ATTACHED:
        (void)snprintf(message, sizeof(message), "Linux Ethernet capture attached");
        break;
    case KOBOX_MODULE_PROGRESS_NET_READY:
        (void)snprintf(message, sizeof(message), "Linux net_device ready");
        break;
    case KOBOX_MODULE_PROGRESS_LIFECYCLE_READY:
        (void)snprintf(message, sizeof(message), "Linux lifecycle READY send");
        break;
    default:
        return;
    }
    nic_progress(NETD_NIC_STEP_LIFECYCLE, message);
}

static int start_generation(void)
{
    struct nic_generation *current = &nic.generation;
    diagnostic = (struct netd_nic_diagnostic){0};
    memset(current, 0, sizeof(*current));
    current->id = kb2_controller_generation(nic.controller);
    current->management = (struct pacha_ipc_channel_pair){.a = -1, .b = -1};
    const uint64_t generation = current->id;
    nic_progress(NETD_NIC_STEP_RESOURCE, "bind PCI resource grant");
    int error = netd_kobox2_package_bind(&nic.package, generation);
    if (!error)
        error = complete_action(nic.controller, KB2_ACTION_ALLOCATE_RESOURCES,
            NETD_RESOURCE_SET_ID, 0);
    if (error) return nic_failure(NETD_NIC_STEP_RESOURCE, error, error, 0, 0);

    const uint64_t channel_rights = PH_IPC_CHANNEL_RIGHTS |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER;
    nic_progress(NETD_NIC_STEP_CHANNEL, "create sandbox management channel");
    error = pacha_ipc_channel_create(&current->management, channel_rights, 0);
    if (!error) error = ph_ipc_init(&current->ipc, current->management.a, generation);
    if (error) return nic_failure(NETD_NIC_STEP_CHANNEL, error, error, 0, 0);
    struct ph_sandbox_config sandbox_config = {.identity = nic.package.identity,
        .artifact_count = nic.package.artifact_count, .resource_count = 1,
        .device = nic.package.device};
    struct gpud_launch_blob blob = {.bytes = &sandbox_config,
        .size = sizeof(sandbox_config), .address = PH_SANDBOX_CONFIG_ADDRESS};
    struct pacha_process_fd_grant grant = {
        .source_fd = (uint64_t)current->management.b,
        .target_fd = PH_SANDBOX_CHANNEL_FD, .rights = PH_IPC_CHANNEL_RIGHTS,
        .flags = PACHA_FD_FLAG_CLOEXEC};
    struct gpud_launch_request request = {.generation = generation,
        .load_bias = UINT64_C(0x40000000), .stack_address = UINT64_C(0x45000000),
        .stack_size = 256 * 1024, .image = nic.package.sandbox,
        .image_size = nic.package.sandbox_size, .blobs = &blob, .blob_count = 1,
        .grants = &grant, .grant_count = 1};
    struct gpud_launch_resources resources = {.generation = generation,
        .resource_set_id = NETD_RESOURCE_SET_ID, .sandbox_id = NETD_SANDBOX_ID,
        .request = &request};
    memcpy(resources.manifest_digest, nic.package.identity.manifest_digest, 32);
    struct gpud_launch_transaction transaction = {0};
    nic_progress(NETD_NIC_STEP_SANDBOX_LAUNCH, "prepare and start sandbox process");
    kb2_status_t step_status = gpud_launch_begin(&transaction, nic.controller,
        &current->launch, &current->ipc, &resources);
    if (step_status == KB2_STATUS_OK) {
        nic_progress(NETD_NIC_STEP_SANDBOX_LAUNCH, "complete sandbox launch action");
        step_status = gpud_launch_step(&transaction);
    }
    if (step_status != KB2_STATUS_OK || !transaction.completed)
        return nic_failure(NETD_NIC_STEP_SANDBOX_LAUNCH, -EIO,
            transaction.launch_error ? transaction.launch_error : (int)step_status, 0, 0);
    (void)pacha_fd_close(current->management.b);
    current->management.b = -1;
    nic_progress(NETD_NIC_STEP_PROCESS_WATCH, "register sandbox process watch");
    if (gpud_process_watch_init(&current->watch, nic.controller,
            &current->launch.process, &current->ipc,
            NETD_RESOURCE_SET_ID, NETD_SANDBOX_ID) != KB2_STATUS_OK)
        return nic_failure(NETD_NIC_STEP_PROCESS_WATCH, -EIO, -EIO, 0, 0);
    struct gpud_bootstrap_resources transfer = {.generation = generation,
        .resource_set_id = NETD_RESOURCE_SET_ID, .sandbox_id = NETD_SANDBOX_ID,
        .items = nic.package.items, .artifact_count = nic.package.artifact_count,
        .resource_handle_count = 1};
    memcpy(transfer.manifest_digest, nic.package.identity.manifest_digest, 32);
    struct gpud_bootstrap_transfer bootstrap = {0};
    nic_progress(NETD_NIC_STEP_TRANSFER, "prepare sandbox module transfer");
    step_status = gpud_bootstrap_begin(&bootstrap, nic.controller,
        &current->ipc, &transfer);
    if (step_status != KB2_STATUS_OK)
        return nic_failure(NETD_NIC_STEP_TRANSFER, -EIO, (int)step_status, 0, 0);
    nic_progress(NETD_NIC_STEP_TRANSFER, "send sandbox modules and PCI handle");
    while (!bootstrap.completed) {
        kb2_status_t status = gpud_bootstrap_step(&bootstrap);
        if (status != KB2_STATUS_OK && status != KB2_STATUS_ACTION_PENDING)
            return nic_failure(NETD_NIC_STEP_TRANSFER, -EIO,
                bootstrap.native_error ? bootstrap.native_error : (int)status, 0, 0);
    }
    nic_progress(NETD_NIC_STEP_LIFECYCLE, "initialize Linux module lifecycle");
    step_status = gpud_lifecycle_init(&current->lifecycle, &current->watch);
    if (step_status != KB2_STATUS_OK)
        return nic_failure(NETD_NIC_STEP_LIFECYCLE, -EIO, (int)step_status, 0, 0);
    nic_progress(NETD_NIC_STEP_LIFECYCLE, "wait for sandbox Linux modules and NIC probe");
    unsigned observed_progress = 0;
    for (;;) {
        kb2_status_t status = gpud_lifecycle_ready(&current->lifecycle);
        if (status == KB2_STATUS_OK) break;
        if (status != KB2_STATUS_ACTION_PENDING) {
            int failure = nic_failure(NETD_NIC_STEP_LIFECYCLE, -EIO,
                current->lifecycle.native_error ?
                    current->lifecycle.native_error : (int)status,
                current->lifecycle.failure_loaded,
                current->lifecycle.failure_pci_bound);
            diagnostic.source = current->lifecycle.failure_source;
            diagnostic.line = current->lifecycle.failure_line;
            diagnostic.fault = current->lifecycle.failure_fault;
            diagnostic.fault_vector = current->lifecycle.failure_vector;
            diagnostic.fault_error_code = current->lifecycle.failure_error_code;
            diagnostic.fault_core_relative = current->lifecycle.failure_core_relative;
            diagnostic.fault_ip = current->lifecycle.failure_ip;
            diagnostic.fault_address = current->lifecycle.failure_address;
            return failure;
        }
        if (current->lifecycle.progress_count != observed_progress) {
            observed_progress = current->lifecycle.progress_count;
            nic_sandbox_progress(&current->lifecycle);
            continue;
        }
        int waited = wait_readable(current->ipc.fd, current->launch.process.fd);
        if (waited)
            return nic_failure(NETD_NIC_STEP_LIFECYCLE, -EIO,
                waited, 0, 0);
    }
    nic_progress(NETD_NIC_STEP_FRAME_ATTACH, "attach shared network frame transport");
    error = netd_kobox2_frame_open(&current->frames, &current->ipc,
        current->launch.process.fd, generation);
    if (error) return nic_failure(NETD_NIC_STEP_FRAME_ATTACH, error, error, 0, 0);
    struct kobox_linux_net_info info = {0};
    nic_progress(NETD_NIC_STEP_FRAME_INFO, "read NIC MAC, MTU and carrier");
    error = netd_kobox2_frame_info(&current->frames, &current->ipc,
        current->launch.process.fd, &info);
    if (error) return nic_failure(NETD_NIC_STEP_FRAME_INFO, error, error, 0, 0);
    /* libuinet is initialized once per netd process. A replacement driver
     * generation must retain the interface identity that stack owns. */
    if (generation == 1) {
        memcpy(nic.initial_mac, info.mac, sizeof(nic.initial_mac));
        nic.initial_mtu = info.mtu;
    } else if (memcmp(nic.initial_mac, info.mac, sizeof(nic.initial_mac)) ||
               nic.initial_mtu != info.mtu) {
        return nic_failure(NETD_NIC_STEP_LINK_ATTACH, -EXDEV, -EXDEV, 0, 0);
    }
    struct netd_link_info link_info = {.generation = generation,
        .interface_id = NETD_LINK_ID_PRIMARY, .mtu = info.mtu,
        .carrier = info.carrier};
    memcpy(link_info.mac, info.mac, sizeof(link_info.mac));
    const struct netd_link_ops ops = {.send = nic_send, .poll = nic_poll};
    nic_progress(NETD_NIC_STEP_LINK_ATTACH, "publish NIC to netd link layer");
    error = netd_link_attach(&link_info, &ops, current);
    if (!error) {
        printf("[netd] nic ready generation=%llu mtu=%u carrier=%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
            (unsigned long long)generation, info.mtu, info.carrier,
            info.mac[0], info.mac[1], info.mac[2], info.mac[3], info.mac[4], info.mac[5]);
        fflush(stdout);
    }
    return error ? nic_failure(NETD_NIC_STEP_LINK_ATTACH, error, error, 0, 0) : 0;
}

static int restart_generation(void)
{
    struct nic_generation *current = &nic.generation;
    const uint64_t generation = current->id;
    (void)netd_link_detach(generation);
    kb2_status_t observed = gpud_process_observe(&current->watch, generation);
    if (observed == KB2_STATUS_ACTION_PENDING) {
        if (kb2_controller_report_fault(nic.controller, generation,
                KB2_FAULT_PROTOCOL, EPIPE) != KB2_STATUS_OK) return -EIO;
    } else if (observed != KB2_STATUS_OK) return -EIO;
    if (kb2_controller_restart(nic.controller) != KB2_STATUS_OK) return -EIO;
    for (;;) {
        const kb2_action_t *action = kb2_controller_pending_action(nic.controller);
        if (!action) return -EIO;
        const kb2_action_type_t type = kb2_action_type(action);
        if (type == KB2_ACTION_ALLOCATE_RESOURCES)
            return kb2_controller_generation(nic.controller) == generation + 1 ? 0 : -EIO;
        if (type == KB2_ACTION_TERMINATE_SANDBOX || type == KB2_ACTION_REAP_SANDBOX) {
            kb2_status_t status = gpud_process_action(&current->watch,
                generation, kb2_action_token(action), 9);
            if (status == KB2_STATUS_ACTION_PENDING) {
                int error = wait_process(current->launch.process.fd);
                if (error) return error;
                continue;
            }
            if (status != KB2_STATUS_OK) return -EIO;
            if (type == KB2_ACTION_REAP_SANDBOX) {
                int error = gpud_native_launch_discard(&current->launch, generation);
                if (!error) error = gpud_lifecycle_release(&current->lifecycle);
                if (!error) error = netd_kobox2_frame_close(&current->frames);
                if (!error) error = ph_ipc_destroy(&current->ipc, generation);
                if (error) return error;
            }
            continue;
        }
        if (type == KB2_ACTION_REVOKE_RESOURCES) {
            int error = pacha_capsule_dma_set_enabled(nic.package.device_fd, 0);
            if (error || complete_action(nic.controller, type, 0, 0))
                return error ? error : -EIO;
            continue;
        }
        if (type == KB2_ACTION_RELEASE_RESOURCES) {
            int error = netd_kobox2_package_unbind(&nic.package, generation);
            if (error || complete_action(nic.controller, type, 0, 0))
                return error ? error : -EIO;
            continue;
        }
        return -EOPNOTSUPP;
    }
}

static int sandbox_exited(int process_fd)
{
    struct pacha_pollfd event = {.fd = process_fd,
        .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
    long result = pacha_fd_wait_many(&event, 1, 0);
    return result > 0 && event.revents;
}

static void nic_poll(void *context)
{
    (void)context;
    if (!nic.started || nic.failed) return;
    struct nic_generation *current = &nic.generation;
    if (sandbox_exited(current->launch.process.fd)) {
        int error = restart_generation();
        if (!error) error = start_generation();
        if (error) {
            nic.failed = 1;
            fprintf(stderr, "[netd] nic restart failed status=%d\n", error);
        }
        return;
    }
    struct kobox_linux_net_info info = {0};
    int error = netd_kobox2_frame_info(&current->frames, &current->ipc,
        current->launch.process.fd, &info);
    if (error) {
        (void)netd_link_set_carrier(current->id, 0);
        return;
    }
    if (memcmp(nic.initial_mac, info.mac, sizeof(nic.initial_mac)) ||
        nic.initial_mtu != info.mtu) {
        (void)netd_link_set_carrier(current->id, 0);
        nic.failed = 1;
        fprintf(stderr, "[netd] NIC identity changed within generation\n");
        return;
    }
    (void)netd_link_set_carrier(current->id, info.carrier);
    for (unsigned round = 0; round < 4; ++round) {
        const struct kobox_linux_net_frame *frames = NULL;
        size_t count = 0;
        error = netd_kobox2_frame_read(&current->frames, &current->ipc,
            current->launch.process.fd, &frames, &count);
        if (error) break;
        for (size_t i = 0; i < count; ++i)
            (void)netd_link_receive(current->id, frames[i].bytes, frames[i].length);
        if (count < PH_NET_FRAME_BATCH) break;
    }
}

int netd_kobox2_nic_start(struct netd_runtime *runtime)
{
    if (!runtime || !runtime->cfg || nic.started) return -EINVAL;
    status_config = runtime->cfg;
    diagnostic = (struct netd_nic_diagnostic){0};
    nic_progress(NETD_NIC_STEP_PACKAGE, "validate PCI capsule and load Kobox2 package");
    int error = netd_kobox2_package_open(&nic.package,
        (int)runtime->cfg->filed_endpoint_fd, (int)runtime->cfg->device_fd);
    if (error) return nic_failure(NETD_NIC_STEP_PACKAGE, error, error, 0, 0);
    nic_progress(NETD_NIC_STEP_CONTROLLER, "create and configure Kobox2 controller");
    kb2_status_t status = kb2_controller_create(controller_allocate,
        controller_deallocate, NULL, &nic.controller);
    if (status != KB2_STATUS_OK)
        return nic_failure(NETD_NIC_STEP_CONTROLLER, -EIO, (int)status, 0, 0);
    error = netd_kobox2_package_configure_controller(&nic.package, nic.controller);
    if (error)
        return nic_failure(NETD_NIC_STEP_CONTROLLER, error, error, 0, 0);
    nic_progress(NETD_NIC_STEP_CONTROLLER, "start Kobox2 controller");
    status = kb2_controller_start(nic.controller);
    if (status != KB2_STATUS_OK)
        return nic_failure(NETD_NIC_STEP_CONTROLLER, -EIO, (int)status, 0, 0);
    error = start_generation();
    if (error) return error;
    nic.started = 1;
    /* Polling is owned by netd's event loop and timer, never by a second
     * thread touching libuinet or the Kobox2 management channel. */
    return 0;
}

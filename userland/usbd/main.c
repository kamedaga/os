/* SPDX-License-Identifier: MIT */
#include <usbd/boot_config.h>
#include "package.h"
#include "input_bridge.h"
#include "source_client.h"
#include "../gpud/bootstrap.h"
#include "../gpud/launch.h"
#include "../gpud/lifecycle.h"
#include "../gpud/process.h"
#include "../kobox2_adapter/sandbox_config.h"

#include <pacha/bootstrap.h>
#include <pacha/capsule.h>
#include <pacha/ipc.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { USBD_RESOURCE_SET_ID = 1, USBD_SANDBOX_ID = 1 };

/* These gpud-prefixed helpers are Kobox2 controller/process/IPC plumbing;
 * no GPU or DRM service source is linked into usbd. */
static void *controller_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void controller_deallocate(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

static int wait_readable(int fd, int process_fd) {
    struct pacha_pollfd events[2] = {
        {.fd = fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
        {.fd = process_fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
    };
    long result = pacha_fd_wait_many(events, 2, PACHA_FD_WAIT_FOREVER);
    if (events[1].revents)
        return -EPIPE;
    return result > 0 && events[0].revents == PACHA_FD_EVENT_READABLE ? 0 : -EIO;
}

static int wait_process(int process_fd) {
    struct pacha_pollfd event = {.fd = process_fd,
        .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
    long result = pacha_fd_wait_many(&event, 1, PACHA_FD_WAIT_FOREVER);
    return result > 0 && event.revents ? 0 : -EIO;
}

static int wait_process_or_tick(int process_fd, int timer_fd) {
    struct pacha_pollfd events[2] = {
        {.fd = process_fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
        {.fd = timer_fd, .events = PACHA_FD_EVENT_READABLE},
    };
    long result = pacha_fd_wait_many(events, 2, PACHA_FD_WAIT_FOREVER);
    if (result <= 0) return -EIO;
    if (events[0].revents) return 0;
    if (events[1].revents != PACHA_FD_EVENT_READABLE) return -EIO;
    uint64_t expirations = 0;
    return pacha_fd_read(timer_fd, &expirations, sizeof(expirations)) ==
        (long)sizeof(expirations) ? 1 : -EIO;
}

static int send_ready(const struct usbd_boot_config *config, int status) {
    const struct pacha_ipc_msg message = {.word0 = USBD_BOOT_READY_MAGIC,
        .word1 = (uint64_t)(int64_t)status, .word2 = status ? 0 : 1};
    return pacha_ipc_send((int)config->ready_channel_fd, &message);
}

static int complete_action(kb2_controller_t *controller,
    kb2_action_type_t type, uint64_t resource, uint64_t sandbox) {
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    if (!action || kb2_action_type(action) != type)
        return -EINVAL;
    return kb2_controller_complete_action(controller, kb2_action_generation(action),
        kb2_action_token(action), KB2_STATUS_OK, resource, sandbox) == KB2_STATUS_OK ? 0 : -EIO;
}

struct usbd_generation {
    uint64_t id;
    struct pacha_ipc_channel_pair management;
    struct ph_ipc ipc;
    struct gpud_native_launch launch;
    struct gpud_process_watch watch;
    struct gpud_lifecycle lifecycle;
    struct usbd_input_bridge input;
    struct usbd_source_client source;
};

static int start_generation(kb2_controller_t *controller, struct usbd_package *package,
    struct usbd_generation *current, const struct usbd_boot_config *config) {
    memset(current, 0, sizeof(*current));
    current->id = kb2_controller_generation(controller);
    current->management = (struct pacha_ipc_channel_pair){.a = -1, .b = -1};
    const uint64_t generation = current->id;
    int error = usbd_package_bind(package, generation);
    if (!error)
        error = complete_action(controller, KB2_ACTION_ALLOCATE_RESOURCES,
            USBD_RESOURCE_SET_ID, 0);
    if (error)
        return error;

    const uint64_t channel_rights = PH_IPC_CHANNEL_RIGHTS |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER;
    error = pacha_ipc_channel_create(&current->management, channel_rights, 0);
    if (!error)
        error = ph_ipc_init(&current->ipc, current->management.a, generation);
    if (error)
        return error;
    struct ph_sandbox_config sandbox_config = {.identity = package->identity,
        .artifact_count = USBD_PACKAGE_ARTIFACTS, .resource_count = 1,
        .device = package->device};
    struct gpud_launch_blob blob = {.bytes = &sandbox_config,
        .size = sizeof(sandbox_config), .address = PH_SANDBOX_CONFIG_ADDRESS};
    struct pacha_process_fd_grant grant = {
        .source_fd = (uint64_t)current->management.b,
        .target_fd = PH_SANDBOX_CHANNEL_FD, .rights = PH_IPC_CHANNEL_RIGHTS,
        .flags = PACHA_FD_FLAG_CLOEXEC};
    struct gpud_launch_request request = {.generation = generation,
        .load_bias = UINT64_C(0x40000000), .stack_address = UINT64_C(0x45000000),
        .stack_size = 256 * 1024, .image = package->sandbox,
        .image_size = package->sandbox_size, .blobs = &blob, .blob_count = 1,
        .grants = &grant, .grant_count = 1};
    struct gpud_launch_resources resources = {.generation = generation,
        .resource_set_id = USBD_RESOURCE_SET_ID, .sandbox_id = USBD_SANDBOX_ID,
        .request = &request};
    memcpy(resources.manifest_digest, package->identity.manifest_digest, 32);
    struct gpud_launch_transaction transaction = {0};
    if (gpud_launch_begin(&transaction, controller, &current->launch,
            &current->ipc, &resources) != KB2_STATUS_OK ||
        gpud_launch_step(&transaction) != KB2_STATUS_OK || !transaction.completed)
        return -EIO;
    (void)pacha_fd_close(current->management.b);
    current->management.b = -1;
    if (gpud_process_watch_init(&current->watch, controller,
            &current->launch.process, &current->ipc,
            USBD_RESOURCE_SET_ID, USBD_SANDBOX_ID) != KB2_STATUS_OK)
        return -EIO;
    struct gpud_bootstrap_resources transfer = {.generation = generation,
        .resource_set_id = USBD_RESOURCE_SET_ID, .sandbox_id = USBD_SANDBOX_ID,
        .items = package->items, .artifact_count = USBD_PACKAGE_ARTIFACTS,
        .resource_handle_count = 1};
    memcpy(transfer.manifest_digest, package->identity.manifest_digest, 32);
    struct gpud_bootstrap_transfer bootstrap = {0};
    if (gpud_bootstrap_begin(&bootstrap, controller, &current->ipc,
            &transfer) != KB2_STATUS_OK)
        return -EIO;
    while (!bootstrap.completed) {
        kb2_status_t status = gpud_bootstrap_step(&bootstrap);
        if (status != KB2_STATUS_OK && status != KB2_STATUS_ACTION_PENDING)
            return -EIO;
    }
    if (gpud_lifecycle_init(&current->lifecycle, &current->watch) != KB2_STATUS_OK)
        return -EIO;
    for (;;) {
        kb2_status_t status = gpud_lifecycle_ready(&current->lifecycle);
        if (status == KB2_STATUS_OK) {
            int connected = usbd_input_bridge_open(&current->input, &current->ipc,
                current->launch.process.fd, generation);
            if (!connected)
                connected = usbd_source_open(&current->source,
                    (int)config->input_source_endpoint_fd, config->resource_id,
                    generation, config->pci_segment, config->pci_bus,
                    config->pci_device, config->pci_function);
            if (!connected)
                connected = usbd_source_sync(&current->source,
                    &current->input, &current->ipc,
                    current->launch.process.fd, 0);
            return connected;
        }
        if (status != KB2_STATUS_ACTION_PENDING ||
            wait_readable(current->ipc.fd, current->launch.process.fd))
            return -EIO;
    }
}

static int restart_generation(kb2_controller_t *controller,
    struct usbd_package *package, struct usbd_generation *current) {
    const uint64_t generation = current->id;
    kb2_status_t observed = gpud_process_observe(&current->watch, generation);
    if (observed == KB2_STATUS_ACTION_PENDING) {
        kb2_status_t fault = kb2_controller_report_fault(controller, generation,
            KB2_FAULT_PROTOCOL, EPIPE);
        if (fault != KB2_STATUS_OK)
            return -EIO;
    } else if (observed != KB2_STATUS_OK) {
        return -EIO;
    }
    if (kb2_controller_restart(controller) != KB2_STATUS_OK)
        return -EIO;
    for (;;) {
        const kb2_action_t *action = kb2_controller_pending_action(controller);
        if (!action)
            return -EIO;
        const kb2_action_type_t type = kb2_action_type(action);
        if (type == KB2_ACTION_ALLOCATE_RESOURCES)
            return kb2_controller_generation(controller) == generation + 1 ? 0 : -EIO;
        if (type == KB2_ACTION_TERMINATE_SANDBOX || type == KB2_ACTION_REAP_SANDBOX) {
            kb2_status_t status = gpud_process_action(
                &current->watch, generation, kb2_action_token(action), 9);
            if (status == KB2_STATUS_ACTION_PENDING) {
                int error = wait_process(current->launch.process.fd);
                if (error)
                    return error;
                continue;
            }
            if (status != KB2_STATUS_OK)
                return -EIO;
            if (type == KB2_ACTION_REAP_SANDBOX) {
                int error = gpud_native_launch_discard(&current->launch, generation);
                if (!error)
                    error = gpud_lifecycle_release(&current->lifecycle);
                if (!error)
                    error = usbd_input_bridge_close(&current->input);
                if (!error)
                    error = usbd_source_close(&current->source);
                if (!error)
                    error = ph_ipc_destroy(&current->ipc, generation);
                if (error)
                    return error;
            }
            continue;
        }
        if (type == KB2_ACTION_REVOKE_RESOURCES) {
            int error = pacha_capsule_dma_set_enabled(package->device_fd, 0);
            if (error)
                return error;
            if (complete_action(controller, type, 0, 0))
                return -EIO;
            continue;
        }
        if (type == KB2_ACTION_RELEASE_RESOURCES) {
            int error = usbd_package_unbind(package, generation);
            if (error)
                return error;
            if (complete_action(controller, type, 0, 0))
                return -EIO;
            continue;
        }
        return -EOPNOTSUPP;
    }
}

static int run(const struct usbd_boot_config *config) {
    struct usbd_package package = {0};
    int error = usbd_package_open(&package,
        (int)config->filed_endpoint_fd, (int)config->device_fd);
    if (error) {
        printf("[usbd] stage=package-open status=%d\n", error);
        return error;
    }
    kb2_controller_t *controller = NULL;
    if (kb2_controller_create(controller_allocate, controller_deallocate, NULL,
            &controller) != KB2_STATUS_OK ||
        (error = usbd_package_configure_controller(&package, controller)) ||
        kb2_controller_start(controller) != KB2_STATUS_OK)
        return error ? error : -EIO;
    for (;;) {
        static struct usbd_generation current;
        error = start_generation(controller, &package, &current, config);
        if (error) {
            printf("[usbd] stage=sandbox-ready status=%d\n", error);
            return error;
        }
        if (current.id == 1 && (error = send_ready(config, 0)))
            return error;
        printf("[usbd] xhci ready generation=%llu modules=%u\n",
            (unsigned long long)current.id, USBD_PACKAGE_ARTIFACTS - 1);
        fflush(stdout);
        const uint64_t timer_rights = PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WAIT |
            PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
        int timer_fd = pacha_timerfd_create(UINT64_C(10000000),
            UINT64_C(10000000), timer_rights, 0);
        if (timer_fd < 16) return timer_fd < 0 ? timer_fd : -EIO;
        for (;;) {
            int wake = wait_process_or_tick(current.launch.process.fd, timer_fd);
            if (wake < 0) { error = wake; break; }
            if (wake == 0) break;
            error = usbd_source_pump(&current.source,
                &current.input, &current.ipc, current.launch.process.fd);
            if (error) break;
        }
        (void)pacha_fd_close(timer_fd);
        if (error) return error;
        error = restart_generation(controller, &package, &current);
        if (error)
            return error;
    }
}

int main(int argc, char **argv) {
    (void)argc;
    struct usbd_boot_config config = {0};
    int bootstrap_fd = pacha_bootstrap_fd_from_argv(argv);
    if (bootstrap_fd < 16 ||
        pacha_fd_read(bootstrap_fd, &config, sizeof(config)) != (long)sizeof(config) ||
        config.magic != USBD_BOOT_CONFIG_MAGIC ||
        config.version != USBD_BOOT_CONFIG_VERSION ||
        config.device_fd < 16 || config.filed_endpoint_fd < 16 ||
        config.ready_channel_fd < 16 || config.input_source_endpoint_fd < 16 ||
        !config.resource_id || config.pci_device > 31 || config.pci_function > 7)
        return 1;
    int status = run(&config);
    fprintf(stderr, "[usbd] stopped status=%d\n", status);
    (void)send_ready(&config, status);
    return 1;
}

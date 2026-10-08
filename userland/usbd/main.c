/* SPDX-License-Identifier: MIT */
#include <usbd/boot_config.h>
#include "package.h"
#include "input_bridge.h"
#include "source_client.h"
#include "media_probe.h"
#include "install_port.h"
#include "../kobox2_adapter/block_client.h"
#include "../kobox2_adapter/block_policy.h"
#include "../gpud/bootstrap.h"
#include "../gpud/launch.h"
#include "../kobox2_owner/boot.h"
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

/* Independent capabilities keep storage maintenance out of input cadence.
 * Leave lower-priority ready FDs undrained: the next wait sees them again. */
static int wait_process_or_tick(int process_fd, int timer_fd,
    int installer_fd, int input_fd) {
    struct pacha_pollfd events[4] = {
        {.fd = process_fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
        {.fd = timer_fd, .events = PACHA_FD_EVENT_READABLE},
        {.fd = installer_fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
        {.fd = input_fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
    };
    size_t count = 2;
    const size_t installer_index = count;
    if (installer_fd >= 16) events[count++] = events[2];
    const size_t input_index = count;
    if (input_fd >= 16) events[count++] = events[3];
    long result = pacha_fd_wait_many(events, count,
        PACHA_FD_WAIT_FOREVER);
    if (result <= 0) return -EIO;
    if (events[0].revents) return 0;
    if (installer_fd >= 16 && events[installer_index].revents)
        return events[installer_index].revents == PACHA_FD_EVENT_READABLE ? 2 : -EPIPE;
    if (!events[1].revents && input_fd >= 16 && events[input_index].revents) {
        uint64_t notifications = 0;
        if (events[input_index].revents != PACHA_FD_EVENT_READABLE) return -EPIPE;
        return pacha_fd_read(input_fd, &notifications, sizeof(notifications)) ==
            (long)sizeof(notifications) ? 3 : -EIO;
    }
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
    struct ph_block_client block;
    kb2_block_disk_t disks[KB2_BLOCK_DEVICE_MAX_DISKS];
    size_t disk_count;
};

static int start_generation(kb2_controller_t *controller, struct usbd_package *package,
    struct usbd_generation *current, const struct usbd_boot_config *config,
    const kb2_block_disk_t *write_disk) {
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

    struct ph_sandbox_config sandbox_config = {.size = sizeof(struct ph_sandbox_config),
        .identity = package->identity,
        .artifact_count = package->artifact_count, .resource_count = 1,
        .device = package->device};
    const struct ph_block_write_policy block_policy = {
        .magic = PH_BLOCK_POLICY_MAGIC,
        .generation = generation,
        .native_device = package->device.native_device,
        .enabled = write_disk != NULL,
        .diskseq = write_disk ? write_disk->diskseq : 0,
        .major = write_disk ? write_disk->major : 0,
        .minor = write_disk ? write_disk->minor : 0,
    };
    const struct gpud_launch_blob blobs[] = {
        {.bytes = &block_policy, .size = sizeof(block_policy),
         .address = PH_BLOCK_POLICY_ADDRESS},
    };
    const struct ph_owner_boot_config start = {.controller = controller,
        .launch = &current->launch, .ipc = &current->ipc,
        .management = &current->management, .watch = &current->watch,
        .lifecycle = &current->lifecycle,
        .resource_set_id = USBD_RESOURCE_SET_ID, .sandbox_id = USBD_SANDBOX_ID,
        .sandbox_config = &sandbox_config, .image = package->sandbox,
        .image_size = package->sandbox_size, .extra_blobs = blobs, .extra_blob_count = 1,
        .items = package->items, .artifact_count = package->artifact_count,
        .resource_count = 1, .log_endpoint_fd = -1};
    struct ph_owner_boot boot = {0};
    if (ph_owner_boot_begin(&boot, &start) != KB2_STATUS_OK)
        return -EIO;
    while (boot.phase != PH_OWNER_BOOT_COMPLETE) {
        kb2_status_t status = ph_owner_boot_step(&boot);
        if (status != KB2_STATUS_OK && status != KB2_STATUS_ACTION_PENDING)
            return -EIO;
    }
    for (;;) {
        kb2_status_t status = gpud_lifecycle_ready(&current->lifecycle);
        if (status == KB2_STATUS_OK) {
            int connected = 0;
            if (config->kind == USBD_KIND_USB_XHCI) {
                connected = usbd_input_bridge_open(&current->input, &current->ipc,
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
            }
            if (!connected)
                connected = ph_block_client_open(&current->block,
                    &current->ipc, current->launch.process.fd, generation);
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
                if (!error && current->input.shared)
                    error = usbd_input_bridge_close(&current->input);
                if (!error && current->source.endpoint_fd >= 16)
                    error = usbd_source_close(&current->source);
                if (!error)
                    error = ph_block_client_close(&current->block);
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
        (int)config->filed_endpoint_fd, (int)config->device_fd, config->kind);
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
    struct usbd_install_port install_port = {
        .fd = config->installer_channel_fd >= 16 ?
            (int)config->installer_channel_fd : -1,
        .shared_fd = -1};
    kb2_block_disk_t write_disk = {0};
    for (;;) {
        static struct usbd_generation current;
        error = start_generation(controller, &package, &current, config,
            write_disk.diskseq ? &write_disk : NULL);
        if (error) {
            printf("[usbd] stage=sandbox-ready status=%d\n", error);
            return error;
        }
        if (current.id == 1 && (error = send_ready(config, 0)))
            return error;
        printf("[usbd] %s ready generation=%llu modules=%u\n",
            config->kind == USBD_KIND_NVME ? "nvme" : "xhci",
            (unsigned long long)current.id, (unsigned)(package.artifact_count - 1));
        fflush(stdout);
        const uint64_t timer_rights = PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WAIT |
            PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
        int timer_fd = pacha_timerfd_create(UINT64_C(1000000000),
            UINT64_C(1000000000), timer_rights, 0);
        if (timer_fd < 16) return timer_fd < 0 ? timer_fd : -EIO;
        if (config->kind == USBD_KIND_USB_XHCI) {
            printf("[usbd] input transport=event-driven frames=preserved\n");
            fflush(stdout);
        }
        for (;;) {
            int wake = wait_process_or_tick(current.launch.process.fd,
                timer_fd, install_port.fd, current.input.event_fd);
            if (wake == -EPIPE && install_port.fd >= 16) {
                /* Loss of the only installer control channel revokes the
                 * launch-time key before this controller can be reused. */
                usbd_install_port_close(&install_port);
                write_disk = (kb2_block_disk_t){0};
                error = 0;
                break;
            }
            if (wake < 0) { error = wake; break; }
            if (wake == 0) break;
            if (wake == 2) {
                int restart_requested = 0;
                error = usbd_install_port_pump(&install_port,
                    &current.block, &current.ipc, current.launch.process.fd,
                    config->resource_id, &write_disk, &restart_requested);
                if (error || restart_requested) break;
                continue;
            }
            if (wake == 3) {
                error = usbd_source_pump(&current.source,
                    &current.input, &current.ipc, current.launch.process.fd);
                if (error) break;
                continue;
            }
            if (wake == 1) {
                size_t count = 0;
                uint64_t next = 0;
                error = ph_block_client_snapshot(&current.block, &current.ipc,
                    current.launch.process.fd, 0, current.disks,
                    KB2_BLOCK_DEVICE_MAX_DISKS, &count, &next);
                if (error) break;
                if (count != current.disk_count) {
                    printf("[usbd] block devices=%zu generation=%llu%s\n",
                        count, (unsigned long long)current.id,
                        next ? " truncated" : "");
                    for (size_t i = 0; i < count; ++i)
                    {
                        int install_media = 0;
                        int gpt_status = usbd_media_probe(&current.block,
                            &current.ipc, current.launch.process.fd,
                            &current.disks[i], &install_media);
                        printf("[usbd] block %s diskseq=%llu bytes=%llu logical=%u ro=%u gpt=%d install_media=%d\n",
                            current.disks[i].name,
                            (unsigned long long)current.disks[i].diskseq,
                            (unsigned long long)current.disks[i].bytes,
                            current.disks[i].logical_block_size,
                            current.disks[i].read_only, gpt_status,
                            install_media);
                    }
                    fflush(stdout);
                    current.disk_count = count;
                }
            }
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
        config.size != sizeof(config) ||
        config.device_fd < 16 || config.filed_endpoint_fd < 16 ||
        config.ready_channel_fd < 16 ||
        (config.kind != USBD_KIND_USB_XHCI && config.kind != USBD_KIND_NVME) ||
        (config.kind == USBD_KIND_USB_XHCI && config.input_source_endpoint_fd < 16) ||
        (config.installer_channel_fd && config.installer_channel_fd < 16) ||
        !config.resource_id || config.pci_device > 31 || config.pci_function > 7)
        return 1;
    int status = run(&config);
    fprintf(stderr, "[usbd] stopped status=%d\n", status);
    (void)send_ready(&config, status);
    return 1;
}

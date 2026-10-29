/* SPDX-License-Identifier: MIT */
#include <gpud/boot_config.h>
#include "bootstrap.h"
#include "drm_service.h"
#include "launch.h"
#include "lifecycle.h"
#include "package.h"
#include "inspect_policy.h"
#include "discovery.h"
#include "process.h"
#include "control_poll.h"
#include "../kobox2_adapter/gpu_query_message.h"
#include "../kobox2_adapter/gpu_queue_message.h"
#include "../kobox2_adapter/sandbox_config.h"
#include "../kobox2_owner/boot.h"

#include <kobox2/virtqueue_x86_64.h>
#include <kobox2/module_progress.h>
#include <pacha/bootstrap.h>
#include <pacha/capsule.h>
#include <pacha/ipc.h>
#include <pacha/log.h>
#include <filed_client/module_image.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { GPUD_RESOURCE_SET_ID = 1, GPUD_SANDBOX_ID = 1 };

/* An opt-in, root-owned RAM command file is only the one-shot diagnostic
 * trigger. Device ownership and package verification remain capabilities in
 * gpud; the ordinary virtio service never reads this path. */
#define GPUD_INSPECT_CONTROL_PATH "/run/pacha/gpu-control"
#define GPUD_INSPECT_POLICY_PATH "/run/pacha/gpu-probe.conf"

static void log_runtime_event(const struct gpud_boot_config *config,
    const char *event, int status) {
    char line[160];
    int size = snprintf(line, sizeof(line), "%s status=%d", event, status);
    if (size <= 0 || (size_t)size >= sizeof(line))
        return;
    if (config->log_endpoint_fd >= 16)
        (void)pacha_log_emit((pacha_fd_t)config->log_endpoint_fd,
            PACHA_LOG_SOURCE_GPUD, line, (size_t)size);
    else
        printf("[gpud] %s\n", line);
}

/* 0: absent/incomplete, 1: probe, 2: stop. A truncated shell write is not
 * a command. The file lives only in the RAM root and is never an authority
 * to select a PCI function or bypass package/firmware verification. */
static int manual_control(pacha_fd_t filed_fd) {
    struct filed_client_module_image image = {0};
    int error = filed_client_load_optional_module_image_limited(filed_fd,
        GPUD_INSPECT_CONTROL_PATH, "gpu-control", 16, &image);
    if (error)
        return error == -ENOENT || error == -7 ? 0 : error;
    int command = image.size == 6 &&
        !memcmp(image.data, "probe\n", 6) ? 1 :
        image.size == 5 && !memcmp(image.data, "stop\n", 5) ? 2 : 0;
    filed_client_release_module_image(&image);
    return command;
}

static int wait_manual_probe(const struct gpud_boot_config *config) {
    log_runtime_event(config, "PCI DRM inspection armed; awaiting RAM command", 0);
    for (;;) {
        int command = manual_control((pacha_fd_t)config->filed_endpoint_fd);
        if (command == 1) {
            log_runtime_event(config, "manual PCI DRM probe requested", 0);
            return 0;
        }
        if (command == 2)
            return -ECANCELED;
        if (command < 0)
            return command;
        /* filed can expose this optional node before the one-shot probe. A
         * caller must get ENODEV, never block on an unserviced endpoint. */
        int rejected = gpud_drm_service_reject_unavailable(
            (pacha_fd_t)config->drm_endpoint_fd);
        if (rejected != -EAGAIN && rejected)
            return rejected;
        struct pacha_pollfd events[2] = {
            {.fd = (pacha_fd_t)config->control_channel_fd,
                .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
            {.fd = (pacha_fd_t)config->drm_endpoint_fd,
                .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
        };
        long waited = pacha_fd_wait_many(events, 2, 1000);
        if (waited == PACHA_ERR_NOT_READY)
            continue;
        if (waited < 0 || events[0].revents ||
            (events[1].revents & PACHA_FD_EVENT_HANGUP))
            return -EPIPE;
    }
}

static int request_display_release(const struct gpud_boot_config *config) {
    const struct pacha_ipc_msg request = {
        .word0 = GPUD_CONTROL_DISPLAY_RELEASE_MAGIC,
    };
    int status = pacha_ipc_send((pacha_fd_t)config->control_channel_fd, &request);
    if (status) return status;
    struct pacha_pollfd event = {.fd = (pacha_fd_t)config->control_channel_fd,
        .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
    /* The owner services this channel from its 20 ms live-console loop.
     * Never start the driver if it cannot attest that GOP stopped writing. */
    long waited = pacha_fd_wait_many(&event, 1, 5000);
    if (waited != 1 || !(event.revents & PACHA_FD_EVENT_READABLE))
        return waited < 0 ? (int)waited : -ETIMEDOUT;
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
    struct pacha_ipc_msg reply = {.fds = fds,
        .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS};
    status = pacha_ipc_recv((pacha_fd_t)config->control_channel_fd, &reply);
    for (size_t i = 0; i < reply.fd_count; ++i)
        (void)pacha_fd_close((pacha_fd_t)fds[i].fd);
    if (status) return status;
    if (reply.word0 != GPUD_CONTROL_DISPLAY_RELEASED_MAGIC ||
        reply.word2 || reply.word3 || reply.fd_count)
        return -EPROTO;
    return (int)(int64_t)reply.word1;
}

/* The trusted boot profile grants the PCI capability and arms this process.
 * An authenticated live root may replace only the expected package digests
 * in RAM for a later one-shot probe; no PCI selection or automatic probe is
 * derived from the uploaded file. This keeps Kobox2 rebuilds off the USB. */
static int refresh_inspection_policy(struct gpud_boot_config *config) {
    struct filed_client_module_image image = {0};
    int error = filed_client_load_optional_module_image_limited(
        (pacha_fd_t)config->filed_endpoint_fd, GPUD_INSPECT_POLICY_PATH,
        "gpu-probe-policy", 384, &image);
    if (error == -ENOENT) return 0;
    if (error) return error;
    error = gpud_inspect_policy_parse(image.data, image.size, config);
    filed_client_release_module_image(&image);
    if (!error)
        log_runtime_event(config, "RAM GPU package policy selected", 0);
    return error;
}

static void log_progress(const struct gpud_boot_config *config,
    const struct gpud_package *package, const struct gpud_lifecycle *lifecycle) {
    char line[192];
    const unsigned phase = lifecycle->progress_phase;
    const unsigned index = lifecycle->progress_module_index;
    const char *name = "";
    int name_length = 0;
    kb2_closure_manifest_artifact_t artifact;
    if ((phase == KOBOX_MODULE_PROGRESS_MODULE_BEGIN ||
         phase == KOBOX_MODULE_PROGRESS_MODULE_FAILED) &&
        index + 1 < package->artifact_count &&
        kb2_closure_manifest_artifact(&package->decoded, index + 1,
            &artifact) == KB2_PROTOCOL_OK) {
        name = artifact.namespace_name.data;
        name_length = (int)(artifact.namespace_name.length < 48 ?
            artifact.namespace_name.length : 48);
    }
    int length = snprintf(line, sizeof(line),
        "Linux phase=%u module=%u/%zu name=%.*s status=%d",
        phase, index + 1, package->artifact_count - 1,
        name_length, name, lifecycle->progress_status);
    if (length <= 0 || (size_t)length >= sizeof(line))
        return;
    if (config->log_endpoint_fd >= 16)
        (void)pacha_log_emit((pacha_fd_t)config->log_endpoint_fd,
            PACHA_LOG_SOURCE_KOBOX2_GPU, line, (size_t)length);
    else
        printf("[gpud] %s\n", line);
}

static void log_probe_failure(const struct gpud_boot_config *config,
    const struct gpud_lifecycle *lifecycle) {
    char line[192];
    int length;
    if (lifecycle->failure_fault) {
        length = snprintf(line, sizeof(line),
            "Linux fault vector=%u error=0x%x core_relative=%u ip=0x%llx address=0x%llx",
            lifecycle->failure_vector, lifecycle->failure_error_code,
            lifecycle->failure_core_relative,
            (unsigned long long)lifecycle->failure_ip,
            (unsigned long long)lifecycle->failure_address);
    } else if (lifecycle->failure_source) {
        length = snprintf(line, sizeof(line),
            "Linux failure source=%llu line=%u errno=%d",
            (unsigned long long)lifecycle->failure_source,
            lifecycle->failure_line, lifecycle->native_error);
    } else {
        length = snprintf(line, sizeof(line),
            "Linux probe failed errno=%d loaded=%u pci_bound=%u phase=%u module=%u",
            lifecycle->native_error, lifecycle->failure_loaded,
            lifecycle->failure_pci_bound, lifecycle->progress_phase,
            lifecycle->progress_module_index);
    }
    if (length <= 0 || (size_t)length >= sizeof(line))
        return;
    if (config->log_endpoint_fd >= 16)
        (void)pacha_log_emit((pacha_fd_t)config->log_endpoint_fd,
            PACHA_LOG_SOURCE_KOBOX2_GPU, line, (size_t)length);
    else
        printf("[gpud] %s\n", line);
}

static void *controller_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void controller_deallocate(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

static int wait_readable(pacha_fd_t fd, pacha_fd_t process_fd, uint64_t deadline_ticks) {
    struct pacha_pollfd events[2] = {
        {.fd = fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
        {.fd = process_fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
    };
    long result = pacha_fd_wait_many(events, 2, deadline_ticks);
    if (events[1].revents)
        return -EPIPE;
    if (result == PACHA_ERR_NOT_READY || result == 0)
        return -ETIMEDOUT;
    if (result > 0 && events[0].revents == PACHA_FD_EVENT_READABLE)
        return 0;
    return -EIO;
}

static int wait_service(
    struct gpud_drm_service *service, pacha_fd_t endpoint_fd,
    pacha_fd_t process_fd, pacha_fd_t control_fd, uint64_t timeout_ticks,
    struct pacha_fd_wait_retained *retained) {
    int error = gpud_drm_service_reserve_pollfds(service, 4);
    if (error) return error;
    struct pacha_pollfd *events = service->pollfds;
    events[0] = (struct pacha_pollfd){
        .fd = endpoint_fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
    events[1] = (struct pacha_pollfd){
        .fd = process_fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP
    };
    size_t event_count = 2;
    const size_t gpu_index = event_count++;
    events[gpu_index] = (struct pacha_pollfd){
        .fd = service->gpu.ipc->fd,
        .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP,
    };
    size_t control_index = SIZE_MAX;
    if (control_fd >= 16) {
        control_index = event_count++;
        events[control_index] = (struct pacha_pollfd){
            .fd = control_fd,
            .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP,
        };
    }
    size_t source_count = gpud_drm_service_pollfds(
        service, events + event_count, service->poll_capacity - event_count);
    long result = pacha_fd_wait_many_retained(
        retained, events, event_count + source_count, timeout_ticks);
    if (events[1].revents)
        return -EPIPE;
    if (result == PACHA_ERR_NOT_READY || result == 0)
        return -EAGAIN;
    if (result <= 0)
        return -EIO;
    if (events[0].revents & PACHA_FD_EVENT_READABLE)
        return 0;
    if (events[gpu_index].revents & PACHA_FD_EVENT_READABLE)
        return 0;
    if (control_index != SIZE_MAX && events[control_index].revents)
        return 0;
    for (size_t i = 0; i < source_count; ++i)
        if (events[event_count + i].revents &
            (PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP))
            return 0;
    return -EIO;
}

static int wait_process(pacha_fd_t process_fd) {
    struct pacha_pollfd event = {
        .fd = process_fd,
        .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP,
    };
    long result = pacha_fd_wait_many(&event, 1, PACHA_FD_WAIT_FOREVER);
    return result > 0 && event.revents ? 0 : -EIO;
}

static int wait_process_bounded(pacha_fd_t process_fd) {
    struct pacha_pollfd event = {.fd = process_fd,
        .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
    /* Native wait deadlines are scheduler ticks. A failed module unload
     * must not leave an authorized GPU doing DMA indefinitely. */
    long result = pacha_fd_wait_many(&event, 1, 30000);
    return result > 0 && event.revents ? 0 :
        (result == PACHA_ERR_NOT_READY || result == 0) ? -ETIMEDOUT : -EIO;
}

static int send_ready(const struct gpud_boot_config *config, int status) {
    const struct pacha_ipc_msg message = {.word0 = GPUD_BOOT_READY_MAGIC,
        .word1 = (uint64_t)(int64_t)status, .word2 = status ? 0 : 1};
    return pacha_ipc_send((pacha_fd_t)config->ready_channel_fd, &message);
}

enum gpud_control_result {
    GPUD_CONTROL_IDLE,
    GPUD_CONTROL_FORCE_RESTART,
    GPUD_CONTROL_STOP,
    GPUD_CONTROL_PEER_CLOSED,
};

static int receive_control(pacha_fd_t fd, uint64_t generation) {
    if (fd < 16)
        return GPUD_CONTROL_IDLE;
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
    struct pacha_ipc_msg message = {
        .fds = fds,
        .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS,
    };
    int status = pacha_ipc_recv(fd, &message);
    if (status == PACHA_ERR_EMPTY || status == PACHA_ERR_NOT_READY) {
        // An empty native channel returns EMPTY even after its peer closes.
        // Drain any final message before retiring the level-triggered HANGUP.
        struct pacha_pollfd event = {.fd = fd,
            .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
        if (pacha_fd_poll(&event, 1) < 0)
            return -EIO;
        return !(event.revents & PACHA_FD_EVENT_READABLE) &&
            (event.revents & PACHA_FD_EVENT_HANGUP) ?
            GPUD_CONTROL_PEER_CLOSED : GPUD_CONTROL_IDLE;
    }
    if (status == PACHA_ERR_CLOSED)
        return GPUD_CONTROL_PEER_CLOSED;
    if (status)
        return -EIO;
    int valid = (message.word0 == GPUD_CONTROL_FORCE_RESTART_MAGIC ||
        message.word0 == GPUD_CONTROL_STOP_MAGIC) &&
        message.word1 == generation && !message.word2 && !message.word3 &&
        !message.fd_count;
    for (size_t i = 0; i < message.fd_count; ++i)
        if (pacha_fd_close((pacha_fd_t)message.fds[i].fd))
            return -EIO;
    return valid ? (message.word0 == GPUD_CONTROL_STOP_MAGIC ?
        GPUD_CONTROL_STOP : GPUD_CONTROL_FORCE_RESTART) : -EPROTO;
}

static int terminate_sandbox(
    struct gpud_native_launch *launch, uint64_t generation) {
    for (;;) {
        int error = gpud_native_process_terminate(
            &launch->process, generation, 9);
        if (!error)
            return 0;
        if (error != -EAGAIN)
            return error;
        error = wait_process(launch->process.fd);
        if (error)
            return error;
    }
}

static int send_restarted(pacha_fd_t control_fd, uint64_t generation) {
    const struct pacha_ipc_msg message = {
        .word0 = GPUD_CONTROL_RESTARTED_MAGIC,
        .word1 = generation,
        .word2 = 1,
    };
    return pacha_ipc_send(control_fd, &message);
}

static int complete_action(kb2_controller_t *controller,
    kb2_action_type_t type, uint64_t resource, uint64_t sandbox) {
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    if (!action || kb2_action_type(action) != type)
        return -22;
    return kb2_controller_complete_action(controller, kb2_action_generation(action),
        kb2_action_token(action), KB2_STATUS_OK, resource, sandbox) == KB2_STATUS_OK ? 0 : -5;
}

struct gpud_generation {
    uint64_t id;
    struct pacha_ipc_channel_pair management;
    struct ph_ipc ipc;
    struct gpud_native_launch launch;
    struct gpud_process_watch watch;
    struct gpud_lifecycle lifecycle;
    pacha_fd_t gpu_fd;
    unsigned char *gpu_mapping;
    struct gpud_gpu_channel gpu_channel;
    struct gpud_drm_service service;
    /* serve_generation is the sole waiting thread for this generation. */
    struct pacha_fd_wait_retained wait_set;
};

static int retire_drm_generation(
    struct gpud_generation *generation, uint64_t *handle_sequence) {
    if (!generation || !handle_sequence)
        return -EINVAL;
    struct gpud_drm_service *service = &generation->service;
    *handle_sequence = service->files.handle_sequence;
    for (size_t i = 0; i < GPUD_DRM_REFERENCES_MAX; ++i) {
        struct gpud_drm_watch *watch = &service->watches[i];
        if (!watch->handle)
            continue;
        int error = pacha_fd_close(watch->fd);
        if (error)
            return error;
        *watch = (struct gpud_drm_watch){0};
    }
    int error = ph_ipc_packet_release(&service->received);
    int cleanup = ph_ipc_packet_release(&service->gpu.incoming);
    if (!error)
        error = cleanup;
    cleanup = gpud_drm_service_retire_mappings(service);
    if (!error)
        error = cleanup;
    if (!error && service->page) {
        error = pacha_munmap(service->page, GPUD_DRM_PAGE_BYTES);
        if (!error)
            service->page = NULL;
    }
    if (!error && generation->gpu_mapping) {
        kb2_vq_channel_fault(&service->gpu.channel->channel);
        error = pacha_munmap(
            generation->gpu_mapping, GPUD_GPU_CHANNEL_SIZE);
        if (!error)
            generation->gpu_mapping = NULL;
    }
    if (!error && generation->gpu_fd >= 16) {
        error = pacha_fd_close(generation->gpu_fd);
        if (!error)
            generation->gpu_fd = -1;
    }
    return error;
}

static int restart_generation(kb2_controller_t *controller,
    struct gpud_package *package, struct gpud_generation *current,
    uint64_t *handle_sequence) {
    const uint64_t generation = current->id;
    kb2_status_t observed = gpud_process_observe(&current->watch, generation);
    if (observed == KB2_STATUS_ACTION_PENDING) {
        kb2_status_t fault = kb2_controller_report_fault(controller, generation,
            KB2_FAULT_PROTOCOL,
            (uint64_t)(unsigned int)-current->service.error);
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
            return kb2_controller_generation(controller) == generation + 1 ?
                0 : -EIO;
        if (type == KB2_ACTION_TERMINATE_SANDBOX ||
            type == KB2_ACTION_REAP_SANDBOX) {
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
                int error = gpud_native_launch_discard(
                    &current->launch, generation);
                if (!error)
                    error = gpud_lifecycle_release(&current->lifecycle);
                if (!error)
                    error = ph_ipc_destroy(&current->ipc, generation);
                if (error)
                    return error;
            }
            continue;
        }
        if (type == KB2_ACTION_REVOKE_RESOURCES) {
            int error = pacha_capsule_dma_set_enabled(package->device_fd, 0);
            if (!error)
                error = retire_drm_generation(current, handle_sequence);
            if (error)
                return error;
            if (complete_action(controller, type, 0, 0))
                return -EIO;
            continue;
        }
        if (type == KB2_ACTION_RELEASE_RESOURCES) {
            int error = gpud_package_unbind(package, generation);
            if (error)
                return error;
            if (complete_action(controller, type, 0, 0))
                return -EIO;
            continue;
        }
        /* This closure currently requires revoke, not an unimplemented host
         * reset action. Never acknowledge a new controller action by default. */
        return -EOPNOTSUPP;
    }
}

static int stop_generation(kb2_controller_t *controller,
    struct gpud_package *package, struct gpud_generation *current,
    uint64_t *handle_sequence, int inspect) {
    const uint64_t generation = current->id;
    int quiesce_error = 0;
    if (kb2_controller_stop(controller) != KB2_STATUS_OK)
        return -EIO;
    for (;;) {
        const kb2_action_t *action = kb2_controller_pending_action(controller);
        if (!action)
            return kb2_controller_state(controller) == KB2_STATE_IDLE ?
                quiesce_error : -EIO;
        const kb2_action_type_t type = kb2_action_type(action);
        const uint64_t token = kb2_action_token(action);
        if (type == KB2_ACTION_QUIESCE_SANDBOX) {
            kb2_status_t status = gpud_lifecycle_quiesce(&current->lifecycle, token);
            if (status == KB2_STATUS_ACTION_PENDING) {
                int error = wait_process_bounded(current->launch.process.fd);
                if (!error)
                    continue;
                /* A nonterminating driver can still access the device.
                 * Quarantine DMA before escalating to forced termination. */
                if (!quiesce_error)
                    quiesce_error = error;
                (void)pacha_capsule_dma_set_enabled(package->device_fd, 0);
                if (kb2_controller_complete_action(controller, generation, token,
                        KB2_STATUS_HOST_FAILURE, 0, 0) != KB2_STATUS_HOST_FAILURE ||
                    kb2_controller_stop(controller) != KB2_STATUS_OK)
                    return error;
                continue;
            }
            if (status != KB2_STATUS_OK) {
                /* Continue resource revoke/release, but do not turn an
                 * abnormal sandbox exit into a successful GPU stop. */
                if (!quiesce_error)
                    quiesce_error = current->lifecycle.native_error ?
                        current->lifecycle.native_error : -EIO;
                (void)pacha_capsule_dma_set_enabled(package->device_fd, 0);
                if (kb2_controller_pending_action(controller) &&
                    kb2_controller_complete_action(controller, generation, token,
                        KB2_STATUS_HOST_FAILURE, 0, 0) != KB2_STATUS_HOST_FAILURE)
                    return -EIO;
                if (kb2_controller_stop(controller) != KB2_STATUS_OK)
                    return -EIO;
            }
            continue;
        }
        if (type == KB2_ACTION_TERMINATE_SANDBOX ||
            type == KB2_ACTION_REAP_SANDBOX) {
            kb2_status_t status = gpud_process_action(&current->watch,
                generation, token, 9);
            if (status == KB2_STATUS_ACTION_PENDING) {
                int error = wait_process_bounded(current->launch.process.fd);
                if (error) {
                    (void)pacha_capsule_dma_set_enabled(package->device_fd, 0);
                    return error;
                }
                continue;
            }
            if (status != KB2_STATUS_OK)
                return -EIO;
            if (type == KB2_ACTION_REAP_SANDBOX) {
                int error = gpud_native_launch_discard(&current->launch, generation);
                if (!error)
                    error = gpud_lifecycle_release(&current->lifecycle);
                if (!error)
                    error = ph_ipc_destroy(&current->ipc, generation);
                if (error)
                    return error;
            }
            continue;
        }
        if (type == KB2_ACTION_REVOKE_RESOURCES) {
            int error = pacha_capsule_dma_set_enabled(package->device_fd, 0);
            if (!error && !inspect)
                error = retire_drm_generation(current, handle_sequence);
            if (error)
                return error;
            if (complete_action(controller, type, 0, 0))
                return -EIO;
            continue;
        }
        if (type == KB2_ACTION_RELEASE_RESOURCES) {
            int error = gpud_package_unbind(package, generation);
            if (error || complete_action(controller, type, 0, 0))
                return error ? error : -EIO;
            continue;
        }
        return -EOPNOTSUPP;
    }
}

static int start_generation(const struct gpud_boot_config *config,
    kb2_controller_t *controller, struct gpud_package *package,
    uint64_t handle_sequence, struct gpud_generation *current) {
    const int inspect = config->backend == GPUD_BACKEND_PCI_DRM_INSPECT;
    memset(current, 0, sizeof(*current));
    current->id = kb2_controller_generation(controller);
    current->management = (struct pacha_ipc_channel_pair){.a = -1, .b = -1};
    current->gpu_fd = -1;
    const uint64_t generation = current->id;
    int error = gpud_package_bind(package, generation);
    if (!error)
        error = complete_action(controller, KB2_ACTION_ALLOCATE_RESOURCES,
            GPUD_RESOURCE_SET_ID, 0);
    if (error) {
        printf("[gpud] stage=resource-bind status=%d\n", error);
        if (inspect)
            log_runtime_event(config, "resource bind failed", error);
        return error;
    }
    if (inspect)
        log_runtime_event(config, "PCI resource bound to Kobox2 owner", 0);

    struct ph_sandbox_config sandbox_config = {.size = sizeof(struct ph_sandbox_config),
        .identity = package->identity,
        .artifact_count = package->artifact_count, .resource_count = 1,
        .client_id = 1, .gpu_channel_id = 1,
        .device = package->device};
    struct gpud_launch_blob firmware_blob = {0};
    if (inspect) {
        if (!package->firmware || !package->firmware_size)
            return -EINVAL;
        sandbox_config.firmware_address = PH_SANDBOX_FIRMWARE_ADDRESS;
        sandbox_config.firmware_size = package->firmware_size;
        memcpy(sandbox_config.firmware_sha256, package->firmware_sha256, 32);
        firmware_blob = (struct gpud_launch_blob){.bytes = package->firmware,
            .size = package->firmware_size, .address = PH_SANDBOX_FIRMWARE_ADDRESS};
        sandbox_config.log_endpoint_fd = config->log_endpoint_fd >= 16 ?
            PH_SANDBOX_LOG_FD : 0;
    }
    const struct ph_owner_boot_config start = {.controller = controller,
        .launch = &current->launch, .ipc = &current->ipc,
        .management = &current->management, .watch = &current->watch,
        .lifecycle = &current->lifecycle,
        .resource_set_id = GPUD_RESOURCE_SET_ID, .sandbox_id = GPUD_SANDBOX_ID,
        .sandbox_config = &sandbox_config, .image = package->sandbox,
        .image_size = package->sandbox_size,
        .extra_blobs = inspect ? &firmware_blob : NULL,
        .extra_blob_count = inspect ? 1 : 0,
        .items = package->items, .artifact_count = package->artifact_count,
        .resource_count = 1,
        .log_endpoint_fd = inspect ? (pacha_fd_t)config->log_endpoint_fd : -1};
    struct ph_owner_boot boot = {0};
    kb2_status_t begin_status = ph_owner_boot_begin(&boot, &start);
    if (begin_status != KB2_STATUS_OK) {
        printf("[gpud] stage=sandbox-boot phase=%u status=%u native=%d\n",
            boot.phase, begin_status, boot.native_error);
        if (inspect)
            log_runtime_event(config, "sandbox boot setup failed",
                boot.native_error ? boot.native_error : -EIO);
        return -5;
    }
    struct timespec transfer_started = {0};
    if (inspect && clock_gettime(CLOCK_MONOTONIC, &transfer_started))
        return -EIO;
    while (boot.phase != PH_OWNER_BOOT_COMPLETE) {
        kb2_status_t status = ph_owner_boot_step(&boot);
        if (status != KB2_STATUS_OK && status != KB2_STATUS_ACTION_PENDING) {
            printf("[gpud] stage=sandbox-boot phase=%u status=%u native=%d\n",
                boot.phase, status, boot.native_error);
            if (inspect)
                log_runtime_event(config, "sandbox boot failed",
                    boot.native_error ? boot.native_error : -EIO);
            return -5;
        }
        if (inspect && boot.phase != PH_OWNER_BOOT_COMPLETE) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now))
                return -EIO;
            if (now.tv_sec - transfer_started.tv_sec >= 180)
                return -ETIMEDOUT;
            int command = manual_control((pacha_fd_t)config->filed_endpoint_fd);
            if (command == 2)
                return -ECANCELED;
            if (command < 0)
                return command;
            if (boot.native_error == -EAGAIN ||
                boot.native_error == -ENOMEM) {
                struct pacha_pollfd events[2] = {
                    {.fd = current->ipc.fd,
                     .events = PACHA_FD_EVENT_WRITABLE | PACHA_FD_EVENT_HANGUP},
                    {.fd = current->launch.process.fd,
                     .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
                };
                long waited = pacha_fd_wait_many(events, 2, 1000);
                if (waited == PACHA_ERR_NOT_READY)
                    continue;
                if (waited < 0 || events[1].revents ||
                    (events[0].revents & PACHA_FD_EVENT_HANGUP))
                    return -EPIPE;
            }
        }
    }
    if (inspect)
        log_runtime_event(config, "sandbox modules transferred", 0);
    struct timespec inspect_started = {0};
    if (inspect && clock_gettime(CLOCK_MONOTONIC, &inspect_started))
        return -EIO;
    for (;;) {
        if (inspect) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now))
                return -EIO;
            if (now.tv_sec - inspect_started.tv_sec >= 180)
                return -ETIMEDOUT;
        }
        const unsigned previous_progress = current->lifecycle.progress_count;
        kb2_status_t status = gpud_lifecycle_ready(&current->lifecycle);
        if (current->lifecycle.progress_count != previous_progress)
            log_progress(config, package, &current->lifecycle);
        if (status == KB2_STATUS_OK)
            break;
        int waited = status == KB2_STATUS_ACTION_PENDING ?
            wait_readable(current->ipc.fd, current->launch.process.fd,
                inspect ? 1000 : PACHA_FD_WAIT_FOREVER) : -EIO;
        if (inspect && waited == -ETIMEDOUT) {
            int command = manual_control((pacha_fd_t)config->filed_endpoint_fd);
            if (command == 2)
                return -ECANCELED;
            if (command < 0)
                return command;
            continue;
        }
        if (status != KB2_STATUS_ACTION_PENDING || waited) {
            printf("[gpud] stage=sandbox-ready status=%u native=%d\n",
                status, current->lifecycle.native_error);
            if (inspect)
                log_probe_failure(config, &current->lifecycle);
            return -5;
        }
    }

    const uint64_t gpu_rights = PH_GPU_QUERY_RIGHTS | PACHA_FD_RIGHT_TRANSFER;
    current->gpu_fd = pacha_vmo_create(
        GPUD_GPU_CHANNEL_SIZE, gpu_rights, 0);
    if (current->gpu_fd < 16)
        return -12;
    current->gpu_mapping = pacha_mmap(
        current->gpu_fd, GPUD_GPU_CHANNEL_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!current->gpu_mapping)
        return -5;
    if (ph_gpu_channel_bind(&current->gpu_channel, current->gpu_mapping,
            generation,
            sandbox_config.gpu_channel_id, KB2_VQ_DRIVER, &kb2_vq_x86_64_atomics))
        return -5;
    struct ph_ipc_packet binding = {.operation = PH_GPU_QUEUE_BIND,
        .generation = generation, .value = sandbox_config.gpu_channel_id, .fd_count = 1,
        .fds = {{.fd = (uint64_t)current->gpu_fd, .rights = PH_GPU_QUERY_RIGHTS,
            .flags = PACHA_FD_FLAG_CLOEXEC}}};
    if ((error = ph_ipc_send(&current->ipc, &binding)))
        return error;

    current->service.backend_client = sandbox_config.client_id;
    current->service.profile_kind = inspect ? KB2_GPU_PROFILE_AMDGPU :
        KB2_GPU_PROFILE_VIRGL;
    current->service.log_endpoint_fd = (pacha_fd_t)config->log_endpoint_fd;
    current->service.gpu.ipc = &current->ipc;
    current->service.gpu.channel = &current->gpu_channel;
    current->service.gpu.mapping = current->gpu_mapping;
    if ((error = gpud_gpu_rpc_start_events(&current->service.gpu)))
        return error;
    if ((error = gpud_drm_files_init(&current->service.files, generation,
            GPUD_GPU_NATIVE_SESSION_LIMIT)))
        return error;
    current->service.files.handle_sequence = handle_sequence;
    if ((error = gpud_drm_service_bind(
            &current->service, (pacha_fd_t)config->drm_endpoint_fd)))
        return error;
    return 0;
}

static int serve_generation(
    struct gpud_generation *current, pacha_fd_t filed_fd, pacha_fd_t *control_fd, int inspect,
    int *forced, int *stop_requested, const struct gpud_boot_config *config) {
    *forced = 0;
    *stop_requested = 0;
    struct gpud_control_poll command_poll = {0};
    for (;;) {
        if (inspect) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now))
                return -EIO;
            uint64_t now_ms = (uint64_t)now.tv_sec * 1000 +
                (uint64_t)now.tv_nsec / 1000000;
            if (gpud_control_poll_due(&command_poll, now_ms)) {
                int command = manual_control(filed_fd);
                if (command == 2) {
                    log_runtime_event(config, "stop requested by manual control", 0);
                    *stop_requested = 1;
                    return 0;
                }
                if (command < 0)
                    return command;
            }
        }
        int control = receive_control(*control_fd, current->id);
        if (control == GPUD_CONTROL_STOP ||
            (inspect && control == GPUD_CONTROL_PEER_CLOSED)) {
            log_runtime_event(config, control == GPUD_CONTROL_STOP ?
                "stop requested by owner" : "owner control peer closed", 0);
            *stop_requested = 1;
            return 0;
        }
        if (control == GPUD_CONTROL_PEER_CLOSED) {
            (void)pacha_fd_close(*control_fd);
            *control_fd = -1;
        } else if (control == GPUD_CONTROL_FORCE_RESTART) {
            if (inspect)
                return -EOPNOTSUPP;
            int error = terminate_sandbox(&current->launch, current->id);
            if (error)
                return -EIO;
            printf("[gpud] sandbox-force-killed generation=%llu\n",
                (unsigned long long)current->id);
            *forced = 1;
            return -EPIPE;
        } else if (control < 0) {
            return control;
        }
        int error = gpud_drm_service_pump_events(&current->service);
        if (error)
            return error;
        error = gpud_drm_service_reap_hangups(&current->service);
        if (error)
            return error;
        error = gpud_drm_service_receive(&current->service);
        if (!error)
            continue;
        if (error != -EAGAIN)
            return error;
        uint64_t timeout = PACHA_FD_WAIT_FOREVER;
        if (inspect) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now))
                return -EIO;
            timeout = gpud_control_poll_timeout(&command_poll,
                (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000);
        }
        error = wait_service(&current->service, current->service.endpoint_fd,
            current->launch.process.fd, *control_fd,
            timeout, &current->wait_set);
        if (inspect && error == -EAGAIN)
            continue;
        if (error)
            return error;
    }
}

static int run(const struct gpud_boot_config *config) {
    struct gpud_boot_config active = *config;
    struct gpud_package package = {0};
    struct gpud_discovery discovery = {0};
    int error;
    if (config->backend == GPUD_BACKEND_PCI_DRM_INSPECT &&
        (error = wait_manual_probe(config))) {
        log_runtime_event(config, "manual probe not started", error);
        return error;
    }
    if (config->backend == GPUD_BACKEND_PCI_DRM_INSPECT &&
        (error = refresh_inspection_policy(&active))) {
        log_runtime_event(config, "RAM GPU package policy rejected", error);
        return error;
    }
    config = &active;
    if (config->backend == GPUD_BACKEND_VIRTIO) {
        error = gpud_package_open(&package,
            (pacha_fd_t)config->filed_endpoint_fd, (pacha_fd_t)config->device_fd);
    } else {
        error = gpud_package_open_at(&package,
            (pacha_fd_t)config->filed_endpoint_fd, (pacha_fd_t)config->device_fd,
            config->package_root, config->firmware_sha256,
            config->manifest_sha256, config->sandbox_sha256);
    }
    if (error) {
        printf("[gpud] stage=package-open status=%d\n", error);
        log_runtime_event(config, "package verification failed", error);
        return error;
    }
    printf("[gpud] package artifacts=%zu\n", package.artifact_count);
    /* Both GPU profiles expose a primary DRM node. The optional profile
     * publishes it only after the manual probe and GOP release succeed. */
    error = gpud_discovery_prepare(&discovery, package.device_fd, 1);
    if (error) {
        log_runtime_event(config, "DRM discovery capability snapshot failed", error);
        (void)gpud_package_close(&package);
        return error;
    }
    if (config->backend == GPUD_BACKEND_PCI_DRM_INSPECT)
        log_runtime_event(config, "package manifest, sandbox and firmware verified", 0);

    kb2_controller_t *controller = NULL;
    if (kb2_controller_create(controller_allocate, controller_deallocate, NULL,
            &controller) != KB2_STATUS_OK ||
        (error = gpud_package_configure_controller(&package, controller)) ||
        kb2_controller_start(controller) != KB2_STATUS_OK) {
        error = error ? error : -5;
        printf("[gpud] stage=controller-config status=%d\n", error);
        log_runtime_event(config, "controller configuration failed", error);
        return error;
    }

    if (config->backend == GPUD_BACKEND_PCI_DRM_INSPECT &&
        (error = request_display_release(config))) {
        log_runtime_event(config, "GOP display handoff failed", error);
        return error;
    }
    if (config->backend == GPUD_BACKEND_PCI_DRM_INSPECT)
        log_runtime_event(config, "GOP display MMIO released", 0);

    uint64_t handle_sequence = 0;
    pacha_fd_t control_fd = (pacha_fd_t)config->control_channel_fd;
    int notify_restart = 0;
    for (;;) {
        /* One owner serves one generation at a time and retires it fully
         * before reuse. Its per-session event buffers are long-lived state,
         * not call-stack scratch space. */
        static struct gpud_generation current;
        error = start_generation(
            config, controller, &package, handle_sequence, &current);
        if (error) {
            if (config->backend == GPUD_BACKEND_PCI_DRM_INSPECT) {
                /* Startup may fail after the PCI grant reached Linux. The
                 * owner must quarantine DMA before it returns an error or
                 * lets the process tree disappear. */
                const int quarantine = pacha_capsule_dma_set_enabled(
                    package.device_fd, 0);
                printf("[gpud] startup quarantine status=%d generation=%llu\n",
                    quarantine, (unsigned long long)current.id);
                log_runtime_event(config, "startup DMA quarantine", quarantine);
                if (current.launch.process.fd >= 16) {
                    /* Preserve the terminal reason before cleanup's KILL can
                     * replace it. An early native failure may have no Linux
                     * lifecycle packet and no usable serial console. */
                    const int observed = gpud_native_process_poll(
                        &current.launch.process, current.id);
                    if (!observed) {
                        char detail[160];
                        const struct gpud_process_exit *exit =
                            &current.launch.process.exit;
                        const int length = snprintf(detail, sizeof(detail),
                            "sandbox exit state=%llu code=%llu principal=%llu",
                            (unsigned long long)exit->state,
                            (unsigned long long)exit->code,
                            (unsigned long long)exit->principal);
                        if (length > 0 && (size_t)length < sizeof(detail))
                            log_runtime_event(config, detail, 0);
                    } else if (observed != -EAGAIN) {
                        log_runtime_event(config, "sandbox exit observation failed", observed);
                    }
                    int killed = gpud_native_process_terminate(
                        &current.launch.process, current.id, 9);
                    if (killed == -EAGAIN &&
                        !wait_process_bounded(current.launch.process.fd))
                        killed = gpud_native_process_poll(
                            &current.launch.process, current.id);
                    printf("[gpud] startup process stop status=%d\n", killed);
                    log_runtime_event(config, "startup process stop", killed);
                }
            }
            log_runtime_event(config, "sandbox startup failed", error);
            return error;
        }
        error = gpud_discovery_publish(&discovery, (pacha_fd_t)config->filed_endpoint_fd);
        if (error) {
            log_runtime_event(config, "DRM discovery publication failed", error);
            (void)stop_generation(controller, &package, &current,
                &handle_sequence, config->backend == GPUD_BACKEND_PCI_DRM_INSPECT);
            return error;
        }
        log_runtime_event(config, "DRM primary/render nodes and PCI discovery published", 0);
        int notice_error = current.id == 1 ? send_ready(config, 0) : 0;
        if (!notice_error && notify_restart) {
            notice_error = send_restarted(control_fd, current.id);
            notify_restart = 0;
        }
        if (notice_error) {
            /* A failed readiness notice must not strand published card0 or
             * leave a physical GPU bus-mastering after its owner exits. */
            int removed = gpud_discovery_remove(&discovery,
                (pacha_fd_t)config->filed_endpoint_fd);
            int stopped = stop_generation(controller, &package, &current,
                &handle_sequence,
                config->backend == GPUD_BACKEND_PCI_DRM_INSPECT);
            if (stopped)
                (void)pacha_capsule_dma_set_enabled(package.device_fd, 0);
            log_runtime_event(config, "DRM readiness notice failed", notice_error);
            if (removed)
                log_runtime_event(config, "DRM discovery removal failed", removed);
            if (stopped)
                log_runtime_event(config, "GPU stop after notice failure failed", stopped);
            return stopped ? stopped : removed ? removed : notice_error;
        }
        printf("[gpud] ready generation=%llu\n",
            (unsigned long long)current.id);
        if (config->backend == GPUD_BACKEND_PCI_DRM_INSPECT)
            log_runtime_event(config, "PCI DRM probe ready", 0);

        int forced = 0, stop_requested = 0;
        const int restart_error = serve_generation(
            &current, (pacha_fd_t)config->filed_endpoint_fd, &control_fd,
            config->backend == GPUD_BACKEND_PCI_DRM_INSPECT,
            &forced, &stop_requested, config);
        int discovery_error = gpud_discovery_remove(&discovery,
            (pacha_fd_t)config->filed_endpoint_fd);
        if (discovery_error)
            log_runtime_event(config, "DRM discovery removal failed", discovery_error);
        if (stop_requested || config->backend == GPUD_BACKEND_PCI_DRM_INSPECT) {
            if (!stop_requested) {
                /* An inspection probe is one-shot; never auto-reprobe a
                 * physical GPU after an unexpected process exit. */
                if (gpud_process_observe(&current.watch, current.id) != KB2_STATUS_OK)
                    (void)pacha_capsule_dma_set_enabled(package.device_fd, 0);
            }
            error = stop_generation(controller, &package, &current,
                &handle_sequence,
                config->backend == GPUD_BACKEND_PCI_DRM_INSPECT);
            printf("[gpud] generation=%llu stopped status=%d cause=%d\n",
                (unsigned long long)current.id, error, restart_error);
            log_runtime_event(config, "PCI DRM probe stopped", error ? error : restart_error);
            if (error)
                (void)pacha_capsule_dma_set_enabled(package.device_fd, 0);
            if (!error)
                error = gpud_package_close(&package);
            if (!error)
                error = discovery_error;
            kb2_controller_destroy(controller);
            if (control_fd >= 16)
                (void)pacha_fd_close(control_fd);
            return error ? error : restart_error;
        }
        printf("[gpud] generation=%llu fault=%d restarting\n",
            (unsigned long long)current.id, restart_error);
        current.service.error = restart_error;
        error = restart_generation(
            controller, &package, &current, &handle_sequence);
        if (error) {
            printf("[gpud] stage=generation-restart generation=%llu status=%d\n",
                (unsigned long long)current.id, error);
            return error;
        }
        printf("[gpud] generation=%llu retired next=%llu\n",
            (unsigned long long)current.id,
            (unsigned long long)kb2_controller_generation(controller));
        notify_restart = forced;
    }
}

int main(int argc, char **argv) {
    (void)argc;
    struct gpud_boot_config config = {0};
    pacha_fd_t bootstrap_fd = pacha_bootstrap_fd_from_argv(argv);
    if (bootstrap_fd < 16 ||
        pacha_fd_read(bootstrap_fd, &config, sizeof(config)) != (long)sizeof(config) ||
        config.magic != GPUD_BOOT_CONFIG_MAGIC ||
        config.size != sizeof(config) ||
        (config.backend != GPUD_BACKEND_VIRTIO &&
         config.backend != GPUD_BACKEND_PCI_DRM_INSPECT) ||
        config.package_root[sizeof(config.package_root) - 1] != 0 ||
        (config.backend == GPUD_BACKEND_VIRTIO &&
         (config.drm_endpoint_fd < 16 ||
          strcmp(config.package_root, "/srv/kobox2"))) ||
        (config.backend == GPUD_BACKEND_PCI_DRM_INSPECT &&
         (config.log_endpoint_fd < 16 || config.drm_endpoint_fd < 16)) ||
        config.device_fd < 16 ||
        config.filed_endpoint_fd < 16 || config.control_channel_fd < 16 ||
        config.ready_channel_fd < 16 ||
        (config.log_endpoint_fd != 0 && config.log_endpoint_fd < 16))
        return 1;
    int status = run(&config);
    fprintf(stderr, "[gpud] stopped status=%d\n", status);
    if (status)
        (void)send_ready(&config, status);
    return status ? 1 : 0;
}

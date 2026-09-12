/* SPDX-License-Identifier: MIT */
#include <gpud/boot_config.h>
#include "bootstrap.h"
#include "drm_service.h"
#include "launch.h"
#include "lifecycle.h"
#include "package.h"
#include "process.h"
#include "../kobox2_adapter/gpu_query_message.h"
#include "../kobox2_adapter/gpu_queue_message.h"
#include "../kobox2_adapter/sandbox_config.h"

#include <kobox2/virtqueue_x86_64.h>
#include <pacha/bootstrap.h>
#include <pacha/capsule.h>
#include <pacha/ipc.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GPUD_RESOURCE_SET_ID = 1, GPUD_SANDBOX_ID = 1 };

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
    if (result > 0 && events[0].revents == PACHA_FD_EVENT_READABLE)
        return 0;
    return -EIO;
}

static int wait_service(
    const struct gpud_drm_service *service, int endpoint_fd,
    int process_fd, int control_fd) {
    struct pacha_pollfd events[GPUD_DRM_REFERENCES_MAX + 3] = {
        {.fd = endpoint_fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
        {.fd = process_fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
    };
    size_t event_count = 2;
    size_t control_index = SIZE_MAX;
    if (control_fd >= 16) {
        control_index = event_count++;
        events[control_index] = (struct pacha_pollfd){
            .fd = control_fd,
            .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP,
        };
    }
    int sources[GPUD_DRM_REFERENCES_MAX];
    size_t source_count = gpud_drm_service_collect_wait_sources(
        service, sources, GPUD_DRM_REFERENCES_MAX);
    for (size_t i = 0; i < source_count; ++i)
        events[event_count + i] = (struct pacha_pollfd){
            .fd = sources[i], .events = PACHA_FD_EVENT_HANGUP};
    long result = pacha_fd_wait_many(
        events, event_count + source_count, PACHA_FD_WAIT_FOREVER);
    if (events[1].revents)
        return -EPIPE;
    if (result <= 0)
        return -EIO;
    if (events[0].revents & PACHA_FD_EVENT_READABLE)
        return 0;
    if (control_index != SIZE_MAX && events[control_index].revents)
        return 0;
    for (size_t i = 0; i < source_count; ++i)
        if (events[event_count + i].revents & PACHA_FD_EVENT_HANGUP)
            return 0;
    return -EIO;
}

static int wait_process(int process_fd) {
    struct pacha_pollfd event = {
        .fd = process_fd,
        .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP,
    };
    long result = pacha_fd_wait_many(&event, 1, PACHA_FD_WAIT_FOREVER);
    return result > 0 && event.revents ? 0 : -EIO;
}

static int send_ready(const struct gpud_boot_config *config, int status) {
    const struct pacha_ipc_msg message = {.word0 = GPUD_BOOT_READY_MAGIC,
        .word1 = (uint64_t)(int64_t)status, .word2 = status ? 0 : 1};
    return pacha_ipc_send((int)config->ready_channel_fd, &message);
}

enum gpud_control_result {
    GPUD_CONTROL_IDLE,
    GPUD_CONTROL_FORCE_RESTART,
    GPUD_CONTROL_PEER_CLOSED,
};

static int receive_control(int fd, uint64_t generation) {
    if (fd < 16)
        return GPUD_CONTROL_IDLE;
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
    struct pacha_ipc_msg message = {
        .fds = fds,
        .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS,
    };
    int status = pacha_ipc_recv(fd, &message);
    if (status == PACHA_ERR_EMPTY || status == PACHA_ERR_NOT_READY)
        return GPUD_CONTROL_IDLE;
    if (status == PACHA_ERR_CLOSED)
        return GPUD_CONTROL_PEER_CLOSED;
    if (status)
        return -EIO;
    int valid = message.word0 == GPUD_CONTROL_FORCE_RESTART_MAGIC &&
        message.word1 == generation && !message.word2 && !message.word3 &&
        !message.fd_count;
    for (size_t i = 0; i < message.fd_count; ++i)
        if (pacha_fd_close((int)message.fds[i].fd))
            return -EIO;
    return valid ? GPUD_CONTROL_FORCE_RESTART : -EPROTO;
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

static int send_restarted(int control_fd, uint64_t generation) {
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
    int gpu_fd;
    unsigned char *gpu_mapping;
    struct gpud_gpu_channel gpu_channel;
    struct gpud_drm_service service;
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
    if (!error)
        error = ph_ipc_packet_release(&service->gpu.incoming);
    if (!error && service->page) {
        error = pacha_munmap(service->page, DRMD_PAGE_BYTES);
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

static int start_generation(const struct gpud_boot_config *config,
    kb2_controller_t *controller, struct gpud_package *package,
    uint64_t handle_sequence, struct gpud_generation *current) {
    *current = (struct gpud_generation){
        .id = kb2_controller_generation(controller),
        .management = {.a = -1, .b = -1},
        .gpu_fd = -1,
    };
    const uint64_t generation = current->id;
    int error = gpud_package_bind(package, generation);
    if (!error)
        error = complete_action(controller, KB2_ACTION_ALLOCATE_RESOURCES,
            GPUD_RESOURCE_SET_ID, 0);
    if (error) {
        printf("[gpud] stage=resource-bind status=%d\n", error);
        return error;
    }

    const uint64_t channel_rights = PH_IPC_CHANNEL_RIGHTS |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER;
    error = pacha_ipc_channel_create(
        &current->management, channel_rights, 0);
    if (error) {
        printf("[gpud] stage=management-channel status=%d\n", error);
        return error;
    }
    error = ph_ipc_init(
        &current->ipc, current->management.a, generation);
    if (error) {
        printf("[gpud] stage=management-adopt status=%d\n", error);
        return error;
    }
    struct ph_sandbox_config sandbox_config = {.identity = package->identity,
        .artifact_count = GPUD_PACKAGE_ARTIFACTS, .resource_count = 1,
        .client_id = 1, .gpu_channel_id = 1, .device = package->device};
    struct gpud_launch_blob blob = {.bytes = &sandbox_config,
        .size = sizeof(sandbox_config), .address = PH_SANDBOX_CONFIG_ADDRESS};
    struct pacha_process_fd_grant grant = {
        .source_fd = (uint64_t)current->management.b,
        .target_fd = PH_SANDBOX_CHANNEL_FD, .rights = PH_IPC_CHANNEL_RIGHTS,
        .flags = PACHA_FD_FLAG_CLOEXEC};
    struct gpud_launch_request launch_request = {.generation = generation,
        .load_bias = UINT64_C(0x40000000), .stack_address = UINT64_C(0x45000000),
        .stack_size = 256 * 1024, .image = package->sandbox,
        .image_size = package->sandbox_size, .blobs = &blob, .blob_count = 1,
        .grants = &grant, .grant_count = 1};
    struct gpud_launch_resources launch_resources = {.generation = generation,
        .resource_set_id = GPUD_RESOURCE_SET_ID, .sandbox_id = GPUD_SANDBOX_ID,
        .request = &launch_request};
    memcpy(launch_resources.manifest_digest,
        package->identity.manifest_digest, 32);
    struct gpud_launch_transaction transaction = {0};
    if (gpud_launch_begin(&transaction, controller, &current->launch,
            &current->ipc,
            &launch_resources) != KB2_STATUS_OK ||
        gpud_launch_step(&transaction) != KB2_STATUS_OK || !transaction.completed) {
        printf("[gpud] stage=sandbox-launch status=%d\n", transaction.launch_error);
        return -5;
    }
    (void)pacha_fd_close(current->management.b);
    current->management.b = -1;
    if (gpud_process_watch_init(&current->watch, controller,
            &current->launch.process, &current->ipc,
            GPUD_RESOURCE_SET_ID, GPUD_SANDBOX_ID) != KB2_STATUS_OK) {
        printf("[gpud] stage=process-watch status=-5\n");
        return -5;
    }
    struct gpud_bootstrap_resources transfer = {.generation = generation,
        .resource_set_id = GPUD_RESOURCE_SET_ID, .sandbox_id = GPUD_SANDBOX_ID,
        .items = package->items, .artifact_count = GPUD_PACKAGE_ARTIFACTS,
        .resource_handle_count = 1};
    memcpy(transfer.manifest_digest, package->identity.manifest_digest, 32);
    struct gpud_bootstrap_transfer bootstrap = {0};
    if (gpud_bootstrap_begin(
            &bootstrap, controller, &current->ipc, &transfer) != KB2_STATUS_OK) {
        printf("[gpud] stage=bootstrap-begin status=-5\n");
        return -5;
    }
    while (!bootstrap.completed) {
        kb2_status_t status = gpud_bootstrap_step(&bootstrap);
        if (status != KB2_STATUS_OK && status != KB2_STATUS_ACTION_PENDING) {
            printf("[gpud] stage=bootstrap-transfer status=%u native=%d\n",
                status, bootstrap.native_error);
            return -5;
        }
    }
    if (gpud_lifecycle_init(
            &current->lifecycle, &current->watch) != KB2_STATUS_OK) {
        printf("[gpud] stage=lifecycle-init status=-5\n");
        return -5;
    }
    for (;;) {
        kb2_status_t status = gpud_lifecycle_ready(&current->lifecycle);
        if (status == KB2_STATUS_OK)
            break;
        if (status != KB2_STATUS_ACTION_PENDING ||
            wait_readable(current->ipc.fd, current->launch.process.fd)) {
            printf("[gpud] stage=sandbox-ready status=%u native=%d\n",
                status, current->lifecycle.native_error);
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

    current->service = (struct gpud_drm_service){
        .backend_client = sandbox_config.client_id,
        .gpu = {
            .ipc = &current->ipc,
            .channel = &current->gpu_channel,
            .mapping = current->gpu_mapping,
        },
    };
    if ((error = gpud_drm_files_init(&current->service.files, generation,
            GPUD_GPU_NATIVE_SESSION_LIMIT)))
        return error;
    current->service.files.handle_sequence = handle_sequence;
    if ((error = gpud_drm_service_bind(
            &current->service, (int)config->drm_endpoint_fd)))
        return error;
    return 0;
}

static int serve_generation(
    struct gpud_generation *current, int *control_fd, int *forced) {
    *forced = 0;
    for (;;) {
        int control = receive_control(*control_fd, current->id);
        if (control == GPUD_CONTROL_PEER_CLOSED) {
            (void)pacha_fd_close(*control_fd);
            *control_fd = -1;
        } else if (control == GPUD_CONTROL_FORCE_RESTART) {
            int error = terminate_sandbox(&current->launch, current->id);
            if (error)
                return error;
            printf("[gpud] sandbox-force-killed generation=%llu\n",
                (unsigned long long)current->id);
            *forced = 1;
            return -EPIPE;
        } else if (control < 0) {
            return control;
        }
        int error = gpud_drm_service_reap_hangups(&current->service);
        if (error)
            return error;
        error = gpud_drm_service_receive(&current->service);
        if (!error)
            continue;
        if (error != -EAGAIN)
            return error;
        error = wait_service(&current->service, current->service.endpoint_fd,
            current->launch.process.fd, *control_fd);
        if (error)
            return error;
    }
}

static int run(const struct gpud_boot_config *config) {
    struct gpud_package package = {0};
    int error = gpud_package_open(&package,
        (int)config->filed_endpoint_fd, (int)config->device_fd);
    if (error) {
        printf("[gpud] stage=package-open status=%d\n", error);
        return error;
    }
    printf("[gpud] package artifacts=%u\n", GPUD_PACKAGE_ARTIFACTS);

    kb2_controller_t *controller = NULL;
    if (kb2_controller_create(controller_allocate, controller_deallocate, NULL,
            &controller) != KB2_STATUS_OK ||
        (error = gpud_package_configure_controller(&package, controller)) ||
        kb2_controller_start(controller) != KB2_STATUS_OK) {
        error = error ? error : -5;
        printf("[gpud] stage=controller-config status=%d\n", error);
        return error;
    }

    uint64_t handle_sequence = 0;
    int control_fd = (int)config->control_channel_fd;
    int notify_restart = 0;
    for (;;) {
        struct gpud_generation current;
        error = start_generation(
            config, controller, &package, handle_sequence, &current);
        if (error)
            return error;
        if (current.id == 1 && (error = send_ready(config, 0)))
            return error;
        printf("[gpud] ready generation=%llu\n",
            (unsigned long long)current.id);
        if (notify_restart) {
            error = send_restarted(control_fd, current.id);
            if (error)
                return error;
            notify_restart = 0;
        }

        int forced = 0;
        const int restart_error = serve_generation(
            &current, &control_fd, &forced);
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
    int bootstrap_fd = pacha_bootstrap_fd_from_argv(argv);
    if (bootstrap_fd < 16 ||
        pacha_fd_read(bootstrap_fd, &config, sizeof(config)) != (long)sizeof(config) ||
        config.magic != GPUD_BOOT_CONFIG_MAGIC ||
        config.version != GPUD_BOOT_CONFIG_VERSION ||
        config.drm_endpoint_fd < 16 || config.device_fd < 16 ||
        config.filed_endpoint_fd < 16 || config.control_channel_fd < 16 ||
        config.ready_channel_fd < 16)
        return 1;
    int status = run(&config);
    fprintf(stderr, "[gpud] stopped status=%d\n", status);
    (void)send_ready(&config, status);
    return 1;
}

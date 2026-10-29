/* SPDX-License-Identifier: MIT */
#include <audiod/boot_config.h>
#include <audiod/protocol.h>
#include "../usbd/package.h"
#include "../kobox2_owner/boot.h"
#include <pacha/bootstrap.h>
#include <pacha/capsule.h>
#include <pacha/ipc.h>
#include <filed_client/client.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { AUDIOD_RESOURCE_SET_ID = 1, AUDIOD_SANDBOX_ID = 1 };
static void *controller_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void controller_deallocate(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

static int wait_readable(pacha_fd_t fd, pacha_fd_t process_fd) {
    struct pacha_pollfd events[2] = {
        {.fd = fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
        {.fd = process_fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
    };
    long result = pacha_fd_wait_many(events, 2, PACHA_FD_WAIT_FOREVER);
    if (events[1].revents)
        return -EPIPE;
    return result > 0 && events[0].revents == PACHA_FD_EVENT_READABLE ? 0 : -EIO;
}

static int wait_process(pacha_fd_t process_fd) {
    struct pacha_pollfd event = {.fd = process_fd,
        .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
    long result = pacha_fd_wait_many(&event, 1, PACHA_FD_WAIT_FOREVER);
    return result > 0 && event.revents ? 0 : -EIO;
}

static int complete_action(kb2_controller_t *controller,
    kb2_action_type_t type, uint64_t resource, uint64_t sandbox) {
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    if (!action || kb2_action_type(action) != type)
        return -EINVAL;
    return kb2_controller_complete_action(controller, kb2_action_generation(action),
        kb2_action_token(action), KB2_STATUS_OK, resource, sandbox) == KB2_STATUS_OK ? 0 : -EIO;
}

struct audio_session { pacha_fd_t fd; uint64_t handle; };
struct audio_generation {
    uint64_t id, sequence;
    struct pacha_ipc_channel_pair management;
    struct ph_ipc ipc;
    struct gpud_native_launch launch;
    struct gpud_process_watch watch;
    struct gpud_lifecycle lifecycle;
    struct ph_audio_request *shared;
    pacha_fd_t shared_fd;
    struct audio_session sessions[AUDIO_SESSION_LIMIT];
};
static int start_generation(kb2_controller_t *controller, struct usbd_package *package,
    struct audio_generation *current, const struct audiod_boot_config *config) {
    (void)config;
    memset(current, 0, sizeof(*current));
    current->id = kb2_controller_generation(controller);
    current->management = (struct pacha_ipc_channel_pair){.a = -1, .b = -1};
    const uint64_t generation = current->id;
    int error = usbd_package_bind(package, generation);
    if (!error)
        error = complete_action(controller, KB2_ACTION_ALLOCATE_RESOURCES,
            AUDIOD_RESOURCE_SET_ID, 0);
    if (error)
        return error;

    struct ph_sandbox_config sandbox_config = {.size = sizeof(struct ph_sandbox_config),
        .identity = package->identity,
        .artifact_count = package->artifact_count, .resource_count = 1,
        .device = package->device};
    const struct ph_owner_boot_config start = {.controller = controller,
        .launch = &current->launch, .ipc = &current->ipc,
        .management = &current->management, .watch = &current->watch,
        .lifecycle = &current->lifecycle,
        .resource_set_id = AUDIOD_RESOURCE_SET_ID, .sandbox_id = AUDIOD_SANDBOX_ID,
        .sandbox_config = &sandbox_config, .image = package->sandbox,
        .image_size = package->sandbox_size,
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
            return 0;
        }
        if (status != KB2_STATUS_ACTION_PENDING ||
            wait_readable(current->ipc.fd, current->launch.process.fd))
            return -EIO;
    }
}

static int restart_generation(kb2_controller_t *controller,
    struct usbd_package *package, struct audio_generation *current) {
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

static int audio_rpc(struct audio_generation *current, pacha_fd_t notice_fd) {
    const uint64_t operation = current->shared ? PH_AUDIO_EXECUTE : PH_AUDIO_ATTACH;
    struct ph_ipc_packet packet = {.operation = operation, .generation = current->id,
        .correlation = ++current->sequence};
    if (notice_fd >= 16) {
        packet.fd_count = 1;
        packet.fds[0] = (struct pacha_ipc_fd){.fd = (uint64_t)notice_fd,
            .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_SEND};
    }
    int error = ph_ipc_send(&current->ipc, &packet);
    while (!error) {
        struct ph_ipc_packet reply = {0};
        error = ph_ipc_receive(&current->ipc, current->id, &reply);
        if (error == -EAGAIN) { error = wait_readable(current->ipc.fd, current->launch.process.fd); continue; }
        if (!error && (reply.operation != operation || reply.correlation != packet.correlation ||
                reply.fd_count || reply.value)) error = -EPROTO;
        int closed = ph_ipc_packet_release(&reply);
        return error ? error : closed;
    }
    return error;
}
static int attach(struct audio_generation *current) {
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    current->shared_fd = pacha_vmo_create(AUDIO_STAGING_BYTES, rights, 0);
    if (current->shared_fd < 16) return -ENOMEM;
    current->shared = pacha_mmap(current->shared_fd, AUDIO_STAGING_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!current->shared) return -ENOMEM;
    struct ph_ipc_packet packet = {.operation = PH_AUDIO_ATTACH, .generation = current->id,
        .correlation = ++current->sequence, .value = AUDIO_STAGING_BYTES, .fd_count = 1,
        .fds = {{.fd = (uint64_t)current->shared_fd, .rights = rights & ~PACHA_FD_RIGHT_INSPECT}}};
    int error = ph_ipc_send(&current->ipc, &packet);
    while (!error) {
        struct ph_ipc_packet reply = {0};
        error = ph_ipc_receive(&current->ipc, current->id, &reply);
        if (error == -EAGAIN) { error = wait_readable(current->ipc.fd, current->launch.process.fd); continue; }
        if (!error && (reply.operation != packet.operation || reply.correlation != packet.correlation ||
            reply.fd_count || reply.value)) error = -EPROTO;
        int closed = ph_ipc_packet_release(&reply);
        return error ? error : closed;
    }
    return error;
}
static void close_received(const struct pacha_ipc_msg *request, pacha_fd_t keep) {
    for (size_t i = 0; i < request->fd_count; ++i)
        if ((pacha_fd_t)request->fds[i].fd != keep)
            (void)pacha_fd_close((pacha_fd_t)request->fds[i].fd);
}
static void answer(const struct pacha_ipc_msg *request, int64_t status, uint64_t result) {
    const struct pacha_ipc_msg reply = {.word0 = AUDIOD_MAGIC,
        .word1 = (uint64_t)status, .word2 = result, .word3 = request->word3};
    (void)pacha_ipc_reply(PACHA_IPC_REPLY_IMPLICIT, &reply);
}
static int session_close(struct audio_generation *current, unsigned slot) {
    struct audio_session *session = &current->sessions[slot];
    *current->shared = (struct ph_audio_request){.operation = AUDIO_CLOSE,
        .slot = slot, .handle = session->handle};
    int error = audio_rpc(current, -1);
    if (!error && current->shared->result) error = (int)current->shared->result;
    if (error) return error;
    (void)pacha_fd_close(session->fd); *session = (struct audio_session){0}; return 0;
}
static int dispatch(struct audio_generation *current, pacha_fd_t endpoint, int slot) {
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS] = {0};
    struct pacha_ipc_msg request = {.fds = fds, .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS};
    int received = pacha_ipc_recv(endpoint, &request);
    if (received == PACHA_ERR_EMPTY || received == PACHA_ERR_NOT_READY) return 0;
    if (received) return -EPIPE;
    int64_t status = -EINVAL; uint64_t result = 0; pacha_fd_t keep = -1;
    if (request.word0 != AUDIOD_MAGIC || !request.word3) goto reply;
    if (slot < 0) {
        if (request.word1 != AUDIO_OPEN || request.fd_count != 1 ||
            request.word2 > UINT32_MAX) goto reply;
        struct pacha_fd_info info = {0};
        const pacha_fd_t fd = (pacha_fd_t)fds[0].fd;
        const uint64_t needed = AUDIOD_SERVER_RIGHTS;
        if (pacha_fd_get_info(fd, &info) || info.kind != PACHA_FD_KIND_CHANNEL ||
            (info.rights & needed) != needed) goto reply;
        for (slot = 0; slot < AUDIO_SESSION_LIMIT && current->sessions[slot].fd >= 16; ++slot) { }
        if (slot == AUDIO_SESSION_LIMIT) { status = -EMFILE; goto reply; }
        *current->shared = (struct ph_audio_request){.operation = AUDIO_OPEN,
            .slot = (uint32_t)slot, .argument = request.word2};
        status = audio_rpc(current, fd);
        if (status) { close_received(&request, -1); answer(&request, status, 0); return (int)status; }
        status = current->shared->result;
        if (!status) {
            current->sessions[slot] = (struct audio_session){.fd = fd, .handle = current->shared->handle};
            keep = fd;
        }
    } else {

        const unsigned op = (unsigned)request.word1;
        if (request.word1 != op || (op != AUDIO_WRITE && op != AUDIO_IOCTL && op != AUDIO_POLL && op != AUDIO_FLAGS)) goto reply;
        const size_t bytes = op == AUDIO_WRITE ? (size_t)request.word2 :
            op == AUDIO_IOCTL ? (request.word2 >> 16) & 0x3fff : 0;
        if (bytes > AUDIO_DATA_BYTES || (op == AUDIO_IOCTL && request.word2 > UINT32_MAX) ||
            request.fd_count != (bytes ? 1u : 0u)) goto reply;

        *current->shared = (struct ph_audio_request){.operation = op, .slot = (uint32_t)slot,
            .handle = current->sessions[slot].handle, .argument = request.word2, .size = bytes};
        void *data = NULL;
        if (bytes) {
            struct pacha_fd_info info = {0};
            if (pacha_fd_get_info((pacha_fd_t)fds[0].fd, &info) || info.kind != PACHA_FD_KIND_VMO ||
                info.size != AUDIO_DATA_BYTES) goto reply;

            data = pacha_mmap((pacha_fd_t)fds[0].fd, AUDIO_DATA_BYTES,
                PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
            if (!data) { status = -EFAULT; goto reply; }
            memcpy(current->shared->data, data, bytes);
        }

        status = audio_rpc(current, -1);
        if (!status) {

            const int64_t value = current->shared->result;
            if (value < 0) status = value;
            else {
                result = (uint64_t)value;
                if (data && op == AUDIO_IOCTL) memcpy(data, current->shared->data, bytes);
            }
        }
        if (data) (void)pacha_munmap(data, AUDIO_DATA_BYTES);
    }
reply:
    if (status < 0) {
        printf("[audiod] PCM request op=%llu argument=0x%llx status=%lld\n",
            (unsigned long long)request.word1, (unsigned long long)request.word2,
            (long long)status); fflush(stdout);
    }
    answer(&request, status, result); close_received(&request, keep); return 0;
}
static int serve(struct audio_generation *current, const struct audiod_boot_config *config) {
    for (;;) {
        struct pacha_pollfd wait[AUDIO_SESSION_LIMIT + 2] = {
            {.fd = current->launch.process.fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
            {.fd = (pacha_fd_t)config->endpoint_fd, .events = PACHA_FD_EVENT_READABLE},
        };
        unsigned slots[AUDIO_SESSION_LIMIT], count = 2;
        for (unsigned i = 0; i < AUDIO_SESSION_LIMIT; ++i) if (current->sessions[i].fd >= 16) {
            slots[count - 2] = i;
            wait[count++] = (struct pacha_pollfd){.fd = current->sessions[i].fd,
                .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
        }
        long woke = pacha_fd_wait_many(wait, count, PACHA_FD_WAIT_FOREVER);
        if (woke < 0 || wait[0].revents) return -EPIPE;
        /* Reap last-close before accepting a new open of the same PCM. */
        for (unsigned i = 2; i < count; ++i) {
            int error = 0;
            if (wait[i].revents & PACHA_FD_EVENT_HANGUP) error = session_close(current, slots[i-2]);
            else if (wait[i].revents & PACHA_FD_EVENT_READABLE) error = dispatch(current, wait[i].fd, (int)slots[i-2]);
            if (error) return error;
        }
        if (wait[1].revents) { int error = dispatch(current, (pacha_fd_t)config->endpoint_fd, -1); if (error) return error; }
    }
}
static uint32_t wire32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int publish_pcm(pacha_fd_t filed) {
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_MKNODAT,
        .mode = 0020660, .length = (14u << 8) | 3u};
    kb2_fs_response_t response;
    int error = filedfs_call(filed, &request, "/dev/dsp", NULL, NULL, 0,
        NULL, 0, &response, NULL, 0, NULL, NULL);
    if (error != -EEXIST) return error;
    uint8_t stat[KB2_FILESYSTEM_STATX_SIZE];
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_STATX, .mask = 0x7ff, .flags = 0x100};
    error = filedfs_call(filed, &request, "/dev/dsp", NULL, NULL, 0,
        stat, sizeof(stat), &response, NULL, 0, NULL, NULL);
    if (error) return error;
    unsigned mode = stat[KB2_FILESYSTEM_STATX_MODE_OFFSET] | stat[KB2_FILESYSTEM_STATX_MODE_OFFSET+1] << 8;
    return response.data_length == sizeof(stat) && (mode & 0170000) == 0020000 &&
        wire32(stat + KB2_FILESYSTEM_STATX_RDEV_MAJOR_OFFSET) == 14 &&
        wire32(stat + KB2_FILESYSTEM_STATX_RDEV_MINOR_OFFSET) == 3 ? 0 : -EEXIST;
}
static int send_ready(const struct audiod_boot_config *config, int status) {
    const struct pacha_ipc_msg ready = {.word0 = AUDIOD_READY_MAGIC, .word1 = (uint64_t)(int64_t)status};
    return pacha_ipc_send((pacha_fd_t)config->ready_fd, &ready);
}
static int run(const struct audiod_boot_config *config) {
    struct usbd_package package = {0};
    int error = usbd_package_open_root(&package, (pacha_fd_t)config->filed_fd,
        (pacha_fd_t)config->device_fd, "/srv/kobox2/virtio-sound");
    if (error) { printf("[audiod] package status=%d\n", error); return error; }
    kb2_controller_t *controller = NULL;
    if (kb2_controller_create(controller_allocate, controller_deallocate, NULL, &controller) != KB2_STATUS_OK ||
        (error = usbd_package_configure_controller(&package, controller)) ||
        kb2_controller_start(controller) != KB2_STATUS_OK) return error ? error : -EIO;
    for (;;) {
        static struct audio_generation current;
        error = start_generation(controller, &package, &current, config);
        if (!error) error = attach(&current);
        if (error) { printf("[audiod] start status=%d\n", error); return error; }
        if (current.id == 1 && ((error = publish_pcm((pacha_fd_t)config->filed_fd)) ||
            (error = send_ready(config, 0)))) return error;
        printf("[audiod] virtio-sound PCM ready generation=%llu modules=%u\n",
            (unsigned long long)current.id, (unsigned)(package.artifact_count - 1)); fflush(stdout);
        error = serve(&current, config);
        printf("[audiod] sandbox failed status=%d; revoke and restart\n", error); fflush(stdout);
        for (unsigned i = 0; i < AUDIO_SESSION_LIMIT; ++i)
            if (current.sessions[i].fd >= 16) (void)pacha_fd_close(current.sessions[i].fd);
        if (current.shared) (void)pacha_munmap(current.shared, AUDIO_STAGING_BYTES);
        if (current.shared_fd >= 16) (void)pacha_fd_close(current.shared_fd);
        if ((error = restart_generation(controller, &package, &current))) return error;
    }
}
int main(int argc, char **argv) {
    (void)argc;
    struct audiod_boot_config config = {0};
    pacha_fd_t bootstrap = pacha_bootstrap_fd_from_argv(argv);
    if (bootstrap < 16 || pacha_fd_read(bootstrap, &config, sizeof(config)) != sizeof(config) ||
        config.magic != AUDIOD_BOOT_MAGIC || config.size != sizeof(config) ||
        config.device_fd < 16 || config.filed_fd < 16 || config.endpoint_fd < 16 || config.ready_fd < 16)
        return 1;
    int error = run(&config);
    fprintf(stderr, "[audiod] stopped status=%d\n", error); (void)send_ready(&config, error); return 1;
}

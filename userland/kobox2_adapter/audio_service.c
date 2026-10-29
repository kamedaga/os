/* SPDX-License-Identifier: MIT */
#include "audio_service.h"
#include "host.h"
#include <errno.h>
#include <string.h>
static int notify_audio(void *context) {
    struct ph_audio_slot *slot = context;
    const struct ph_ipc_notice notice = {.fd = slot->notify_fd};
    return ph_ipc_notify(&notice);
}
static int prepare(void *context, struct ph_ipc_packet *packet) {
    struct ph_audio_service *audio = context;
    if (!packet->correlation || packet->generation != audio->generation || audio->operation)
        return -EPROTO;
    if (packet->operation == PH_AUDIO_ATTACH) {
        struct pacha_fd_info info = {0};
        if (audio->shared || packet->fd_count != 1 || packet->value != AUDIO_STAGING_BYTES ||
            pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, packet->fds[0].fd, (uintptr_t)&info) ||
            info.kind != PACHA_FD_KIND_VMO || info.size != AUDIO_STAGING_BYTES)
            return -EPROTO;
        long mapped = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, packet->fds[0].fd, 0,
            AUDIO_STAGING_BYTES, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
        if (mapped < 4096) return -ENOMEM;
        audio->shared = (void *)(uintptr_t)mapped;
    } else if (packet->operation == PH_AUDIO_EXECUTE) {
        if (!audio->shared || packet->value) return -EPROTO;
        memcpy(&audio->request, audio->shared, sizeof(audio->request));
        if (audio->request.slot >= AUDIO_SESSION_LIMIT || audio->request.size > AUDIO_DATA_BYTES)
            return -EPROTO;
        const unsigned expected = audio->request.operation == AUDIO_OPEN;
        if (packet->fd_count != expected) return -EPROTO;
        if (expected) {
            struct pacha_fd_info info = {0};
            if (pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, packet->fds[0].fd, (uintptr_t)&info) ||
                info.kind != PACHA_FD_KIND_CHANNEL || !(info.rights & PACHA_FD_RIGHT_SEND))
                return -EPROTO;
            audio->pending_fd = (pacha_fd_t)packet->fds[0].fd;
            packet->fds[0].fd = PH_IPC_NO_FD;
        }
    } else return -EPROTO;
    audio->operation = packet->operation; audio->correlation = packet->correlation;
    return 0;
}
static int dispatch(void *context, void *port) {
    struct ph_audio_service *audio = context;
    if (audio->operation == PH_AUDIO_ATTACH) return 0;
    struct ph_audio_request *request = &audio->request;
    struct ph_audio_slot *slot = &audio->slots[request->slot];
    long result = -EBADF;
    if (request->operation == AUDIO_OPEN) {
        if (slot->handle || audio->pending_fd < 16) result = -EBUSY;
        else {
            const struct kobox_linux_audio_notify notify = {.context = slot, .notify = notify_audio};
            slot->notify_fd = audio->pending_fd;
            result = audio->open(port, (uint32_t)request->argument, &notify, &slot->handle);
            if (!result) audio->pending_fd = -1;
            else slot->notify_fd = -1;
        }
    } else if (slot->handle && request->handle == slot->handle) {
        switch (request->operation) {
        case AUDIO_CLOSE:
            result = audio->close(port, slot->handle);
            if (!result) {
                /* The Linux wait callbacks are detached before FD retirement. */
                PH_OK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, slot->notify_fd));
                slot->notify_fd = -1; slot->handle = 0;
            }
            break;
        case AUDIO_WRITE:
            result = audio->write(port, slot->handle, request->data, request->size); break;
        case AUDIO_IOCTL:
            result = audio->ioctl(port, slot->handle, (uint32_t)request->argument,
                request->data, request->size); break;
        case AUDIO_FLAGS: result = audio->flags(port, slot->handle, request->argument); break;
        case AUDIO_POLL: result = audio->poll(port, slot->handle); break;
        default: result = -EINVAL; break;
        }
    }
    request->result = result; request->handle = slot->handle;
    memcpy(audio->shared, request, sizeof(*request));
    return 0;
}
static int complete(void *context, struct ph_ipc *ipc) {
    struct ph_audio_service *audio = context;
    struct ph_ipc_packet reply = {.operation = audio->operation,
        .generation = audio->generation, .correlation = audio->correlation};
    return ph_ipc_send(ipc, &reply);
}
static int release(void *context) {
    struct ph_audio_service *audio = context;
    if (audio->pending_fd >= 16) {
        PH_OK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, audio->pending_fd));
        audio->pending_fd = -1;
    }
    audio->operation = 0; audio->correlation = 0; return 0;
}
static int stop(void *context) {
    struct ph_audio_service *audio = context;
    /* The module-launch teardown closes the port and quiesces all callbacks
     * after lifecycle stop. Keep its notice capabilities until process exit. */
    if (audio->shared) {
        PH_OK(pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, (uintptr_t)audio->shared, AUDIO_STAGING_BYTES));
        audio->shared = NULL;
    }
    return release(context);
}
int ph_audio_service_init(struct ph_audio_service *audio, void *core,
    uint64_t generation, struct ph_lifecycle_service *service) {
    if (!audio || !core || !generation || !service) return -EINVAL;
    memset(audio, 0, sizeof(*audio)); audio->pending_fd = -1;
#define BIND(field, symbol) do { void *address = ph_image_lookup(core, symbol); \
    if (!address) return -ENOENT; memcpy(&audio->field, &address, sizeof(address)); } while (0)
    BIND(open, "kobox_linux_audio_open"); BIND(close, "kobox_linux_audio_close");
    BIND(write, "kobox_linux_audio_write"); BIND(ioctl, "kobox_linux_audio_ioctl");
    BIND(poll, "kobox_linux_audio_poll"); BIND(flags, "kobox_linux_audio_flags");
#undef BIND
    audio->generation = generation;
    *service = (struct ph_lifecycle_service){.context = audio, .prepare = prepare,
        .dispatch = dispatch, .complete = complete, .release = release, .stop = stop};
    return 0;
}

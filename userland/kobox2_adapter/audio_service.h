/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_AUDIO_SERVICE_H
#define PACHA_KOBOX_AUDIO_SERVICE_H
#include "lifecycle.h"
#include "../audiod/include/audiod/protocol.h"
#include "boot/audio_port.h"
struct ph_audio_slot { pacha_fd_t notify_fd; uint64_t handle; };
struct ph_audio_service {
    struct ph_audio_request *shared, request;
    uint64_t generation, operation, correlation;
    struct ph_audio_slot slots[AUDIO_SESSION_LIMIT];
    pacha_fd_t pending_fd;
    int (*open)(void *, uint32_t, const struct kobox_linux_audio_notify *, uint64_t *);
    int (*close)(void *, uint64_t);
    long (*write)(void *, uint64_t, const void *, size_t);
    int (*ioctl)(void *, uint64_t, uint32_t, void *, size_t);
    int (*poll)(void *, uint64_t);
    int (*flags)(void *, uint64_t, uint64_t);
};
int ph_audio_service_init(struct ph_audio_service *, void *, uint64_t,
    struct ph_lifecycle_service *);
#endif

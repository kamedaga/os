/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_DRM_SERVICE_H
#define PACHA_GPUD_DRM_SERVICE_H

#include "drm_files.h"
#include "gpu_rpc.h"
#include "../../kobox2/linux-sandbox/kobox/boot/drm_limits.h"

struct gpud_drm_watch {
    uint64_t handle, device_minor;
    int fd;
    unsigned int transferred, submit_reported, modeset_reported;
    unsigned int dirty_probe_reported, dirty_update_reported;
};

enum {
    GPUD_DRM_MAPPINGS_MAX = KOBOX_DRM_MAPPING_LIMIT,
    GPUD_DRM_PRIMES_MAX = KOBOX_DRM_PRIME_LIMIT,
    /* Mapping and PRIME pools are independent. Allow a reference per object
     * plus a handoff reference while fork/exec transfers ownership. */
    GPUD_DRM_OBJECT_LEASES_MAX =
        2 * (GPUD_DRM_MAPPINGS_MAX + GPUD_DRM_PRIMES_MAX),
    GPUD_DRM_WAIT_SOURCES_MAX =
        GPUD_DRM_REFERENCES_MAX + GPUD_DRM_OBJECT_LEASES_MAX,
    GPUD_DRM_EVENT_BACKLOG_BYTES = 4096,
};

struct gpud_drm_mapping {
    uint64_t id, exchange, handle, length;
    uint32_t rights, cache_policy, gem_handle;
    int view_fd;
    unsigned int owner_closed;
};

struct gpud_drm_prime {
    uint64_t token, length;
    int view_fd;
    unsigned int owner_closed;
};

struct gpud_drm_object_lease {
    uint64_t object_id;
    int fd;
    unsigned int prime_owner;
};

struct gpud_drm_event_buffer {
    uint64_t handle;
    size_t bytes;
    unsigned char data[GPUD_DRM_EVENT_BACKLOG_BYTES];
};

struct gpud_drm_connection {
    struct gpud_drm_connection *next;
    void *page;
    int fd;
    unsigned ready;
    void *aux;
};

/* filed supplies one process-lifetime endpoint. Each OPEN/DUP transfers a
 * notification capability that owns one frontend reference; request payloads
 * never supply lifetime authority. Call only after sandbox READY and channel
 * binding. No Linux ABI enters the sandbox. */
struct gpud_drm_service {
    struct gpud_drm_files files;
    struct gpud_gpu_rpc gpu;
    uint64_t backend_client, correlation, event_sequence;
    uint64_t resource_creates, gem_closes, exec_submits;
    struct gpud_drm_pending_fence *fences;
    struct gpud_drm_connection *connections;
    struct pacha_pollfd *pollfds;
    size_t poll_capacity;
    unsigned prefer_connection;
    size_t mapping_high_water;
    struct gpud_drm_watch watches[GPUD_DRM_REFERENCES_MAX];
    struct gpud_drm_mapping mappings[GPUD_DRM_MAPPINGS_MAX];
    struct gpud_drm_prime primes[GPUD_DRM_PRIMES_MAX];
    struct gpud_drm_object_lease object_leases[GPUD_DRM_OBJECT_LEASES_MAX];
    struct gpud_drm_event_buffer events[GPUD_DRM_FILES_MAX];
    /* Received capabilities and mappings remain owned here if cleanup fails.
     * The launch owner must retire the process, not reuse failed state. */
    struct ph_ipc_packet received;
    void *page;
    void *request_aux;
    int endpoint_fd, error;
};

/* Binds this generation to filed's process-lifetime DRM endpoint. The
 * endpoint is borrowed so a replacement generation can bind it again. */
int gpud_drm_service_bind(struct gpud_drm_service *service, int endpoint_fd);
/* Receives and replies to one existing LPR native IPC_CALL. -EAGAIN means no
 * request; other errors stop the service. A Linux operation error is sent in
 * the reply, not returned here. CLOSE waits for the real backend completion. */
int gpud_drm_service_receive(struct gpud_drm_service *service);
/* Drain completed sandbox event-lane messages into the handle-local backlog
 * and signal the original LPR notification channel on empty->nonempty. */
int gpud_drm_service_pump_events(struct gpud_drm_service *service);
/* Transfer leases and the original open notification each own one frontend
 * reference. HANGUP drops only that reference and closes the backend session
 * synchronously when it was the last one. */
int gpud_drm_service_reap_hangups(struct gpud_drm_service *service);
/* Generation retirement revokes every exported view and closes all lease
 * endpoints. The stopped sandbox already quiesced its Linux mapping ledger. */
int gpud_drm_service_retire_mappings(struct gpud_drm_service *service);
/* Returns the full required count even when capacity is smaller. */
size_t gpud_drm_service_pollfds(const struct gpud_drm_service *service,
    struct pacha_pollfd *fds, size_t capacity);
int gpud_drm_service_reserve_pollfds(struct gpud_drm_service *service, size_t extra);

#endif

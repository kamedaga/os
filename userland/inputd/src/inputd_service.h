#pragma once

#include "input_island.h"

#include <pacha/ipc.h>

struct inputd_uevent;
typedef struct inputd_service {
    const struct inputd_boot_config *cfg;
    struct inputd_input_island *input;
    void *sources;
    size_t source_count;
    /* The service thread owns this FIFO. Retain an event until SEND commits;
     * netd startup and its queue pressure must never block input RPCs. */
    struct inputd_uevent *uevent_head, *uevent_tail;
    int uevent_error;
} inputd_service_t;

int inputd_service_flush_uevents(inputd_service_t *service);
int inputd_service_uevents_pending(const inputd_service_t *service);
void inputd_service_uevent_hangup(inputd_service_t *service);
int inputd_service_send_boot_ready(inputd_service_t *service, int64_t status, uint64_t result);
int inputd_service_publish_startup_devices(inputd_service_t *service);
int inputd_service_dispatch(
    inputd_service_t *service,
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds);
int inputd_service_source_dispatch(
    inputd_service_t *service,
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds);

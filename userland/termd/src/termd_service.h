#pragma once

#include "linux_tty_island.h"
#include "termd/boot_config.h"

#include <pacha/ipc.h>

#include <stdint.h>

enum {
    TERMD_ERR_INVAL = -22,
    TERMD_ERR_NODEV = -19,
    TERMD_ERR_NOTSUP = -95,
    TERMD_ERR_AGAIN = -11,
};

typedef struct termd_service {
    const struct termd_boot_config *cfg;
    struct termd_linux_tty_island *tty;
    int signal_supervisor_endpoint_fd;
    uint64_t signal_supervisor_request_id;
} termd_service_t;

void termd_service_init(
    termd_service_t *service,
    const struct termd_boot_config *cfg,
    struct termd_linux_tty_island *tty);

int termd_service_send_boot_ready(termd_service_t *service, int64_t status, uint64_t result);
int termd_service_dispatch_request(
    termd_service_t *service,
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds);
void termd_service_forward_pending_tty_signals(termd_service_t *service);

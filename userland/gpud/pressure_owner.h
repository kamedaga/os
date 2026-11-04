/* SPDX-License-Identifier: MIT */
#pragma once
#include <pacha/pressure.h>
#include "../kobox2_adapter/ipc.h"
/* The boot parent grants a private, send-only registry capability. The
 * liveness peer belongs only to this owner, never to clients or the child. */
static int gpud_export_pressure(struct ph_ipc *ipc, pacha_fd_t process,
    pacha_fd_t *liveness) {
    struct pacha_fd_info info = {0};
    if (pacha_fd_get_info(PACHA_PRESSURE_EXPORT_FD, &info)) return 0;
    if (info.kind != PACHA_FD_KIND_CHANNEL) return -22;
    if (*liveness >= 16) { (void)pacha_fd_close(*liveness); *liveness = -1; }
    struct pacha_ipc_channel_pair pressure = {.a=-1,.b=-1}, life = {.a=-1,.b=-1};
    int error = pacha_ipc_channel_create(&pressure, PACHA_PRESSURE_RIGHTS | PACHA_FD_RIGHT_TRANSFER, 0);
    if (!error) error = pacha_ipc_channel_create(&life, PACHA_PRESSURE_RIGHTS | PACHA_FD_RIGHT_TRANSFER, 0);
    if (!error) {
        struct ph_ipc_packet bind = {.operation=PACHA_PRESSURE_BIND,
            .generation=ipc->generation, .fd_count=1,
            .fds={{.fd=pressure.b,.rights=PACHA_PRESSURE_RIGHTS,.flags=PACHA_FD_FLAG_CLOEXEC}}};
        error = ph_ipc_send(ipc, &bind);
    }
    if (!error) {
        uint64_t watch = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_WAIT |
            PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
        struct pacha_ipc_fd fds[3] = {
            {.fd=pressure.a,.rights=PACHA_PRESSURE_RIGHTS,.flags=PACHA_FD_FLAG_CLOEXEC},
            {.fd=process,.rights=watch,.flags=PACHA_FD_FLAG_CLOEXEC},
            {.fd=life.b,.rights=watch,.flags=PACHA_FD_FLAG_CLOEXEC}};
        struct pacha_ipc_msg message = {.word0=PACHA_PRESSURE_REGISTER,
            .word1=ipc->generation,.fds=fds,.fd_count=3};
        error = pacha_ipc_send(PACHA_PRESSURE_EXPORT_FD, &message);
    }
    if (!error) { *liveness=life.a; life.a=-1; }
    if (pressure.a>=16) (void)pacha_fd_close(pressure.a);
    if (pressure.b>=16) (void)pacha_fd_close(pressure.b);
    if (life.a>=16) (void)pacha_fd_close(life.a);
    if (life.b>=16) (void)pacha_fd_close(life.b);
    return error;
}

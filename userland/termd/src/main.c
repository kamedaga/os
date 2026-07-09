#include "termd/boot_config.h"
#include "termd_service.h"

#include <pacha/abi.h>
#include <pacha/ipc.h>
#include <pacha/trace.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const struct termd_boot_config *cfg =
        (const struct termd_boot_config *)(uintptr_t)TERMD_BOOT_CONFIG_VA;
    if (cfg == 0 ||
        cfg->magic != TERMD_BOOT_CONFIG_MAGIC ||
        cfg->tty_endpoint_fd < 16 ||
        cfg->ready_channel_fd < 16) {
        pacha_trace0(PACHA_TRACE_COMPONENT_TERMD, PACHA_TRACE_EVENT_TERMD_BOOT_CONFIG_INVALID, PACHA_TRACE_CLASS_ERROR);
        return 1;
    }

    static struct termd_linux_tty_island tty_island;
    termd_service_t service;
    termd_service_init(&service, cfg, &tty_island);

    const int island_status = termd_linux_tty_island_init(&tty_island, cfg);
    if (island_status != 0) {
        pacha_trace1(PACHA_TRACE_COMPONENT_TERMD, PACHA_TRACE_EVENT_TERMD_ISLAND_INIT, PACHA_TRACE_CLASS_ERROR, (uint64_t)island_status);
    }

    if (tty_island.ready) {
        printf(
            "[termd] linux tty modules ready endpoint_fd=%llu loader=%s modules=%u loaded=%u sources=%u ptmx=%s\n",
            (unsigned long long)cfg->tty_endpoint_fd,
            tty_island.loader_version,
            (unsigned)tty_island.configured_module_count,
            (unsigned)tty_island.loaded_module_count,
            (unsigned)tty_island.source_count,
            tty_island.ptmx_registered ? "registered" : "missing");
    } else {
        printf(
            "[termd] linux tty modules not ready endpoint_fd=%llu loader=%s modules=%u loaded=%u load_status=%d init_status=%d\n",
            (unsigned long long)cfg->tty_endpoint_fd,
            tty_island.loader_version,
            (unsigned)tty_island.configured_module_count,
            (unsigned)tty_island.loaded_module_count,
            (int)tty_island.load_status,
            (int)tty_island.init_status);
    }
    fflush(stdout);

    const int ready_status =
        island_status != 0 ? island_status :
        (tty_island.ready && tty_island.source_count != 0 ? 0 : TERMD_ERR_NODEV);
    const int ready_send_status =
        termd_service_send_boot_ready(&service, ready_status, tty_island.source_count);
    if (ready_send_status != 0) {
        pacha_trace2(PACHA_TRACE_COMPONENT_TERMD, PACHA_TRACE_EVENT_TERMD_BOOT_READY_SEND, PACHA_TRACE_CLASS_ERROR, (uint64_t)ready_send_status, (uint64_t)ready_status);
        return 1;
    }
    if (ready_status != 0) {
        return 1;
    }

    for (;;) {
        struct pacha_ipc_msg request;
        struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
        memset(&request, 0, sizeof(request));
        memset(fds, 0, sizeof(fds));
        request.fds = fds;
        request.fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS;

        int status = pacha_ipc_recv(cfg->tty_endpoint_fd, &request);
        if (status == 0) {
            (void)termd_service_dispatch_request(&service, &request, fds);
            termd_service_forward_pending_tty_signals(&service);
        } else if (status != PACHA_ERR_EMPTY && status != PACHA_ERR_NOT_READY) {
            pacha_trace1(PACHA_TRACE_COMPONENT_TERMD, PACHA_TRACE_EVENT_TERMD_RECV, PACHA_TRACE_CLASS_ERROR, (uint64_t)status);
        } else {
            termd_service_forward_pending_tty_signals(&service);
        }
    }
}

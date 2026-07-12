#include <inputd/boot_config.h>
#include "inputd_service.h"

#include <pacha/abi.h>
#include <pacha/ipc.h>
#include <kobox/shim.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
    const struct inputd_boot_config *cfg =
        (const struct inputd_boot_config *)(uintptr_t)INPUTD_BOOT_CONFIG_VA;
    if (cfg == NULL || cfg->magic != INPUTD_BOOT_CONFIG_MAGIC ||
        cfg->input_endpoint_fd < 16 || cfg->ready_channel_fd < 16 ||
        cfg->device_count != INPUTD_DEVICE_COUNT) return 1;
    static struct inputd_input_island island;
    inputd_service_t service = { .cfg = cfg, .input = &island };
    const int init_status = inputd_input_island_init(&island, cfg);
    printf("[inputd] init status=%d modules=%u devices=%u ready=%d\n",
        init_status, island.loaded_module_count, island.device_count, island.ready);
    const int ready_status = inputd_service_send_boot_ready(
        &service, init_status, island.ready ? island.device_count : 0);
    if (ready_status != 0 || init_status != 0) return 1;
    printf("[inputd] service loop endpoint_fd=%llu\n",
        (unsigned long long)cfg->input_endpoint_fd);
    for (;;) {
        struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
        struct pacha_ipc_msg request;
        memset(fds, 0, sizeof(fds));
        memset(&request, 0, sizeof(request));
        request.fds = fds;
        request.fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS;
        const int status = pacha_ipc_recv((int)cfg->input_endpoint_fd, &request);
        int wait_for_work = 0;
        if (status == 0) {
            (void)inputd_service_dispatch(&service, &request, fds);
        } else if (status == PACHA_ERR_EMPTY || status == PACHA_ERR_NOT_READY) {
            wait_for_work = 1;
        } else {
            fprintf(stderr, "[inputd] service receive failed status=%d endpoint_fd=%llu\n",
                status, (unsigned long long)cfg->input_endpoint_fd);
            return 1;
        }
        inputd_input_island_pump(&island);
        if (wait_for_work) {
            struct pacha_pollfd pollfd = {
                .fd = (int)cfg->input_endpoint_fd,
                .events = PACHA_FD_EVENT_READABLE,
                .revents = 0,
            };
            (void)pacha_fd_wait_many(&pollfd, 1, PACHA_FD_WAIT_FOREVER);
        }
    }
}

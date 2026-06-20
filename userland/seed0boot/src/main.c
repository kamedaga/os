#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pacha/ipc.h"
#include "pachaos_capsule_launcher.h"

struct seed0_service {
    const char *name;
    unsigned flags;
    int control_fd;
    int peer_control_fd;
    int endpoint_fd;
};

static const uint64_t seed0_endpoint_rights =
    PACHA_FD_RIGHT_INSPECT |
    PACHA_FD_RIGHT_WAIT |
    PACHA_FD_RIGHT_POLL |
    PACHA_FD_RIGHT_CLOSE |
    PACHA_FD_RIGHT_SEND |
    PACHA_FD_RIGHT_RECV |
    PACHA_FD_RIGHT_CALL |
    PACHA_FD_RIGHT_TRANSFER;

static const uint64_t seed0_channel_rights =
    PACHA_FD_RIGHT_INSPECT |
    PACHA_FD_RIGHT_WAIT |
    PACHA_FD_RIGHT_POLL |
    PACHA_FD_RIGHT_CLOSE |
    PACHA_FD_RIGHT_SEND |
    PACHA_FD_RIGHT_RECV |
    PACHA_FD_RIGHT_CALL |
    PACHA_FD_RIGHT_TRANSFER;

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("[seed0boot] start\n");

    enum { seed0_service_count = 1 };
    struct seed0_service *services = calloc(seed0_service_count, sizeof(*services));
    if (!services) {
        fprintf(stderr, "[seed0boot] calloc failed\n");
        return 1;
    }
    services[0].name = "seed0-root";
    services[0].flags = 0;
    services[0].control_fd = -1;
    services[0].peer_control_fd = -1;
    services[0].endpoint_fd = pacha_ipc_endpoint_create(seed0_endpoint_rights, 0);
    if (services[0].endpoint_fd < 0) {
        fprintf(stderr, "[seed0boot] endpoint create failed status=%d\n", services[0].endpoint_fd);
        return 10;
    }

    struct pacha_ipc_channel_pair control_pair = { .a = -1, .b = -1 };
    if (pacha_ipc_channel_create(&control_pair, seed0_channel_rights, 0) != 0 || control_pair.a < 16 || control_pair.b < 16) {
        fprintf(stderr, "[seed0boot] control channel create failed a=%d b=%d\n", control_pair.a, control_pair.b);
        return 11;
    }
    services[0].control_fd = control_pair.a;
    services[0].peer_control_fd = control_pair.b;

    struct pacha_fd_info endpoint_info = {0};
    struct pacha_fd_info control_info = {0};
    if (pacha_fd_get_info(services[0].endpoint_fd, &endpoint_info) != 0 ||
        pacha_fd_get_info(services[0].control_fd, &control_info) != 0 ||
        endpoint_info.kind != PACHA_FD_KIND_ENDPOINT ||
        control_info.kind != PACHA_FD_KIND_CHANNEL) {
        fprintf(stderr,
            "[seed0boot] ipc fd info failed endpoint_fd=%d kind=%llu control_fd=%d kind=%llu\n",
            services[0].endpoint_fd,
            (unsigned long long)endpoint_info.kind,
            services[0].control_fd,
            (unsigned long long)control_info.kind);
        return 12;
    }

    struct pacha_ipc_msg control_msg = {
        .word0 = 0x5345454430424f4full,
        .word1 = 1,
        .word2 = 0,
        .word3 = 0,
    };
    if (pacha_ipc_send(services[0].control_fd, &control_msg) != 0) {
        fprintf(stderr, "[seed0boot] control send failed\n");
        return 13;
    }
    struct pacha_ipc_msg received_msg = {0};
    if (pacha_ipc_recv(services[0].peer_control_fd, &received_msg) != 0 ||
        received_msg.word0 != control_msg.word0 ||
        received_msg.word1 != control_msg.word1) {
        fprintf(stderr,
            "[seed0boot] control recv failed word0=%llx word1=%llu\n",
            (unsigned long long)received_msg.word0,
            (unsigned long long)received_msg.word1);
        return 14;
    }

    printf("[seed0boot] storage_boot starting\n");
    int capsule_status = seed0_launch_storage_boot_nvme();
    if (capsule_status != 0) {
        fprintf(stderr, "[seed0boot] storage_boot launch failed status=%d\n", capsule_status);
        return 17;
    }

    printf("[seed0boot] storage_boot started\n");
    printf("[seed0boot] ready\n");
    fflush(stdout);
    fflush(stderr);
    for (;;) __asm__ volatile("pause");
}

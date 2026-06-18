#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

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

static int arm_timer(int fd, long nsec)
{
    struct itimerspec spec;
    memset(&spec, 0, sizeof(spec));
    spec.it_value.tv_nsec = nsec;
    return timerfd_settime(fd, 0, &spec, 0);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("[seed0boot] start\n");
    fprintf(stderr, "[seed0boot] stderr online\n");

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

    int event_fd = eventfd(0, EFD_CLOEXEC);
    if (event_fd < 0) {
        fprintf(stderr, "[seed0boot] eventfd failed errno=%d\n", errno);
        return 2;
    }

    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (timer_fd < 0) {
        fprintf(stderr, "[seed0boot] timerfd_create failed errno=%d\n", errno);
        return 3;
    }
    if (arm_timer(timer_fd, 1000000) != 0) {
        fprintf(stderr, "[seed0boot] timerfd_settime failed errno=%d\n", errno);
        return 4;
    }

    uint64_t kick = 1;
    if (write(event_fd, &kick, sizeof(kick)) != (ssize_t)sizeof(kick)) {
        fprintf(stderr, "[seed0boot] eventfd write failed errno=%d\n", errno);
        return 5;
    }

    struct pollfd fds[2] = {
        { .fd = event_fd, .events = POLLIN, .revents = 0 },
        { .fd = timer_fd, .events = POLLIN, .revents = 0 },
    };
    int saw_event = 0;
    int saw_timer = 0;
    for (int iter = 0; iter < 8 && (!saw_event || !saw_timer); iter++) {
        int ready = poll(fds, 2, 50);
        if (ready < 0) {
            fprintf(stderr, "[seed0boot] poll failed errno=%d\n", errno);
            return 6;
        }
        if (ready == 0) continue;
        if (fds[0].revents & POLLIN) {
            uint64_t value = 0;
            if (read(event_fd, &value, sizeof(value)) != (ssize_t)sizeof(value) || value != 1) {
                fprintf(stderr, "[seed0boot] eventfd read failed errno=%d value=%llu\n", errno, (unsigned long long)value);
                return 7;
            }
            saw_event = 1;
        }
        if (fds[1].revents & POLLIN) {
            uint64_t expirations = 0;
            if (read(timer_fd, &expirations, sizeof(expirations)) != (ssize_t)sizeof(expirations) || expirations == 0) {
                fprintf(stderr, "[seed0boot] timerfd read failed errno=%d expirations=%llu\n", errno, (unsigned long long)expirations);
                return 8;
            }
            saw_timer = 1;
        }
    }

    if (!saw_event || !saw_timer) {
        fprintf(stderr, "[seed0boot] readiness loop incomplete event=%d timer=%d\n", saw_event, saw_timer);
        return 9;
    }

    int capsule_status = seed0_launch_storage_boot_nvme();
    if (capsule_status != 0) {
        fprintf(stderr, "[seed0boot] storage_boot launch failed status=%d\n", capsule_status);
        return 17;
    }

    for (int i = 0; i < seed0_service_count; i++) {
        printf("[seed0boot] service table: %s endpoint_fd=%d control_fd=%d peer_fd=%d\n",
            services[i].name,
            services[i].endpoint_fd,
            services[i].control_fd,
            services[i].peer_control_fd);
    }
    close(timer_fd);
    close(event_fd);

    printf("[seed0boot] ready\n");
    fflush(stdout);
    fflush(stderr);
    for (;;) __asm__ volatile("pause");
}

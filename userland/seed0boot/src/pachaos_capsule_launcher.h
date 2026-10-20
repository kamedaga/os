#include <pacha/abi.h>
#pragma once

int seed0_launch_storage_boot_nvme(
    pacha_fd_t ready_channel_fd,
    pacha_fd_t root_handoff_channel_fd,
    pacha_fd_t power_channel_fd);
int seed0_launch_live_filed(pacha_fd_t ready_channel_fd, pacha_fd_t unix_path_fd,
    pacha_fd_t *out_endpoint_fd);
int seed0_launch_live_seed0root(pacha_fd_t filed_endpoint_fd, pacha_fd_t unix_path_fd,
    pacha_fd_t root_handoff_fd, pacha_fd_t power_channel_fd);

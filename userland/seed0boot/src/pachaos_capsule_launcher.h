#pragma once

int seed0_launch_storage_boot_nvme(
    int ready_channel_fd,
    int root_handoff_channel_fd);
int seed0_launch_live_filed(int ready_channel_fd, int unix_path_fd,
    int *out_endpoint_fd);
int seed0_launch_live_seed0root(int filed_endpoint_fd, int unix_path_fd,
    int root_handoff_fd, int power_channel_fd);

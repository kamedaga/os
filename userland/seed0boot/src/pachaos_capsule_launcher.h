#ifndef SEED0BOOT_PACHAOS_CAPSULE_LAUNCHER_H
#define SEED0BOOT_PACHAOS_CAPSULE_LAUNCHER_H

int seed0_launch_pachaos_capsule_nvme(void);
int seed0_launch_netd(int filed_endpoint_fd, int *out_socket_endpoint_fd);
int seed0_launch_termd(int ready_channel_fd, int *out_tty_endpoint_fd);
int seed0_launch_drmd(int ready_channel_fd, int netd_endpoint_fd, int *out_drm_endpoint_fd);
int seed0_launch_inputd(int ready_channel_fd, int netd_endpoint_fd, int *out_input_endpoint_fd);
int seed0_launch_storage_boot_nvme(int ready_channel_fd, int service_ready_channel_fd);

#endif

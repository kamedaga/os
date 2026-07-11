#pragma once

#include <stdint.h>

#include "drmd/boot_config.h"
#include "drmd/ipc_protocol.h"

struct drmd_drm_island {
    void *device_backend;
    void *modules[DRMD_MAX_MODULES];
    uint32_t loaded_module_count;
    int ready;
    int load_status;
};

int drmd_drm_island_init(struct drmd_drm_island *island, const struct drmd_boot_config *cfg);
int drmd_drm_island_open(struct drmd_drm_island *island, const drmd_open_request_t *request, uint64_t *out_handle);
int drmd_drm_island_close(struct drmd_drm_island *island, uint64_t handle);
int drmd_drm_island_dup(struct drmd_drm_island *island, uint64_t handle, uint64_t *out_handle);
int drmd_drm_island_ioctl(struct drmd_drm_island *island, drmd_ioctl_request_t *request);
int drmd_drm_island_mmap(
    struct drmd_drm_island *island,
    const drmd_mmap_request_t *request,
    int *out_vmo_fd);

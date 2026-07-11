#pragma once

#include "drm_island.h"

int drmd_kms_init(struct drmd_drm_island *island);
int drmd_kms_ioctl(struct drmd_drm_island *island, drmd_ioctl_request_t *request, int *out_handled);
int drmd_kms_mmap(
    struct drmd_drm_island *island,
    const drmd_mmap_request_t *request,
    int *out_vmo_fd);
void drmd_kms_handle_close(struct drmd_drm_island *island, uint64_t handle);
void drmd_kms_handle_open(uint64_t handle);

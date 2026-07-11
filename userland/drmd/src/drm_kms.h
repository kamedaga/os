#pragma once

#include "drm_island.h"

typedef struct drmd_kms_state_counts {
    uint32_t fb;
    uint32_t dumb;
    uint32_t event_queues;
    uint32_t events;
    uint64_t master_handle;
} drmd_kms_state_counts_t;

int drmd_kms_init(struct drmd_drm_island *island);
int drmd_kms_ioctl(struct drmd_drm_island *island, drmd_ioctl_request_t *request, int *out_handled);
int drmd_kms_mmap(
    struct drmd_drm_island *island,
    const drmd_mmap_request_t *request,
    int *out_vmo_fd);
int drmd_kms_read(uint64_t handle, void *data, uint64_t capacity, uint64_t *out_size);
int drmd_kms_poll(uint64_t handle, uint32_t events, uint32_t *out_revents);
void drmd_kms_handle_irq(void);
void drmd_kms_handle_close(struct drmd_drm_island *island, uint64_t handle);
void drmd_kms_handle_open(uint64_t handle);
void drmd_kms_get_state_counts(drmd_kms_state_counts_t *out_counts);

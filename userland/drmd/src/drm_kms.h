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
int drmd_kms_peek_event(uint64_t handle, void *data, uint64_t capacity, uint64_t *out_size);
int drmd_kms_consume_event(uint64_t handle);
int drmd_kms_poll(uint64_t handle, uint32_t events, uint32_t *out_revents);
int drmd_kms_prime_export(
    uint64_t owner,
    uint32_t gem_handle,
    uint32_t flags,
    uint64_t *out_token,
    int *out_vmo_fd,
    uint64_t *out_rights);
int drmd_kms_prime_import(uint64_t owner, uint64_t token, uint32_t flags, uint32_t *out_gem_handle);
int drmd_kms_prime_import_vmo(
    uint64_t owner,
    int vmo_fd,
    uint64_t size,
    uint32_t flags,
    uint32_t *out_gem_handle);
int drmd_kms_prime_import_sync_file(uint64_t token, int wait_fd);
int drmd_kms_prime_acquire(uint64_t token);
int drmd_kms_prime_release(uint64_t token);
void drmd_kms_handle_close(struct drmd_drm_island *island, uint64_t handle);
void drmd_kms_handle_orphan(uint64_t handle);
void drmd_kms_handle_open(uint64_t handle);
void drmd_kms_get_state_counts(drmd_kms_state_counts_t *out_counts);

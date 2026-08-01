#pragma once

#include <stddef.h>
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

enum { DRMD_DRM_WAIT_SOURCE_MAX = 128 };

typedef struct drmd_ioctl_attachments {
    int input_wait_fd;
    int output_notify_fd;
    uint32_t consumed_fd_flags;
} drmd_ioctl_attachments_t;

int drmd_drm_island_init(struct drmd_drm_island *island, const struct drmd_boot_config *cfg);
int drmd_drm_island_open(struct drmd_drm_island *island, const drmd_open_request_t *request, int notify_fd, uint64_t *out_handle);
int drmd_drm_island_close(struct drmd_drm_island *island, uint64_t handle);
int drmd_drm_island_dup(struct drmd_drm_island *island, uint64_t handle, uint64_t *out_handle);
int drmd_drm_island_transfer_dup(
    struct drmd_drm_island *island,
    uint64_t handle,
    int lease_fd,
    uint64_t *out_handle);
int drmd_drm_island_ioctl(
    struct drmd_drm_island *island,
    drmd_ioctl_request_t *request,
    void *aux_data,
    uint64_t aux_size,
    drmd_ioctl_attachments_t *attachments);
int drmd_drm_island_mmap(
    struct drmd_drm_island *island,
    const drmd_mmap_request_t *request,
    int *out_vmo_fd);
int drmd_drm_island_read(struct drmd_drm_island *island, drmd_read_request_t *request, uint64_t *out_size);
int drmd_drm_island_poll(struct drmd_drm_island *island, const drmd_handle_request_t *request, uint64_t *out_events);
int drmd_drm_island_prime_export(
    struct drmd_drm_island *island,
    const drmd_prime_export_request_t *request,
    uint64_t *out_token,
    int *out_vmo_fd,
    uint64_t *out_rights);
int drmd_drm_island_prime_import(
    struct drmd_drm_island *island,
    const drmd_prime_import_request_t *request,
    int import_vmo_fd,
    uint64_t *out_gem_handle);
int drmd_drm_island_prime_acquire(
    struct drmd_drm_island *island,
    uint64_t token,
    int lease_fd);
int drmd_drm_island_prime_release(struct drmd_drm_island *island, uint64_t token);
size_t drmd_drm_island_collect_wait_sources(int *out_fds, size_t capacity);
size_t drmd_drm_island_reap_hangups(struct drmd_drm_island *island);
void drmd_drm_island_notify_readable(struct drmd_drm_island *island);

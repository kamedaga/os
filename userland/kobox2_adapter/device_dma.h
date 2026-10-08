/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_DEVICE_DMA_H
#define PACHA_KOBOX_DEVICE_DMA_H

#include "boot/dma_host.h"
#include <stdatomic.h>

struct ph_dma_mapping {
    uint64_t iova;
    uint64_t ram_offset; /* Valid for direct RAM mappings (view == NULL). */
    size_t length;
    int fd;
    void *view;
#if defined(PH_DMA_PROFILE) && PH_DMA_PROFILE
    unsigned int profile_direction;
#endif
};

struct ph_dma_config {
    int device_fd;
    uint64_t native_device;
    uint64_t generation;
    void *ram;
    int ram_fd;
    size_t ram_length;
    /* Populated from the device capsule query by ph_dma_init, not input policy. */
    uint64_t aperture_start;
    uint64_t aperture_end;
    struct ph_dma_mapping *mappings;
    size_t mapping_capacity;
};

struct ph_dma {
    struct kobox_linux_dma_host host;
    struct ph_dma_config config;
    atomic_uint lock;
    unsigned int admitted;
    unsigned int enabled;
    unsigned int drained;
    struct ph_dma *image_next; /* Owner's immutable attached-domain registry. */
#if defined(PH_DMA_PROFILE) && PH_DMA_PROFILE
    /* Diagnostic only, serialized by this port's existing lock. */
    uint64_t profile_counts[2][3][6][2];
#endif
};

/* Sandbox-local DMA port, initialized from a zeroed object. The bootstrap
 * owner must validate the RAM capability and keep its direct mapping stable
 * until destroy. The device grant exclusively names this translation domain;
 * the aperture is queried from the native domain, not a guessed HW limit.
 * Device FD, RAM and preallocated inventory remain caller-owned. Mapping FDs
 * are private to this port: never duplicate/transfer them, since close must
 * mean last-close and synchronous invalidation. Linux serializes leaf calls;
 * stop all callers before revoke/destroy. No allocation or Linux reentry.
 * A quarantined/unknown domain after failed publication terminates the sandbox
 * rather than returning RAM to Linux while device access may still be live. */
int ph_dma_init(struct ph_dma *dma, const struct ph_dma_config *config);
int ph_dma_revoke(struct ph_dma *dma, uint64_t generation);
int ph_dma_destroy(struct ph_dma *dma);

/* Failure-path snapshot only. Scattered views are counted separately because
 * their page list is not retained after the native mapping has been made. */
int ph_dma_cache_overlap(struct ph_dma *dma, size_t ram_offset, size_t length,
    size_t *direct_count, size_t *scatter_count, uint64_t *sample_iova);

#endif

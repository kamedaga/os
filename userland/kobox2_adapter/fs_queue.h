/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_FS_QUEUE_H
#define PACHA_KOBOX_FS_QUEUE_H

#include "lifecycle.h"
#include "../kobox2_owner/fs_channel.h"
#include "boot/fs_service.h"
#include "boot/fs_worker.h"

#define PH_FS_QUEUE_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_MAP_READ | \
    PACHA_FD_RIGHT_MAP_WRITE)

struct ph_fs_queue_request;
struct ph_fs_queue_lane {
    struct ph_fs_channel channel;
    kb2_vq_chain_t **slots;
    uint16_t *owners;
    kb2_vq_segment_t *segments;
    kb2_vq_chain_t probe; /* Owner-only transient validation, never executed. */
    struct ph_fs_queue_request *requests, *free_requests;
    size_t request_count, inflight, inventory_bytes;
    void *mapping;
    size_t mapping_bytes;
    int bound, direct;
    atomic_int retiring;
    atomic_uint ring_lock;
};

struct ph_fs_client_queue {
    struct ph_fs_client_queue *next;
    struct ph_fs_queue_lane lane;
    struct ph_fs_channel_config config;
    struct ph_ipc ipc;
    uint64_t client;
    int release_requested;
    int watch_fd; /* Only this endpoint number is read by the native helper. */
};

struct ph_fs_queue {
    struct ph_fs_channel_config config[3];
    struct ph_fs_queue_lane lanes[3];
    kb2_vq_atomic_ops_t atomics;
    struct kobox_linux_fs_workers *workers;
    struct kobox_linux_fs_worker_config worker_config;
    int (*create)(struct kobox_linux_fs_service *,
        const struct kobox_linux_fs_worker_config *, struct kobox_linux_fs_workers **);
    int (*submit)(struct kobox_linux_fs_workers *, uint64_t,
        const void *, size_t, size_t, void *);
    int (*submit_control)(struct kobox_linux_fs_workers *, const void *, size_t, size_t, void *);
    int (*collect)(struct kobox_linux_fs_workers *, struct kobox_linux_fs_work **,
        void **, const void **, size_t *, int *);
    int (*release_work)(struct kobox_linux_fs_workers *, struct kobox_linux_fs_work *);
    int (*close)(struct kobox_linux_fs_workers *);
    int (*destroy)(struct kobox_linux_fs_workers *);
    size_t inflight, inventory_bytes, inventory_budget;
    struct ph_fs_queue_request *control_active;
    uint64_t barrier_client, barrier_target;
    unsigned int next;
    uint64_t exchange_watermark, exchange_client;
    int exchange_owner, exchange_consumed, exchange_release_requested;
    struct ph_fs_client_queue *clients, *client_cursor;
    uint64_t client_watermark, reply_id, reply_operation;
    unsigned prefer_client;
    int closing;
    atomic_flag watch_lock;
    uint64_t watch_epoch, wait_after;
    struct ph_ipc wake_tx, wake_rx;
#ifdef PH_FS_STORAGE_PROFILE
    /* Private aggregate clocks, not part of the filesystem wire schema. */
    atomic_ullong profile_rows[3][5];
#endif
};

/* Initialize fresh process-local state exactly once per capsule generation;
 * state with published mappings must be retired, never rebound/reset. */
int ph_fs_queue_init(struct ph_fs_queue *queue, void *core,
    const struct ph_fs_channel_config *data,
    const struct ph_fs_channel_config *owner,
    const kb2_vq_atomic_ops_t *atomics, struct ph_lifecycle_service *service);
/* Called after lifecycle receiver joined and Linux FS/driver quiesce. */
int ph_fs_queue_finish(struct ph_fs_queue *queue);

#endif

/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_FS_QUEUE_H
#define PACHA_KOBOX_FS_QUEUE_H

#include <pacha/abi.h>
#include "lifecycle.h"
#include "../kobox2_owner/fs_channel.h"
#include "boot/fs_service.h"
#include "boot/fs_worker.h"
#include "boot/fs_shared.h"

#define PH_FS_QUEUE_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_MAP_READ | \
    PACHA_FD_RIGHT_MAP_WRITE)

struct ph_fs_queue_request;
struct ph_fs_shared_export;
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
    /* The device event index published by the last arm that found the ring
     * empty, while consumed has not moved since. That arm stays in force. */
    int armed;
    uint16_t armed_consumed;
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
    /* The native helper alone receives this endpoint's doorbells, under
     * watch_lock and only while watch_fd is nonzero (see drain_doorbells). */
    pacha_fd_t watch_fd;
    atomic_int doorbell_fault; /* Set by the helper; the owner faults the client. */
};

struct ph_fs_queue {
    struct ph_fs_channel_config config[3];
    struct ph_fs_queue_lane lanes[3];
    kb2_vq_atomic_ops_t atomics;
    struct kobox_linux_fs_workers *workers;
    struct kobox_linux_fs_service *filesystem;
    int (*shared_bind)(struct kobox_linux_fs_service *, const struct kobox_linux_fs_shared_ops *);
    int (*shared_close)(struct kobox_linux_fs_service *);
    struct ph_fs_shared_export *shared_exports;
    atomic_uint shared_lock;
    uint64_t next_shared;
    struct kobox_linux_fs_worker_config worker_config;
    int (*create)(struct kobox_linux_fs_service *,
        const struct kobox_linux_fs_worker_config *, struct kobox_linux_fs_workers **);
    int (*submit)(struct kobox_linux_fs_workers *, uint64_t,
        const void *, size_t, size_t, void *);
    int (*submit_control)(struct kobox_linux_fs_workers *, const void *, size_t, size_t, void *);
    int (*serve)(struct kobox_linux_fs_workers *);
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
    /* receive_async owns this on its dedicated native receiver thread. */
    struct ph_wait_retained wait_set;
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

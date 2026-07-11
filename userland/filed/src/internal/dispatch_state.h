#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "filed/payload.h"
#include "filed/runtime.h"
#include "../cache/internal.h"
#include "koboxd/storage_protocol.h"
#include "personality/linux_lpr.h"

enum {
    FILED_BOOTSTRAP_PATCH_BYTES = 4096,
    FILED_EXEC_MAX_FDS = 256,
    FILED_EXEC_FILED_ENDPOINT_FD = 240,
    FILED_EXEC_NETD_SOCKET_ENDPOINT_FD = 241,
    FILED_EXEC_TERMD_TTY_ENDPOINT_FD = 242,
    FILED_EXEC_DRMD_DRM_ENDPOINT_FD = LPR_DRMD_DRM_ENDPOINT_FD,
    FILED_EXEC_LPR_BOOTSTRAP_FD = LPR_BOOTSTRAP_FD,
    FILED_METRIC_OP_MAX = 0x8000u,
};

typedef struct filed_dispatch_metric {
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t total_cycles;
    uint64_t max_cycles;
    uint64_t reply_errors;
} filed_dispatch_metric_t;

typedef struct filed_fast_metric {
    uint64_t enqueued;
    uint64_t completed;
    uint64_t batches;
    uint64_t ring_full;
    uint64_t doorbells;
    uint64_t recv_total_ns;
    uint64_t recv_max_ns;
    uint64_t drain_total_ns;
    uint64_t drain_max_ns;
    uint64_t reply_total_ns;
    uint64_t reply_max_ns;
    uint64_t recv_total_cycles;
    uint64_t recv_max_cycles;
    uint64_t drain_total_cycles;
    uint64_t drain_max_cycles;
    uint64_t reply_total_cycles;
    uint64_t reply_max_cycles;
} filed_fast_metric_t;

typedef struct filed_fast_op_metric {
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t total_cycles;
    uint64_t max_cycles;
    uint64_t errors;
} filed_fast_op_metric_t;

typedef struct filed_dispatch_state {
    filed_dispatch_metric_t dispatch_metrics[FILED_METRIC_OP_MAX];
    filed_fast_metric_t fast_metrics;
    filed_fast_op_metric_t fast_op_metrics[FILED_METRIC_OP_MAX];
    filed_cache_t cache;
    uint64_t target_lookup_vfs_hits;
    uint64_t target_lookup_backend_hits;
    uint64_t target_lookup_misses;
} filed_dispatch_state_t;

int filed_dispatch_runtime_init(filed_runtime_t *runtime);

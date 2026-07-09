#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "filed/payload_v2.h"
#include "filed/runtime.h"
#include "koboxd/storage_protocol_v2.h"
#include "personality/linux_lpr.h"

enum {
    FILED_BOOTSTRAP_PATCH_BYTES = 4096,
    FILED_EXEC_MAX_FDS = 256,
    FILED_EXEC_FILED_ENDPOINT_FD = 240,
    FILED_EXEC_NETD_SOCKET_ENDPOINT_FD = 241,
    FILED_EXEC_TERMD_TTY_ENDPOINT_FD = 242,
    FILED_EXEC_LPR_BOOTSTRAP_FD = LPR_BOOTSTRAP_FD,
    FILED_METRIC_OP_MAX = 0x8000u,
    FILED_PAGE_CACHE_BYTES = 16384,
    FILED_PAGE_CACHE_SLOTS = 64,
    FILED_DIR_CACHE_SLOTS = 32,
    FILED_NEGATIVE_LOOKUP_CACHE_SLOTS = 64,
    FILED_RUNTIME_FILE_VMO_CACHE_SLOTS = 16,
    FILED_FILE_VMO_MAX_BYTES = 8u * 1024u * 1024u,
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

typedef struct filed_page_cache_slot {
    bool valid;
    bool dirty;
    uint64_t backend_object;
    uint64_t page_index;
    uint64_t last_used;
    uint32_t bytes;
    uint32_t valid_start;
    uint32_t dirty_start;
    uint32_t dirty_end;
    uint8_t data[FILED_PAGE_CACHE_BYTES];
} filed_page_cache_slot_t;

typedef struct filed_page_cache {
    filed_page_cache_slot_t slots[FILED_PAGE_CACHE_SLOTS];
    uint64_t clock;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t direct_reads;
    uint64_t dirty_writes;
    uint64_t flushes;
    uint64_t flush_errors;
    uint64_t active_slots;
    bool configured;
} filed_page_cache_t;

typedef struct filed_dir_cache_slot {
    bool valid;
    uint64_t backend_object;
    uint64_t offset;
    uint64_t last_used;
    storage_v2_getdents_request_t entries;
} filed_dir_cache_slot_t;

typedef struct filed_dir_cache {
    filed_dir_cache_slot_t slots[FILED_DIR_CACHE_SLOTS];
    uint64_t clock;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
} filed_dir_cache_t;

typedef struct filed_negative_lookup_cache_slot {
    bool valid;
    uint64_t parent_backend_object;
    filed_generation_t parent_dir_generation;
    uint64_t last_used;
    int64_t status;
    char name[FILED_V2_NAME_BYTES];
} filed_negative_lookup_cache_slot_t;

typedef struct filed_negative_lookup_cache {
    filed_negative_lookup_cache_slot_t slots[FILED_NEGATIVE_LOOKUP_CACHE_SLOTS];
    uint64_t clock;
    uint64_t hits;
    uint64_t misses;
    uint64_t stores;
    uint64_t evictions;
} filed_negative_lookup_cache_t;

typedef struct filed_file_vmo_cache_entry {
    uint8_t active;
    uint8_t reserved0;
    uint16_t reserved1;
    int vmo_fd;
    uint64_t backend_object;
    uint64_t object_generation;
    uint64_t file_offset;
    uint64_t length;
    uint64_t clock;
} filed_file_vmo_cache_entry_t;

typedef struct filed_dispatch_state {
    filed_dispatch_metric_t dispatch_metrics[FILED_METRIC_OP_MAX];
    filed_fast_metric_t fast_metrics;
    filed_fast_op_metric_t fast_op_metrics[FILED_METRIC_OP_MAX];
    filed_page_cache_t page_cache;
    filed_dir_cache_t dir_cache;
    filed_negative_lookup_cache_t negative_lookup_cache;
    filed_file_vmo_cache_entry_t file_vmo_cache[FILED_RUNTIME_FILE_VMO_CACHE_SLOTS];
    uint64_t target_lookup_vfs_hits;
    uint64_t target_lookup_backend_hits;
    uint64_t target_lookup_misses;
    uint64_t file_vmo_cache_hits;
    uint64_t file_vmo_cache_misses;
    uint64_t file_vmo_cache_stores;
    uint64_t file_vmo_cache_evictions;
    uint64_t file_vmo_cache_clock;
} filed_dispatch_state_t;

int filed_dispatch_runtime_init(filed_runtime_t *runtime);

#pragma once

#include <stdint.h>

#include "filed/bootstrap.h"
#include "filed/kobox_backend.h"
#include "filed/tmpfs_internal.h"
#include "filed/vfs.h"

enum {
    FILED_RUNTIME_MAX_SESSIONS = 32,
    FILED_RUNTIME_FILE_VMO_CACHE_SLOTS = 16,
};

typedef struct filed_session {
    int channel_fd;
    int page_fd;
    void *page;
    uint64_t page_size;
    uint8_t active;
} filed_session_t;

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

typedef struct filed_runtime {
    filed_vfs_t vfs;
    filed_bootstrap_t bootstrap;
    filed_kobox_backend_t backend;
    filed_tmpfs_backend_t tmpfs;
    int bootstrap_fd;
    int client_endpoint_fd;
    int syncer_timer_fd;
    int netd_socket_endpoint_fd;
    int termd_tty_endpoint_fd;
    filed_session_t sessions[FILED_RUNTIME_MAX_SESSIONS];
    filed_file_vmo_cache_entry_t file_vmo_cache[FILED_RUNTIME_FILE_VMO_CACHE_SLOTS];
    uint64_t request_sequence;
    uint64_t file_vmo_cache_clock;
    uint64_t syncer_ticks;
    uint64_t syncer_flushes;
    uint64_t syncer_errors;
    int syncer_last_status;
    uint64_t root_size;
    filed_mount_id_t root_mount_id;
    filed_handle_id_t root_handle_id;
    filed_handle_id_t tmpfs_root_handle_id;
    uint8_t root_tmpfs_synthetic_dirent;
    uint8_t tmpfs_root_handle_valid;
} filed_runtime_t;

void filed_runtime_init(filed_runtime_t *runtime);
int filed_runtime_bootstrap(filed_runtime_t *runtime, char **argv);
int filed_runtime_mount_root(filed_runtime_t *runtime);
int filed_runtime_serve(filed_runtime_t *runtime);

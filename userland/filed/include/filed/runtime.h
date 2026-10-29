#include <pacha/abi.h>
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "filed/mutex.h"
#include "filed/identity.h"
#include "filed/storage.h"
#include "filed/snapshot_cache.h"
#include "storage/bootstrap.h"

struct filed_storage_owner;
struct filed_session;
struct filed_lease;
struct filed_unix_hold;
struct filed_pager;
typedef struct filed_runtime {
    storage_filed_bootstrap_t bootstrap;
    filed_snapshot_cache_t snapshots;
    /* The pager worker and client dispatcher share one sandbox data queue. */
    filed_mutex_t storage_exchange_lock;
    struct filed_pager *pager;
    struct filed_storage_owner *storage;
    pacha_fd_t bootstrap_fd, client_endpoint_fd, unix_path_fd;
    pacha_fd_t live_bootfs_fd, live_ready_fd;
    uint64_t live_bootfs_size;
    uintptr_t executable_phdr;
    uint64_t executable_phnum;
    uint8_t live_root;
    struct filed_client *clients, *actor;
    struct filed_session *sessions;
    struct filed_lease *leases;
    struct filed_unix_hold *unix_holds;
    uint64_t unix_hold_sequence;
    uint64_t client_sequence, request_sequence, storage_generation;
    uint64_t file_vmo_requests, file_vmo_misses, file_vmo_miss_bytes;
    pacha_fd_t netd_socket_endpoint_fd, termd_tty_endpoint_fd;
    pacha_fd_t gpud_drm_endpoint_fd, inputd_input_endpoint_fd, audiod_pcm_endpoint_fd;
} filed_runtime_t;
void filed_runtime_init(filed_runtime_t *);
int filed_runtime_bootstrap(filed_runtime_t *, char **);
int filed_runtime_mount_root(filed_runtime_t *);
int filed_live_root_import(filed_runtime_t *);
int filed_runtime_serve(filed_runtime_t *);
int filed_sessions_release_client(filed_runtime_t *, struct filed_client *);
int filed_owner_control(filed_runtime_t *, kb2_fs_request_t *, const uint8_t *,
                       size_t, kb2_fs_response_t *);

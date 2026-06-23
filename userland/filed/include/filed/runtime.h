#pragma once

#include <stdint.h>

#include "filed/bootstrap.h"
#include "filed/kobox_backend.h"
#include "filed/vfs.h"

typedef struct filed_runtime {
    filed_vfs_t vfs;
    filed_bootstrap_t bootstrap;
    filed_kobox_backend_t backend;
    int bootstrap_fd;
    int client_endpoint_fd;
    uint64_t request_sequence;
    uint64_t root_size;
    filed_mount_id_t root_mount_id;
    filed_handle_id_t root_handle_id;
} filed_runtime_t;

void filed_runtime_init(filed_runtime_t *runtime);
int filed_runtime_bootstrap(filed_runtime_t *runtime, char **argv);
int filed_runtime_mount_root(filed_runtime_t *runtime);
int filed_runtime_serve(filed_runtime_t *runtime);

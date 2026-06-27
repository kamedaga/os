#pragma once

#include "block_service.h"
#include "bootstrap.h"
#include "fs_backend.h"
#include "ipc_service.h"
#include "filed/runtime.h"
#include "kobox/device.h"
#include "kobox/module.h"
#include "linux_subsystem/fs/fs.h"

#include <stdint.h>

enum {
    KOBOXD_ROOTFS_GPT_PARTITION_INDEX = 2,
};

typedef struct koboxd_storage_runtime {
    koboxd_ipc_service_t *ipc_service;
    kb_device_backend_t *device_backend;
    koboxd_block_service_t block_service;
    koboxd_fs_backend_t fs_backend;
    filed_runtime_t filed_runtime;
    kb_module_t *ext4_module;
    void *disk;
    kb_fs_block_device_t *root_device;
    uint8_t block_ready;
    uint8_t fs_ready;
    uint8_t filed_ready;
} koboxd_storage_runtime_t;

int koboxd_storage_runtime_init(
    koboxd_storage_runtime_t *runtime,
    koboxd_ipc_service_t *ipc_service,
    const koboxd_bootstrap_t *bootstrap);
koboxd_fs_backend_t *koboxd_storage_runtime_fs_backend(koboxd_storage_runtime_t *runtime);
int koboxd_storage_runtime_serve(
    koboxd_storage_runtime_t *runtime,
    int control_fd);
int koboxd_run_storage(
    koboxd_ipc_service_t *ipc_service,
    const koboxd_bootstrap_t *bootstrap);

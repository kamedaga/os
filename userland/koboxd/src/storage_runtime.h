#pragma once

#include "bootstrap.h"
#include "fs_backend.h"
#include "kobox/device.h"
#include "kobox/module.h"
#include "linux_subsystem/fs/fs.h"

#include <stdint.h>

typedef struct koboxd_storage_runtime {
    kb_device_backend_t *device_backend;
    koboxd_fs_backend_t fs_backend;
    kb_module_t *ext4_module;
    void *disk;
    kb_fs_block_device_t *root_device;
    uint8_t fs_ready;
} koboxd_storage_runtime_t;

int koboxd_storage_runtime_init(
    koboxd_storage_runtime_t *runtime,
    const koboxd_bootstrap_t *bootstrap);
koboxd_fs_backend_t *koboxd_storage_runtime_fs_backend(koboxd_storage_runtime_t *runtime);

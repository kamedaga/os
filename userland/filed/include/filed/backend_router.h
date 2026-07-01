#pragma once

#include <stdint.h>

#include "filed/runtime.h"
#include "koboxd/ipc_protocol.h"

int filed_runtime_backend_lookup(
    filed_runtime_t *runtime,
    uint64_t parent_object_id,
    const char *name,
    uint64_t *out_object_id);

int filed_runtime_backend_statx(
    filed_runtime_t *runtime,
    uint64_t object_id,
    koboxd_wire_fs_statx_t *out_stat);

int filed_runtime_backend_pread(
    filed_runtime_t *runtime,
    uint64_t object_id,
    uint64_t offset,
    void *buffer,
    uint64_t length,
    uint64_t *out_bytes);

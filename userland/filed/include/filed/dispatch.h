#pragma once

#include "filed/payload.h"
#include "filed/runtime.h"
#include "filed/vfs.h"

int filed_dispatch_client_once(filed_runtime_t *runtime, int client_fd);
int filed_dispatch_session_once(filed_runtime_t *runtime, uint64_t session_index);
int filed_dispatch_sync_all(filed_runtime_t *runtime);
void filed_dispatch_log_state_checkpoint(filed_runtime_t *runtime, const char *source);
int64_t filed_openat_path(
    filed_runtime_t *runtime,
    const filed_openat_t *openat,
    filed_vfs_open_result_t *out_open);

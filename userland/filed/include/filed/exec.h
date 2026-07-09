#pragma once

#include <stdint.h>

#include "filed/payload_v2.h"
#include "filed/vfs.h"

struct filed_runtime;

int filed_exec_handle(
    struct filed_runtime *runtime,
    filed_handle_id_t handle_id,
    const filed_v2_exec_path_t *request,
    const int *inherit_fds,
    uint64_t inherit_fd_count,
    int bootstrap_fd,
    int *out_process_fd,
    int *out_thread_fd);

void filed_exec_invalidate_backend_object(struct filed_runtime *runtime, uint64_t backend_object);

#pragma once

#include <stdint.h>

#include "filed/ipc_protocol.h"
#include "filed/vfs.h"

struct filed_runtime;

int filed_exec_native_handle(
    struct filed_runtime *runtime,
    filed_handle_id_t handle_id,
    const filed_wire_exec_path_t *request,
    const int *inherit_fds,
    uint64_t inherit_fd_count,
    int bootstrap_fd,
    int *out_process_fd,
    int *out_thread_fd);

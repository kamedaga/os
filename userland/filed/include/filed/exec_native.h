#include <pacha/abi.h>
#pragma once

#include <stdint.h>

#include "filed/payload.h"
#include "filed/storage.h"

struct filed_runtime;

int filed_exec_native_handle(
    struct filed_runtime *runtime,
    filed_handle_id_t handle_id,
    const filed_exec_path_t *request,
    const pacha_fd_t *inherit_fds,
    uint64_t inherit_fd_count,
    pacha_fd_t bootstrap_fd,
    pacha_fd_t *out_process_fd,
    pacha_fd_t *out_thread_fd);

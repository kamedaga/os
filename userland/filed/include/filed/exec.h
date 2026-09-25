#pragma once

#include <stdint.h>

#include "filed/payload.h"
#include "filed/vfs.h"
#include "pacha/ipc.h"

struct filed_runtime;
enum { FILED_EXEC_NATIVE_BOOTSTRAP_FD = 255 };

int filed_exec_build_grants(struct filed_runtime *runtime,
    const filed_exec_path_t *request, const int *sources, uint64_t source_count,
    int bootstrap_fd, struct pacha_process_fd_grant *grants, uint64_t *count);

int filed_exec_handle(
    struct filed_runtime *runtime,
    filed_handle_id_t handle_id,
    const filed_exec_path_t *request,
    const int *inherit_fds,
    uint64_t inherit_fd_count,
    int bootstrap_fd,
    int *out_process_fd,
    int *out_thread_fd);

void filed_exec_invalidate_backend_object(struct filed_runtime *runtime, uint64_t backend_object);

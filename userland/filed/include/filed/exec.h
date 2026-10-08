#pragma once

#include <stdint.h>

#include "filed/payload.h"
#include "filed/storage.h"
#include "pacha/ipc.h"
#include "personality/linux_lpr.h"

struct filed_runtime;
enum {
    FILED_EXEC_NATIVE_BOOTSTRAP_FD = 255,
    FILED_EXEC_NETD_SOCKET_ENDPOINT_FD = 241,
    FILED_EXEC_TERMD_TTY_ENDPOINT_FD = 242,
    FILED_EXEC_GPUD_DRM_ENDPOINT_FD = LPR_GPUD_DRM_ENDPOINT_FD,
    FILED_EXEC_INPUTD_INPUT_ENDPOINT_FD = LPR_INPUTD_INPUT_ENDPOINT_FD,
    FILED_EXEC_LPR_BOOTSTRAP_FD = LPR_BOOTSTRAP_FD,
};

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

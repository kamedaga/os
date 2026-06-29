#include "filed/exec.h"

#include "filed/exec_linux_lpr.h"
#include "filed/exec_native.h"

int filed_exec_handle(
    struct filed_runtime *runtime,
    filed_handle_id_t handle_id,
    const filed_wire_exec_path_t *request,
    const int *inherit_fds,
    uint64_t inherit_fd_count,
    int bootstrap_fd,
    int *out_process_fd,
    int *out_thread_fd)
{
    if (request != 0 && (request->flags & FILED_WIRE_EXEC_LINUX_LPR) != 0) {
        return filed_exec_linux_lpr_handle(
            runtime,
            handle_id,
            request,
            inherit_fds,
            inherit_fd_count,
            bootstrap_fd,
            out_process_fd,
            out_thread_fd);
    }
    return filed_exec_native_handle(
        runtime,
        handle_id,
        request,
        inherit_fds,
        inherit_fd_count,
        bootstrap_fd,
        out_process_fd,
        out_thread_fd);
}

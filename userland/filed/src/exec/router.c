#include "filed/exec.h"

#include "filed/exec_linux_lpr.h"
#include "filed/exec_native.h"
#include "filed/runtime.h"
#include "pacha/launch.h"
#include "../internal/dispatch_state.h"

int filed_exec_build_grants(struct filed_runtime *runtime,
    const filed_exec_path_t *request, const int *sources, uint64_t source_count,
    int bootstrap_fd, struct pacha_process_fd_grant *grants, uint64_t *count)
{
    if (!runtime || !request || !grants || !count ||
        source_count > FILED_EXEC_MAX_INHERIT_FDS ||
        source_count != request->inherit_fd_count || (source_count && !sources) ||
        (!(request->flags & FILED_EXEC_INHERIT_FDS) && source_count)) return -22;
    *count = 0;
    const struct pacha_process_fd_grant logs[] = { PACHA_LAUNCH_LOG_GRANTS(0) };
    for (unsigned i = 0; i < 2; ++i) grants[(*count)++] = logs[i];
    for (uint64_t i = 0; i < source_count; ++i) {
        const filed_exec_fd_grant_t *spec = &request->fd_grants[i];
        if (sources[i] < 16 || spec->target < 16 || spec->target >= PACHA_FD_TABLE_LIMIT)
            return -22;
        grants[(*count)++] = (struct pacha_process_fd_grant){
            .source_fd = (uint64_t)sources[i], .target_fd = spec->target,
            .rights = spec->rights, .flags = spec->flags };
    }
    const int linux = (request->flags & FILED_EXEC_LINUX_LPR) != 0;
    const uint64_t services = request->flags & (FILED_EXEC_SERVICE_NETD |
        FILED_EXEC_SERVICE_TERMD | FILED_EXEC_SERVICE_GPUD_DRM | FILED_EXEC_SERVICE_INPUTD);
    if (services && (!linux || runtime->actor)) return -1;
    if (request->flags & FILED_EXEC_BOOTSTRAP_FD) {
        if (bootstrap_fd < 16) return -22;
        grants[(*count)++] = (struct pacha_process_fd_grant)PACHA_LAUNCH_GRANT(
            bootstrap_fd, linux ? FILED_EXEC_LPR_BOOTSTRAP_FD : FILED_EXEC_NATIVE_BOOTSTRAP_FD,
            PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ |
                /* LPR retains this slot for rollback-safe self-exec replacement. */
                (linux ? PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_SET_FLAGS : PACHA_FD_RIGHT_READ));
    }
    /* Native services receive only their configured capabilities and logs.
     * The Linux personality gets client handles, never service RECV authority. */
    if (linux) {
        const uint64_t client = PACHA_LAUNCH_CLIENT | PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_SET_FLAGS;
        const struct pacha_process_fd_grant clients[] = {
            PACHA_LAUNCH_GRANT(runtime->netd_socket_endpoint_fd, FILED_EXEC_NETD_SOCKET_ENDPOINT_FD, client),
            PACHA_LAUNCH_GRANT(runtime->termd_tty_endpoint_fd, FILED_EXEC_TERMD_TTY_ENDPOINT_FD, client),
            PACHA_LAUNCH_GRANT(runtime->gpud_drm_endpoint_fd, FILED_EXEC_GPUD_DRM_ENDPOINT_FD, client),
            PACHA_LAUNCH_GRANT(runtime->inputd_input_endpoint_fd, FILED_EXEC_INPUTD_INPUT_ENDPOINT_FD, client),
        };
        for (unsigned i = 0; i < sizeof(clients) / sizeof(clients[0]); ++i)
            if (services & (FILED_EXEC_SERVICE_NETD << i)) {
                if (clients[i].source_fd < 16 || clients[i].source_fd >= PACHA_FD_TABLE_LIMIT)
                    return -107;
                grants[(*count)++] = clients[i];
            }
    }
    /* Reject collisions with fixed runtime/config slots before creating a child. */
    for (uint64_t i = 0; i < *count; ++i)
        for (uint64_t j = 0; j < i; ++j)
            if (grants[i].target_fd == grants[j].target_fd) return -22;
    return 0;
}

int filed_exec_handle(
    struct filed_runtime *runtime,
    filed_handle_id_t handle_id,
    const filed_exec_path_t *request,
    const int *inherit_fds,
    uint64_t inherit_fd_count,
    int bootstrap_fd,
    int *out_process_fd,
    int *out_thread_fd)
{
    if (request != 0 && (request->flags & FILED_EXEC_LINUX_LPR) != 0) {
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

void filed_exec_invalidate_backend_object(struct filed_runtime *runtime, uint64_t backend_object)
{
    filed_exec_linux_lpr_invalidate_backend_object(runtime, backend_object);
}

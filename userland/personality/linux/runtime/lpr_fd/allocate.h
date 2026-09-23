#pragma once
#include "../lpr_filed_internal.h"

/* Publish a fully initialized backend while selecting its number under the
 * same table lock. Ownership transfers only on success. */
static const lpr_linux_fd_t lpr_fd_allocation_excluded[] = {
        LPR_FILED_ENDPOINT_FD, LPR_NETD_ENDPOINT_FD, LPR_TERMD_TTY_ENDPOINT_FD,
        LPR_GPUD_DRM_ENDPOINT_FD, LPR_INPUTD_INPUT_ENDPOINT_FD,
        LPR_BOOTSTRAP_FD, LPR_SUPERVISOR_ENDPOINT_FD,
};

static inline int lpr_fd_alloc_initialized_batch(
    const lpr_fd_install_t *installs, uint32_t count, lpr_linux_fd_t *fds)
{
    lpr_fd_arrays_init();
    while (lpr_fd_table_alloc_batch(&lpr_control_fd_table, 3, installs, count,
            lpr_fd_allocation_excluded, sizeof(lpr_fd_allocation_excluded) /
                sizeof(lpr_fd_allocation_excluded[0]), fds) != 0) {
        const uint64_t capacity = lpr_fd_table_capacity;
        if (capacity >= LPR_FD_TABLE_MAX_SIZE ||
            lpr_fd_table_ensure_capacity(capacity + 1) != 0)
            return -LPR_LINUX_EMFILE;
    }
    return 0;
}

static inline int lpr_fd_alloc_initialized(const lpr_fd_install_t *install)
{
    lpr_linux_fd_t fd;
    const int status = lpr_fd_alloc_initialized_batch(install, 1, &fd);
    return status ? status : (int)fd;
}

/* Copy a complete, caller-owned record before publishing it. Native objects
 * named by the record remain caller-owned if allocation fails. */
int lpr_fd_alloc_state(uint8_t ops_id, uint64_t flags,
    uint64_t offset, const void *record, size_t bytes);

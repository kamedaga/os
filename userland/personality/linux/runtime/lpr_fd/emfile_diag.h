#pragma once
#include "../lpr_filed_internal.h"
#include "../support/emfile_diag.h"

/* Failure-only snapshot. Call after releasing the table lock. Include staged
 * SCM entries: they consume slots even before publication. */
static inline uint64_t lpr_emfile_fd_used(void)
{
    uint64_t used = 0;
    lpr_fd_table_lock(&lpr_control_fd_table);
    for (uint32_t i = 0; i < lpr_control_fd_table.entry_count; ++i)
        used += lpr_control_fd_table.entries[i].active != 0;
    lpr_fd_table_unlock(&lpr_control_fd_table);
    return used;
}
#define lpr_fd_emfile(site, detail) lpr_emfile_diag(site, \
    lpr_emfile_fd_used(), lpr_fd_table_soft_limit(&lpr_control_fd_table), detail)

#define lpr_fd_emfile_result(site, detail) \
    ({ lpr_fd_emfile(site, detail); -LPR_LINUX_EMFILE; })

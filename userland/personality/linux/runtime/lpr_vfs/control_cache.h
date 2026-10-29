/* SPDX-License-Identifier: MIT */
#ifndef LPR_FS_CONTROL_CACHE_H
#define LPR_FS_CONTROL_CACHE_H
#include <stdint.h>
#include <pacha/abi.h>
struct lpr_fs_control_lease {
    struct lpr_fs_control_lease *next;
    pacha_fd_t fd;
    uint64_t address, bytes;
};
int64_t lpr_fs_control_acquire(struct lpr_fs_control_lease *, uint64_t bytes);
void lpr_fs_control_release(struct lpr_fs_control_lease *, int completed);
void lpr_fs_control_fork_lock(void);
void lpr_fs_control_fork_unlock(void);
int lpr_fs_control_fork_child(void);
#endif

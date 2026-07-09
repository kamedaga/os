#pragma once

#include <lpr_supervisor/ipc_protocol_v2.h>
#include <stdint.h>

typedef struct lpr_supervisor_fd_snapshot_ops {
    uint64_t *request_counter;
    int64_t (*status_to_errno)(int64_t status);
    int (*create_page)(void **out_page);
    void (*destroy_page)(int fd, void *page);
    int (*count_fds)(void *ctx, uint64_t *out_count);
    int (*next_fd)(void *ctx, uint64_t *cursor, lprs_v2_fd_desc_t *out, int *out_has);
    int (*install_fd)(void *ctx, const lprs_v2_fd_desc_t *desc);
} lpr_supervisor_fd_snapshot_ops_t;

int lpr_supervisor_fd_snapshot_replace(
    uint64_t token,
    const lpr_supervisor_fd_snapshot_ops_t *ops,
    void *ctx);

int lpr_supervisor_fd_snapshot_restore(
    uint64_t token,
    const lpr_supervisor_fd_snapshot_ops_t *ops,
    void *ctx);

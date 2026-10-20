#include <pacha/abi.h>
#ifndef LPR_FS_RPC_H
#define LPR_FS_RPC_H

#include <kobox2/filesystem.h>

struct lpr_fs_buffer;
int64_t lpr_fs_call_buffers(kb2_fs_request_t *, const struct lpr_fs_buffer *, unsigned,
    kb2_fs_response_t *, const struct lpr_fs_buffer *, unsigned);

/* All decoded values are process-local. Paths/data are copied into an
 * immutable request before ownership passes to the peer. */
int64_t lpr_fs_call(kb2_fs_request_t *request, const char *path,
    const char *second_path, const void *data, uint64_t data_bytes,
    void *output, uint64_t output_bytes, kb2_fs_response_t *response,
    pacha_fd_t transfer_fd, pacha_fd_t *returned_fd);
int64_t lpr_fs_call_many(kb2_fs_request_t *request, const char *path,
    const char *second_path, const void *data, uint64_t data_bytes,
    void *output, uint64_t output_bytes, kb2_fs_response_t *response,
    pacha_fd_t transfer_fd, pacha_fd_t *returned_fds, uint64_t returned_capacity);
int64_t lpr_fs_iov(uint64_t handle, uint64_t iov_raw, uint64_t iov_count, int writing);
int64_t lpr_fs_call_attach(kb2_fs_request_t *, pacha_fd_t, pacha_fd_t, kb2_fs_response_t *);

#endif

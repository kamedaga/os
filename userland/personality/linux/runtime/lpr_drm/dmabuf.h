#pragma once
#include "../lpr_fd/allocate.h"

static inline int lpr_drm_install(uint64_t handle, uint64_t flags,
    int native_wait_fd, uint32_t node_kind)
{
    lpr_drm_backend_t *backend = lpr_backend_state_alloc(sizeof(*backend));
    if (backend == 0) return -LPR_LINUX_ENOMEM;
    *backend = (lpr_drm_backend_t){
        .active = 1, .flags = (uint32_t)flags, .handle = handle,
        .wait_fd.raw = native_wait_fd, .lease_fd.raw = -1, .node_kind = node_kind,
    };
    const lpr_fd_install_t install = {
        .ops_id = LPR_FD_OPS_DRM,
        .fd_flags = (flags & LPR_LINUX_O_CLOEXEC) ? LPR_FD_ENTRY_CLOEXEC : 0,
        .access_mode = (uint16_t)(flags & LPR_LINUX_O_ACCMODE),
        .status_flags = ((flags & LPR_LINUX_O_NONBLOCK) ? LPR_OFD_NONBLOCK : 0) |
            ((flags & LPR_LINUX_O_APPEND) ? LPR_OFD_APPEND : 0),
        .rights = LPR_FD_RIGHT_STAT | LPR_FD_RIGHT_DUP | LPR_FD_RIGHT_READ |
            LPR_FD_RIGHT_IOCTL | LPR_FD_RIGHT_MMAP,
        .backend_state = backend, .backend_state_bytes = sizeof(*backend),
    };
    const int fd = lpr_fd_alloc_initialized(&install);
    if (fd < 0) (void)lpr_backend_state_free(backend, sizeof(*backend));
    return fd;
}

/* Publish the complete DMA-BUF backend atomically. The caller retains the
 * native FD and PRIME token on failure, and transfers both on success. */
static inline int lpr_dmabuf_install(int native_fd, int lease_fd, uint64_t token,
    uint64_t size, uint64_t flags)
{
    lpr_dmabuf_backend_t *backend = lpr_backend_state_alloc(sizeof(*backend));
    if (backend == 0) return -LPR_LINUX_ENOMEM;
    *backend = (lpr_dmabuf_backend_t){
        .active = 1, .writable = 1, .flags = (uint32_t)flags,
        .token = token, .size = size,
        .native.raw = native_fd, .lease_fd.raw = lease_fd,
    };
    const lpr_fd_install_t install = {
        .ops_id = LPR_FD_OPS_DMABUF,
        .fd_flags = (flags & LPR_LINUX_O_CLOEXEC) ? LPR_FD_ENTRY_CLOEXEC : 0,
        .access_mode = LPR_LINUX_O_RDWR,
        .rights = LPR_FD_RIGHT_IOCTL | LPR_FD_RIGHT_MMAP | LPR_FD_RIGHT_STAT | LPR_FD_RIGHT_DUP,
        .backend_state = backend, .backend_state_bytes = sizeof(*backend),
    };
    const int fd = lpr_fd_alloc_initialized(&install);
    if (fd < 0) (void)lpr_backend_state_free(backend, sizeof(*backend));
    return fd;
}

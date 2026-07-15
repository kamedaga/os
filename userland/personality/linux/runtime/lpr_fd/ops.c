#include "../lpr_filed_internal.h"

typedef int64_t (*lpr_fd_io_op_t)(
    const lpr_fd_pin_t *pin,
    uint64_t arg0,
    uint64_t arg1);
typedef int64_t (*lpr_fd_mmap_op_t)(
    const lpr_fd_pin_t *pin,
    uint64_t addr,
    uint64_t len,
    uint64_t prot,
    uint64_t flags,
    uint64_t offset);
typedef int64_t (*lpr_fd_close_op_t)(void *state);
typedef int64_t (*lpr_fd_dup_op_t)(const lpr_fd_pin_t *pin);

typedef struct lpr_fd_ops {
    lpr_fd_io_op_t read;
    lpr_fd_io_op_t write;
    lpr_fd_io_op_t readv;
    lpr_fd_io_op_t writev;
    lpr_fd_io_op_t ioctl;
    lpr_fd_io_op_t stat;
    lpr_fd_mmap_op_t mmap;
    lpr_fd_close_op_t close;
    lpr_fd_dup_op_t dup;
} lpr_fd_ops_t;

static int64_t lpr_ops_read(
    const lpr_fd_pin_t *pin,
    uint64_t buffer,
    uint64_t count)
{
    return lpr_backend_read(pin->fd, buffer, count);
}

static int64_t lpr_ops_write(
    const lpr_fd_pin_t *pin,
    uint64_t buffer,
    uint64_t count)
{
    return lpr_backend_write(pin->fd, buffer, count);
}

static int64_t lpr_ops_readv(
    const lpr_fd_pin_t *pin,
    uint64_t iov,
    uint64_t count)
{
    return lpr_backend_readv(pin->fd, iov, count);
}

static int64_t lpr_ops_writev(
    const lpr_fd_pin_t *pin,
    uint64_t iov,
    uint64_t count)
{
    return lpr_backend_writev(pin->fd, iov, count);
}

static int64_t lpr_ops_ioctl(
    const lpr_fd_pin_t *pin,
    uint64_t request,
    uint64_t arg)
{
    return lpr_backend_ioctl(pin->fd, request, arg);
}

static int64_t lpr_ops_stat(
    const lpr_fd_pin_t *pin,
    uint64_t statbuf,
    uint64_t unused)
{
    (void)unused;
    return lpr_backend_fstat(pin->fd, statbuf);
}

static int64_t lpr_ops_mmap(
    const lpr_fd_pin_t *pin,
    uint64_t addr,
    uint64_t len,
    uint64_t prot,
    uint64_t flags,
    uint64_t offset)
{
    return lpr_backend_mmap(addr, len, prot, flags, pin->fd, offset);
}

static int64_t lpr_ops_socket_read(
    const lpr_fd_pin_t *pin,
    uint64_t buffer,
    uint64_t count)
{
    return lpr_linux_socket_read(pin->fd, buffer, count);
}

static int64_t lpr_ops_socket_write(
    const lpr_fd_pin_t *pin,
    uint64_t buffer,
    uint64_t count)
{
    return lpr_linux_socket_write(pin->fd, buffer, count);
}

static int64_t lpr_ops_socket_readv(
    const lpr_fd_pin_t *pin,
    uint64_t iov,
    uint64_t count)
{
    return lpr_linux_socket_readv(pin->fd, iov, count);
}

static int64_t lpr_ops_socket_writev(
    const lpr_fd_pin_t *pin,
    uint64_t iov,
    uint64_t count)
{
    return lpr_linux_socket_writev(pin->fd, iov, count);
}

static int64_t lpr_ops_socket_ioctl(
    const lpr_fd_pin_t *pin,
    uint64_t request,
    uint64_t arg)
{
    return lpr_linux_socket_ioctl(pin->fd, request, arg);
}

static int64_t lpr_ops_socket_stat(
    const lpr_fd_pin_t *pin,
    uint64_t statbuf,
    uint64_t unused)
{
    (void)unused;
    return lpr_linux_socket_fstat(pin->fd, statbuf);
}

static int64_t lpr_ops_no_close(void *state)
{
    (void)state;
    return 0;
}

static int64_t lpr_ops_dup(const lpr_fd_pin_t *pin)
{
    return pin != 0 && pin->state != 0 ? 0 : -LPR_LINUX_EBADF;
}

static int64_t lpr_ops_filed_close(void *state)
{
    const lpr_filed_backend_t *filed = state;
    return filed->handle != 0 ? lpr_filed_close_handle(filed->handle) : 0;
}

static int64_t lpr_ops_tty_close(void *state)
{
    const lpr_tty_backend_t *tty = state;
    return tty->handle != 0 ?
        lpr_termd_call_handle(TERMD_OP_HANDLE_CLOSE, tty->handle, 0) : 0;
}

static int64_t lpr_ops_drm_close(void *state)
{
    const lpr_drm_backend_t *drm = state;
    if (drm->wait_fd.raw >= 16) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)drm->wait_fd.raw);
    }
    return drm->handle != 0 ? lpr_drm_close_handle(drm->handle) : 0;
}

static int64_t lpr_ops_input_close(void *state)
{
    const lpr_input_backend_t *input = state;
    if (input->wait_fd.raw >= 16) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)input->wait_fd.raw);
    }
    return input->handle != 0 ? lpr_input_close_handle(input->handle) : 0;
}

static int64_t lpr_ops_pipe_close(void *state)
{
    const lpr_pipe_backend_t *pipe = state;
    if (pipe->native.raw < 0) {
        return 0;
    }
    return lpr_pacha_status_to_errno(lpr_pacha_syscall1(
        PACHAOS_SYSCALL_FD_CLOSE,
        (uint64_t)(uint32_t)pipe->native.raw));
}

static int64_t lpr_ops_dmabuf_close(void *state)
{
    const lpr_dmabuf_backend_t *dmabuf = state;
    int64_t close_status = 0;
    if (dmabuf->native.raw >= 0) {
        close_status = lpr_pacha_status_to_errno(lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE,
            (uint64_t)(uint32_t)dmabuf->native.raw));
    }
    const int64_t release_status = dmabuf->token != 0 ?
        lpr_drm_prime_ref(DRMD_OP_PRIME_RELEASE, dmabuf->token) : 0;
    return close_status != 0 ? close_status : release_status;
}

static int64_t lpr_ops_epoll_close(void *state)
{
    const lpr_epoll_backend_t *epoll = state;
    if (epoll->instance < 4096 || epoll->map_bytes == 0) {
        return 0;
    }
    return lpr_pacha_status_to_errno(lpr_pacha_syscall2(
        PACHAOS_SYSCALL_MUNMAP,
        epoll->instance,
        epoll->map_bytes));
}

static const lpr_fd_ops_t lpr_fd_ops_registry[LPR_FD_OPS_COUNT] = {
    [LPR_FD_OPS_FILED] = {
        lpr_ops_read, lpr_ops_write, lpr_ops_readv, lpr_ops_writev,
        lpr_ops_ioctl, lpr_ops_stat, lpr_ops_mmap, lpr_ops_filed_close, lpr_ops_dup,
    },
    [LPR_FD_OPS_DEVICE] = {
        lpr_ops_read, lpr_ops_write, lpr_ops_readv, lpr_ops_writev,
        lpr_ops_ioctl, lpr_ops_stat, 0, lpr_ops_no_close, lpr_ops_dup,
    },
    [LPR_FD_OPS_TTY] = {
        lpr_ops_read, lpr_ops_write, lpr_ops_readv, lpr_ops_writev,
        lpr_ops_ioctl, lpr_ops_stat, 0, lpr_ops_tty_close, lpr_ops_dup,
    },
    [LPR_FD_OPS_DRM] = {
        lpr_ops_read, 0, lpr_ops_readv, 0,
        lpr_ops_ioctl, lpr_ops_stat, lpr_ops_mmap, lpr_ops_drm_close, lpr_ops_dup,
    },
    [LPR_FD_OPS_INPUT] = {
        lpr_ops_read, 0, lpr_ops_readv, 0,
        lpr_ops_ioctl, lpr_ops_stat, 0, lpr_ops_input_close, lpr_ops_dup,
    },
    [LPR_FD_OPS_PIPE] = {
        lpr_ops_read, lpr_ops_write, lpr_ops_readv, lpr_ops_writev,
        lpr_ops_ioctl, lpr_ops_stat, 0, lpr_ops_pipe_close, lpr_ops_dup,
    },
    [LPR_FD_OPS_EVENT] = {
        lpr_ops_read, lpr_ops_write, lpr_ops_readv, lpr_ops_writev,
        lpr_ops_ioctl, lpr_ops_stat, 0, lpr_ops_no_close, lpr_ops_dup,
    },
    [LPR_FD_OPS_SOCKET] = {
        lpr_ops_socket_read, lpr_ops_socket_write,
        lpr_ops_socket_readv, lpr_ops_socket_writev,
        lpr_ops_socket_ioctl, lpr_ops_socket_stat, 0, lpr_socket_close_backend,
        lpr_ops_dup,
    },
    [LPR_FD_OPS_EPOLL] = {
        0, 0, 0, 0, lpr_ops_ioctl, lpr_ops_stat, 0, lpr_ops_epoll_close,
        lpr_ops_dup,
    },
    [LPR_FD_OPS_DMABUF] = {
        0, 0, 0, 0, lpr_ops_ioctl, lpr_ops_stat, lpr_ops_mmap,
        lpr_ops_dmabuf_close, lpr_ops_dup,
    },
};

static const lpr_fd_ops_t *lpr_fd_ops_for(uint8_t ops_id)
{
    return ops_id > LPR_FD_OPS_NONE && ops_id < LPR_FD_OPS_COUNT ?
        &lpr_fd_ops_registry[ops_id] : 0;
}

void lpr_fd_unpin(const lpr_fd_pin_t *pin)
{
    lpr_fd_drop_t drop;
    if (lpr_fd_table_unpin(&lpr_control_fd_table, pin, &drop) == 0 && drop.ready) {
        (void)lpr_backend_finish_drop(&drop);
    }
}

int64_t lpr_fd_prepare_dup(uint64_t fd)
{
    if (fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EBADF;
    }
    lpr_fd_pin_t pin;
    if (lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, &pin) != 0) {
        return -LPR_LINUX_EBADF;
    }
    const lpr_fd_ops_t *ops = lpr_fd_ops_for(pin.ops_id);
    const int64_t result = (pin.effective_rights & LPR_FD_RIGHT_DUP) == 0 ?
        -LPR_LINUX_EBADF :
        (ops != 0 && ops->dup != 0 ? ops->dup(&pin) : -LPR_LINUX_EOPNOTSUPP);
    lpr_fd_unpin(&pin);
    return result;
}

static int64_t lpr_fd_dispatch_io(
    uint64_t fd,
    uint64_t arg0,
    uint64_t arg1,
    uint32_t operation)
{
    if (fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EBADF;
    }
    lpr_fd_pin_t pin;
    if (lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, &pin) != 0) {
        return -LPR_LINUX_EBADF;
    }
    const lpr_fd_ops_t *ops = lpr_fd_ops_for(pin.ops_id);
    lpr_fd_io_op_t callback = 0;
    uint32_t required_right = 0;
    if (ops != 0) {
        switch (operation) {
        case 0: callback = ops->read; required_right = LPR_FD_RIGHT_READ; break;
        case 1: callback = ops->write; required_right = LPR_FD_RIGHT_WRITE; break;
        case 2: callback = ops->readv; required_right = LPR_FD_RIGHT_READ; break;
        case 3: callback = ops->writev; required_right = LPR_FD_RIGHT_WRITE; break;
        case 4: callback = ops->ioctl; required_right = LPR_FD_RIGHT_IOCTL; break;
        case 5: callback = ops->stat; required_right = LPR_FD_RIGHT_STAT; break;
        default: break;
        }
    }
    const int64_t result = (pin.effective_rights & required_right) == 0 ?
        -LPR_LINUX_EBADF :
        (callback != 0 ? callback(&pin, arg0, arg1) : -LPR_LINUX_EOPNOTSUPP);
    lpr_fd_unpin(&pin);
    return result;
}

int64_t lpr_linux_read(uint64_t fd, uint64_t buffer, uint64_t count)
{
    return lpr_fd_dispatch_io(fd, buffer, count, 0);
}

int64_t lpr_linux_write(uint64_t fd, uint64_t buffer, uint64_t count)
{
    return lpr_fd_dispatch_io(fd, buffer, count, 1);
}

int64_t lpr_linux_readv(uint64_t fd, uint64_t iov, uint64_t count)
{
    return lpr_fd_dispatch_io(fd, iov, count, 2);
}

int64_t lpr_linux_writev(uint64_t fd, uint64_t iov, uint64_t count)
{
    return lpr_fd_dispatch_io(fd, iov, count, 3);
}

int64_t lpr_linux_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
    return lpr_fd_dispatch_io(fd, request, arg, 4);
}

int64_t lpr_linux_fstat(uint64_t fd, uint64_t statbuf)
{
    return lpr_fd_dispatch_io(fd, statbuf, 0, 5);
}

int64_t lpr_fd_dispatch_mmap(
    uint64_t addr,
    uint64_t len,
    uint64_t prot,
    uint64_t flags,
    uint64_t fd,
    uint64_t offset)
{
    if (fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EBADF;
    }
    lpr_fd_pin_t pin;
    if (lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, &pin) != 0) {
        return -LPR_LINUX_EBADF;
    }
    const lpr_fd_ops_t *ops = lpr_fd_ops_for(pin.ops_id);
    const int64_t result = (pin.effective_rights & LPR_FD_RIGHT_MMAP) == 0 ?
        -LPR_LINUX_EBADF :
        (ops != 0 && ops->mmap != 0 ?
            ops->mmap(&pin, addr, len, prot, flags, offset) : -LPR_LINUX_EOPNOTSUPP);
    lpr_fd_unpin(&pin);
    return result;
}

int64_t lpr_backend_finish_drop(const lpr_fd_drop_t *drop)
{
    if (drop == 0 || !drop->ready || drop->state == 0 || drop->state_bytes == 0) {
        return 0;
    }
    const lpr_fd_ops_t *ops = lpr_fd_ops_for(drop->ops_id);
    const int64_t close_status = ops != 0 && ops->close != 0 ?
        ops->close(drop->state) : 0;
    const int64_t unmap_status = lpr_pacha_status_to_errno(lpr_pacha_syscall2(
        PACHAOS_SYSCALL_MUNMAP,
        (uint64_t)(uintptr_t)drop->state,
        drop->state_bytes));
    return close_status != 0 ? close_status : unmap_status;
}

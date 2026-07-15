#include "../lpr_filed_internal.h"

#define LPR_LINUX_EMSGSIZE 90

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
    const int transfer_lease =
        (filed->reserved2 & LPR_BACKEND_TRANSFER_LEASE) != 0;
    const int64_t handle_status = !transfer_lease && filed->handle != 0 ?
        lpr_filed_close_handle(filed->handle) : 0;
    const int64_t lease_status = filed->lease_fd.raw >= 16 ?
        lpr_close_native_fd_if_open((uint64_t)(uint32_t)filed->lease_fd.raw) : 0;
    return handle_status != 0 ? handle_status : lease_status;
}

static int64_t lpr_ops_tty_close(void *state)
{
    const lpr_tty_backend_t *tty = state;
    const int transfer_lease =
        (tty->reserved1 & LPR_BACKEND_TRANSFER_LEASE) != 0;
    const int64_t handle_status = !transfer_lease && tty->handle != 0 ?
        lpr_termd_call_handle(TERMD_OP_HANDLE_CLOSE, tty->handle, 0) : 0;
    const int64_t wait_status = tty->wait_fd.raw >= 16 ?
        lpr_close_native_fd_if_open((uint64_t)(uint32_t)tty->wait_fd.raw) : 0;
    const int64_t lease_status = tty->lease_fd.raw >= 16 ?
        lpr_close_native_fd_if_open((uint64_t)(uint32_t)tty->lease_fd.raw) : 0;
    return handle_status != 0 ? handle_status :
        (wait_status != 0 ? wait_status : lease_status);
}

static int64_t lpr_ops_drm_close(void *state)
{
    const lpr_drm_backend_t *drm = state;
    if (drm->wait_fd.raw >= 16) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)drm->wait_fd.raw);
    }
    if (drm->lease_fd.raw >= 16)
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)drm->lease_fd.raw);
    return (drm->reserved1 & LPR_BACKEND_TRANSFER_LEASE) == 0 && drm->handle != 0 ?
        lpr_drm_close_handle(drm->handle) : 0;
}

static int64_t lpr_ops_input_close(void *state)
{
    const lpr_input_backend_t *input = state;
    if (input->wait_fd.raw >= 16) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)input->wait_fd.raw);
    }
    if (input->lease_fd.raw >= 16)
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)input->lease_fd.raw);
    return (input->reserved1 & LPR_BACKEND_TRANSFER_LEASE) == 0 && input->handle != 0 ?
        lpr_input_close_handle(input->handle) : 0;
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
    const int transfer_lease =
        (dmabuf->reserved0 & LPR_BACKEND_TRANSFER_LEASE) != 0;
    const int64_t release_status = !transfer_lease && dmabuf->token != 0 ?
        lpr_drm_prime_ref(DRMD_OP_PRIME_RELEASE, dmabuf->token) : 0;
    const int64_t lease_status = dmabuf->lease_fd.raw >= 16 ?
        lpr_close_native_fd_if_open((uint64_t)(uint32_t)dmabuf->lease_fd.raw) : 0;
    return close_status != 0 ? close_status :
        (release_status != 0 ? release_status : lease_status);
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

static uint64_t lpr_transfer_input_handle(uint64_t token)
{
    return token & 0x00ffffffffffffffull;
}

static int lpr_transfer_duplicate_capability(int source_fd)
{
    struct pacha_fd_info info;
    lpr_memset(&info, 0, sizeof(info));
    if (source_fd < 16 ||
        !lpr_native_fd_info((uint64_t)(uint32_t)source_fd, &info) ||
        (info.rights & (PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER)) !=
            (PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER))
        return -LPR_LINUX_EBADF;
    const int64_t duplicate = lpr_pacha_syscall4(
        PACHA_FD_SYSCALL_FCNTL,
        (uint64_t)(uint32_t)source_fd,
        PACHA_FD_FCNTL_DUP,
        16,
        info.rights);
    return duplicate >= 16 ? (int)duplicate : (int)lpr_pacha_status_to_errno(duplicate);
}

int lpr_fd_transfer_prepare(
    const lpr_fd_pin_t *pin,
    netd_transfer_occurrence_t *item,
    int *capability_fds,
    uint32_t capability_capacity,
    uint32_t *out_capability_count)
{
    if (pin == 0 || pin->state == 0 || item == 0 || out_capability_count == 0)
        return -LPR_LINUX_EINVAL;
    lpr_memset(item, 0, sizeof(*item));
    *out_capability_count = 0;
    item->provider_id = pin->ops_id;
    item->rights = pin->effective_rights;
    if (pin->ops_id == LPR_FD_OPS_FILED) {
        const lpr_filed_backend_t *filed = pin->state;
        if ((filed->reserved1 & LPR_FILED_FD_MEMFD) == 0 ||
            filed->handle == 0 || filed->handle > UINT32_MAX)
            return -LPR_LINUX_EOPNOTSUPP;
        if (capability_fds == 0 || capability_capacity < 1)
            return -LPR_LINUX_EMSGSIZE;
        int lease_fd = -1;
        int remote_lease_fd = -1;
        const int pair_status = lpr_native_wait_pair(&lease_fd, &remote_lease_fd);
        if (pair_status != 0) return pair_status;
        uint64_t ticket = 0;
        const int64_t status = lpr_filed_transfer_dup_handle(
            filed->handle, 0, remote_lease_fd, &ticket);
        if (status != 0) {
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)lease_fd);
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)remote_lease_fd);
            return (int)status;
        }
        if (ticket == 0 || ticket > UINT32_MAX) {
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)lease_fd);
            return -LPR_LINUX_EIO;
        }
        item->transfer_token = ticket |
            ((uint64_t)filed->reserved1 << 32u);
        item->fd_flags = filed->flags & ~(uint32_t)LPR_LINUX_O_CLOEXEC;
        item->capability_count = 1;
        capability_fds[0] = lease_fd;
        *out_capability_count = 1;
        return 0;
    }
    if (pin->ops_id == LPR_FD_OPS_INPUT) {
        const lpr_input_backend_t *input = pin->state;
        if (input->handle == 0 || input->handle > 0x00ffffffffffffffull ||
            input->wait_fd.raw < 16)
            return -LPR_LINUX_EOPNOTSUPP;
        if (capability_fds == 0 || capability_capacity < 2)
            return -LPR_LINUX_EMSGSIZE;
        const int wait_fd = lpr_transfer_duplicate_capability(input->wait_fd.raw);
        if (wait_fd < 0) return wait_fd;
        int lease_fd = -1;
        int remote_lease_fd = -1;
        const int pair_status = lpr_native_wait_pair(&lease_fd, &remote_lease_fd);
        if (pair_status != 0) {
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
            return pair_status;
        }
        uint64_t ticket = 0;
        const int64_t status = lpr_input_transfer_dup_handle(
            input->handle, remote_lease_fd, &ticket);
        if (status != 0) {
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)lease_fd);
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)remote_lease_fd);
            return (int)status;
        }
        if (ticket != input->handle) {
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)lease_fd);
            return -LPR_LINUX_EIO;
        }
        item->transfer_token = ticket | ((uint64_t)input->reserved0 << 56u);
        item->fd_flags = input->flags & ~(uint32_t)LPR_LINUX_O_CLOEXEC;
        item->capability_count = 2;
        capability_fds[0] = wait_fd;
        capability_fds[1] = lease_fd;
        *out_capability_count = 2;
        return 0;
    }
    if (pin->ops_id == LPR_FD_OPS_DRM) {
        const lpr_drm_backend_t *drm = pin->state;
        if (drm->handle == 0 || drm->wait_fd.raw < 16)
            return -LPR_LINUX_EOPNOTSUPP;
        if (capability_fds == 0 || capability_capacity < 2)
            return -LPR_LINUX_EMSGSIZE;
        const int wait_fd = lpr_transfer_duplicate_capability(drm->wait_fd.raw);
        if (wait_fd < 0) return wait_fd;
        int lease_fd = -1;
        int remote_lease_fd = -1;
        const int pair_status = lpr_native_wait_pair(&lease_fd, &remote_lease_fd);
        if (pair_status != 0) {
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
            return pair_status;
        }
        uint64_t ticket = 0;
        const int64_t status = lpr_drm_transfer_dup_handle(
            drm->handle, remote_lease_fd, &ticket);
        if (status != 0) {
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)lease_fd);
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)remote_lease_fd);
            return (int)status;
        }
        if (ticket != drm->handle) {
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)lease_fd);
            return -LPR_LINUX_EIO;
        }
        item->transfer_token = ticket;
        item->fd_flags = drm->flags & ~(uint32_t)LPR_LINUX_O_CLOEXEC;
        item->capability_count = 2;
        capability_fds[0] = wait_fd;
        capability_fds[1] = lease_fd;
        *out_capability_count = 2;
        return 0;
    }
    return -LPR_LINUX_EOPNOTSUPP;
}

void lpr_fd_transfer_cancel_ticket(const netd_transfer_occurrence_t *item)
{
    (void)item;
}

int lpr_fd_transfer_import_batch(
    const netd_transfer_occurrence_t *items,
    uint32_t item_count,
    const int *capability_fds,
    uint32_t capability_count,
    uint32_t receive_flags,
    int *out_fds)
{
    const uint64_t known_rights = LPR_FD_RIGHT_READ | LPR_FD_RIGHT_WRITE |
        LPR_FD_RIGHT_IOCTL | LPR_FD_RIGHT_STAT | LPR_FD_RIGHT_MMAP |
        LPR_FD_RIGHT_DUP;
    if (items == 0 || out_fds == 0 || item_count == 0 ||
        item_count > NETD_TRANSFER_MAX_ITEMS ||
        capability_count > NETD_TRANSFER_MAX_CAPABILITIES ||
        capability_fds == 0 ||
        (receive_flags & ~(uint32_t)LPR_LINUX_O_CLOEXEC) != 0)
        return -LPR_LINUX_EINVAL;
    lpr_fd_install_t installs[NETD_TRANSFER_MAX_ITEMS];
    void *states[NETD_TRANSFER_MAX_ITEMS];
    lpr_linux_fd_t installed[NETD_TRANSFER_MAX_ITEMS];
    lpr_memset(installs, 0, sizeof(installs));
    lpr_memset(states, 0, sizeof(states));
    lpr_memset(installed, 0, sizeof(installed));
    uint32_t prepared_count = 0;
    uint32_t next_capability = 0;
    int status = 0;
    for (; prepared_count < item_count; ++prepared_count) {
        const netd_transfer_occurrence_t *item = &items[prepared_count];
        if (item->transfer_token == 0 || item->reserved0 != 0 ||
            (item->rights & ~known_rights) != 0 ||
            item->capability_first != next_capability ||
            item->capability_count > capability_count - next_capability)
        {
            status = -LPR_LINUX_EINVAL;
            break;
        }
        const int *item_capabilities = capability_fds + next_capability;
        uint64_t handle = item->transfer_token;
        if (item->provider_id == LPR_FD_OPS_FILED) {
            if (item->capability_count != 1 || item_capabilities[0] < 16 ||
                (item->transfer_token >> 40u) != 0)
            {
                status = -LPR_LINUX_EINVAL;
                break;
            }
            handle = (uint32_t)item->transfer_token;
        } else if (item->provider_id == LPR_FD_OPS_INPUT) {
            if (item->capability_count != 2 || item_capabilities[0] < 16 ||
                item_capabilities[1] < 16)
            {
                status = -LPR_LINUX_EINVAL;
                break;
            }
            handle = lpr_transfer_input_handle(item->transfer_token);
        } else if (item->provider_id == LPR_FD_OPS_DRM) {
            if (item->capability_count != 2 || item_capabilities[0] < 16 ||
                item_capabilities[1] < 16)
            {
                status = -LPR_LINUX_EINVAL;
                break;
            }
        } else {
            status = -LPR_LINUX_EOPNOTSUPP;
            break;
        }
        const int64_t mapped = lpr_pacha_syscall6(
            PACHAOS_SYSCALL_MMAP,
            0,
            0,
            4096,
            PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
            PACHAOS_MMAP_PRIVATE | PACHAOS_MMAP_ANONYMOUS,
            0);
        if (mapped < 4096) {
            status = -LPR_LINUX_ENOMEM;
            break;
        }
        states[prepared_count] = (void *)(uintptr_t)mapped;
        lpr_memset(states[prepared_count], 0, 4096);
        const uint64_t linux_flags = item->fd_flags | receive_flags;
        if (item->provider_id == LPR_FD_OPS_FILED) {
            lpr_filed_backend_t *filed = states[prepared_count];
            filed->active = 1;
            filed->offset_valid = 1;
            filed->flags = (uint32_t)linux_flags;
            filed->handle = handle;
            filed->reserved1 = (uint8_t)(item->transfer_token >> 32u);
            filed->reserved2 |= LPR_BACKEND_TRANSFER_LEASE;
            filed->lease_fd.raw = item_capabilities[0];
        } else if (item->provider_id == LPR_FD_OPS_INPUT) {
            lpr_input_backend_t *input = states[prepared_count];
            input->active = 1;
            input->flags = (uint32_t)linux_flags;
            input->handle = handle;
            input->reserved0 = (uint8_t)(item->transfer_token >> 56u);
            input->reserved1 |= LPR_BACKEND_TRANSFER_LEASE;
            input->wait_fd.raw = item_capabilities[0];
            input->lease_fd.raw = item_capabilities[1];
        } else {
            lpr_drm_backend_t *drm = states[prepared_count];
            drm->active = 1;
            drm->flags = (uint32_t)linux_flags;
            drm->handle = handle;
            drm->reserved1 |= LPR_BACKEND_TRANSFER_LEASE;
            drm->wait_fd.raw = item_capabilities[0];
            drm->lease_fd.raw = item_capabilities[1];
        }
        lpr_fd_install_t *install = &installs[prepared_count];
        install->ops_id = (uint8_t)item->provider_id;
        install->fd_flags = lpr_control_fd_flags_from_linux(linux_flags);
        install->access_mode =
            (uint16_t)(linux_flags & LPR_LINUX_O_ACCMODE);
        install->status_flags =
            lpr_control_status_flags_from_linux(linux_flags);
        install->rights = (uint32_t)item->rights;
        install->offset = 0;
        install->backend_state = states[prepared_count];
        install->backend_state_bytes = 4096;
        next_capability += item->capability_count;
    }
    if (status == 0 && next_capability != capability_count)
        status = -LPR_LINUX_EINVAL;
    if (status == 0) {
        const lpr_linux_fd_t excluded[] = {
            LPR_FILED_ENDPOINT_FD,
            LPR_NETD_ENDPOINT_FD,
            LPR_TERMD_TTY_ENDPOINT_FD,
            LPR_DRMD_DRM_ENDPOINT_FD,
            LPR_INPUTD_INPUT_ENDPOINT_FD,
            LPR_BOOTSTRAP_FD,
            LPR_SUPERVISOR_ENDPOINT_FD,
        };
        int batch_status = lpr_fd_table_alloc_batch(
                &lpr_control_fd_table,
                3,
                installs,
                item_count,
                excluded,
                (uint32_t)(sizeof(excluded) / sizeof(excluded[0])),
                installed);
        if (batch_status != 0) {
            const uint64_t required_capacity =
                lpr_fd_table_capacity + (uint64_t)item_count;
            if (required_capacity == 0 ||
                required_capacity > LPR_FD_TABLE_MAX_SIZE ||
                lpr_fd_table_ensure_capacity(required_capacity) != 0)
            {
                status = -LPR_LINUX_EMFILE;
            } else {
                batch_status = lpr_fd_table_alloc_batch(
                    &lpr_control_fd_table,
                    3,
                    installs,
                    item_count,
                    excluded,
                    (uint32_t)(sizeof(excluded) / sizeof(excluded[0])),
                    installed);
                if (batch_status != 0) status = -LPR_LINUX_EMFILE;
            }
        }
    }
    if (status != 0) {
        for (uint32_t i = 0; i < prepared_count; ++i)
            if (states[i] != 0)
                (void)lpr_pacha_syscall2(
                    PACHAOS_SYSCALL_MUNMAP,
                    (uint64_t)(uintptr_t)states[i],
                    4096);
        return status;
    }
    for (uint32_t i = 0; i < item_count; ++i)
        out_fds[i] = (int)installed[i];
    return status;
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

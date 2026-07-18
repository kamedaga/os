#include "../lpr_filed_internal.h"

#define LPR_LINUX_EMSGSIZE 90

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
    const int transfer_lease =
        (drm->reserved1 & LPR_BACKEND_TRANSFER_LEASE) != 0;
    const int64_t handle_status = !transfer_lease && drm->handle != 0 ?
        lpr_drm_close_handle(drm->handle) : 0;
    const int64_t wait_status = drm->wait_fd.raw >= 16 ?
        lpr_close_native_fd_if_open((uint64_t)(uint32_t)drm->wait_fd.raw) : 0;
    const int64_t lease_status = drm->lease_fd.raw >= 16 ?
        lpr_close_native_fd_if_open((uint64_t)(uint32_t)drm->lease_fd.raw) : 0;
    return handle_status != 0 ? handle_status :
        (wait_status != 0 ? wait_status : lease_status);
}

static int64_t lpr_ops_input_close(void *state)
{
    const lpr_input_backend_t *input = state;
    const int64_t status =
        (input->reserved1 & LPR_BACKEND_TRANSFER_LEASE) == 0 && input->handle != 0 ?
            lpr_input_close_handle(input->handle) : 0;
    if (input->wait_fd.raw >= 16) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)input->wait_fd.raw);
    }
    if (input->lease_fd.raw >= 16)
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)input->lease_fd.raw);
    return status;
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

static int64_t lpr_ops_event_close(void *state)
{
    const lpr_event_backend_t *event = state;
    int64_t status = 0;
    if (event->wait_fd.raw >= 16)
        status = lpr_close_native_fd_if_open(
            (uint64_t)(uint32_t)event->wait_fd.raw);
    if (event->notify_fd.raw >= 16) {
        const int64_t notify_status = lpr_close_native_fd_if_open(
            (uint64_t)(uint32_t)event->notify_fd.raw);
        if (status == 0) status = notify_status;
    }
    return status;
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

static int64_t lpr_ops_sync_file_close(void *state)
{
    const lpr_sync_file_backend_t *sync_file = state;
    return sync_file->wait_fd.raw >= 16 ?
        lpr_close_native_fd_if_open(
            (uint64_t)(uint32_t)sync_file->wait_fd.raw) : 0;
}

static int64_t lpr_ops_epoll_close(void *state)
{
    const lpr_epoll_backend_t *epoll = state;
    int64_t status = 0;
    if (epoll->wait_fd.raw >= 16)
        status = lpr_close_native_fd_if_open(
            (uint64_t)(uint32_t)epoll->wait_fd.raw);
    if (epoll->notify_fd.raw >= 16) {
        const int64_t notify_status = lpr_close_native_fd_if_open(
            (uint64_t)(uint32_t)epoll->notify_fd.raw);
        if (status == 0) status = notify_status;
    }
    if (epoll->instance >= 4096 && epoll->map_bytes != 0) {
        const int64_t map_status = lpr_pacha_status_to_errno(lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            epoll->instance,
            epoll->map_bytes));
        if (status == 0) status = map_status;
    }
    return status;
}

static int64_t lpr_fd_close_backend(uint8_t ops_id, void *state)
{
    switch (ops_id) {
    case LPR_FD_OPS_FILED: return lpr_ops_filed_close(state);
    case LPR_FD_OPS_DEVICE: return 0;
    case LPR_FD_OPS_TTY: return lpr_ops_tty_close(state);
    case LPR_FD_OPS_DRM: return lpr_ops_drm_close(state);
    case LPR_FD_OPS_INPUT: return lpr_ops_input_close(state);
    case LPR_FD_OPS_PIPE: return lpr_ops_pipe_close(state);
    case LPR_FD_OPS_EVENT: return lpr_ops_event_close(state);
    case LPR_FD_OPS_SOCKET: return lpr_socket_close_backend(state);
    case LPR_FD_OPS_EPOLL: return lpr_ops_epoll_close(state);
    case LPR_FD_OPS_DMABUF: return lpr_ops_dmabuf_close(state);
    case LPR_FD_OPS_SYNC_FILE: return lpr_ops_sync_file_close(state);
    default: return 0;
    }
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
        item->transfer_token = ticket | ((uint64_t)input->event_index << 56u);
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
    if (pin->ops_id == LPR_FD_OPS_SYNC_FILE) {
        const lpr_sync_file_backend_t *sync_file = pin->state;
        if (sync_file->wait_fd.raw < 16 || capability_fds == 0 ||
            capability_capacity < 1)
            return -LPR_LINUX_EOPNOTSUPP;
        const int wait_fd =
            lpr_transfer_duplicate_capability(sync_file->wait_fd.raw);
        if (wait_fd < 0) return wait_fd;
        item->transfer_token = 1;
        item->fd_flags =
            sync_file->flags & ~(uint32_t)LPR_LINUX_O_CLOEXEC;
        item->capability_count = 1;
        capability_fds[0] = wait_fd;
        *out_capability_count = 1;
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
        } else if (item->provider_id == LPR_FD_OPS_SYNC_FILE) {
            if (item->transfer_token != 1 || item->capability_count != 1 ||
                item_capabilities[0] < 16)
            {
                status = -LPR_LINUX_EINVAL;
                break;
            }
        } else {
            status = -LPR_LINUX_EOPNOTSUPP;
            break;
        }
        const uint64_t state_bytes =
            lpr_backend_state_bytes_for_ops((uint8_t)item->provider_id);
        states[prepared_count] = lpr_backend_state_alloc(state_bytes);
        if (states[prepared_count] == 0) {
            status = -LPR_LINUX_ENOMEM;
            break;
        }
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
            input->event_index = (uint8_t)(item->transfer_token >> 56u);
            input->reserved1 |= LPR_BACKEND_TRANSFER_LEASE;
            input->wait_fd.raw = item_capabilities[0];
            input->lease_fd.raw = item_capabilities[1];
        } else if (item->provider_id == LPR_FD_OPS_SYNC_FILE) {
            lpr_sync_file_backend_t *sync_file = states[prepared_count];
            sync_file->active = 1;
            sync_file->flags = (uint32_t)linux_flags;
            sync_file->wait_fd.raw = item_capabilities[0];
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
        install->backend_state_bytes = state_bytes;
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
        for (uint32_t i = 0; i < prepared_count; ++i) {
            if (states[i] != 0) {
                (void)lpr_backend_state_free(
                    states[i], installs[i].backend_state_bytes);
            }
        }
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
    int64_t result = 0;
    if ((pin.effective_rights & LPR_FD_RIGHT_DUP) == 0 || pin.state == 0)
        result = -LPR_LINUX_EBADF;
    else if (pin.ops_id <= LPR_FD_OPS_NONE || pin.ops_id >= LPR_FD_OPS_COUNT)
        result = -LPR_LINUX_EOPNOTSUPP;
    lpr_fd_unpin(&pin);
    return result;
}

static int lpr_fd_io_supported(uint8_t ops_id, uint32_t operation)
{
    switch (ops_id) {
    case LPR_FD_OPS_FILED:
    case LPR_FD_OPS_DEVICE:
    case LPR_FD_OPS_TTY:
    case LPR_FD_OPS_PIPE:
    case LPR_FD_OPS_EVENT:
    case LPR_FD_OPS_SOCKET:
        return operation <= 5;
    case LPR_FD_OPS_DRM:
    case LPR_FD_OPS_INPUT:
        return operation == 0 || operation == 2 || operation == 4 || operation == 5;
    case LPR_FD_OPS_EPOLL:
    case LPR_FD_OPS_DMABUF:
        return operation == 4 || operation == 5;
    case LPR_FD_OPS_SYNC_FILE:
        return operation == 5;
    default:
        return 0;
    }
}

static int64_t lpr_fd_dispatch_direct(
    const lpr_fd_pin_t *pin,
    uint64_t arg0,
    uint64_t arg1,
    uint32_t operation)
{
    if (pin->ops_id == LPR_FD_OPS_SOCKET) {
        switch (operation) {
        case 0: return lpr_linux_socket_read(pin->fd, arg0, arg1);
        case 1: return lpr_linux_socket_write(pin->fd, arg0, arg1);
        case 2: return lpr_linux_socket_readv(pin->fd, arg0, arg1);
        case 3: return lpr_linux_socket_writev(pin->fd, arg0, arg1);
        case 4: return lpr_linux_socket_ioctl(pin->fd, arg0, arg1);
        case 5: return lpr_linux_socket_fstat(pin->fd, arg0);
        default: return -LPR_LINUX_EOPNOTSUPP;
        }
    }
    switch (operation) {
    case 0: return lpr_backend_read(pin, arg0, arg1);
    case 1: return lpr_backend_write(pin->fd, arg0, arg1);
    case 2: return lpr_backend_readv(pin, arg0, arg1);
    case 3: return lpr_backend_writev(pin->fd, arg0, arg1);
    case 4: return lpr_backend_ioctl(pin->fd, arg0, arg1);
    case 5: return lpr_backend_fstat(pin->fd, arg0);
    default: return -LPR_LINUX_EOPNOTSUPP;
    }
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
    uint32_t required_right = 0;
    switch (operation) {
    case 0: required_right = LPR_FD_RIGHT_READ; break;
    case 1: required_right = LPR_FD_RIGHT_WRITE; break;
    case 2: required_right = LPR_FD_RIGHT_READ; break;
    case 3: required_right = LPR_FD_RIGHT_WRITE; break;
    case 4: required_right = LPR_FD_RIGHT_IOCTL; break;
    case 5: required_right = LPR_FD_RIGHT_STAT; break;
    default: break;
    }
    int64_t result = -LPR_LINUX_EOPNOTSUPP;
    if (required_right != 0 && (pin.effective_rights & required_right) == 0)
        result = -LPR_LINUX_EBADF;
    else if (required_right != 0 && lpr_fd_io_supported(pin.ops_id, operation))
        result = lpr_fd_dispatch_direct(&pin, arg0, arg1, operation);
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
    int64_t result = -LPR_LINUX_EOPNOTSUPP;
    if ((pin.effective_rights & LPR_FD_RIGHT_MMAP) == 0) {
        result = -LPR_LINUX_EBADF;
    } else if (pin.ops_id == LPR_FD_OPS_FILED ||
        pin.ops_id == LPR_FD_OPS_DRM || pin.ops_id == LPR_FD_OPS_DMABUF)
    {
        result = lpr_backend_mmap(addr, len, prot, flags, pin.fd, offset);
    }
    lpr_fd_unpin(&pin);
    return result;
}

int64_t lpr_backend_finish_drop(const lpr_fd_drop_t *drop)
{
    if (drop == 0 || !drop->ready || drop->state == 0 || drop->state_bytes == 0) {
        return 0;
    }
    const int64_t close_status = lpr_fd_close_backend(drop->ops_id, drop->state);
    const int64_t free_status =
        lpr_backend_state_free(drop->state, drop->state_bytes);
    return close_status != 0 ? close_status : free_status;
}

#include "../lpr_filed_internal.h"

static uint64_t lpr_backend_type_bytes(uint8_t ops_id)
{
    switch (ops_id) {
    case LPR_FD_OPS_FILED: return sizeof(lpr_filed_backend_t);
    case LPR_FD_OPS_DEVICE: return sizeof(lpr_device_backend_t);
    case LPR_FD_OPS_TTY: return sizeof(lpr_tty_backend_t);
    case LPR_FD_OPS_DRM: return sizeof(lpr_drm_backend_t);
    case LPR_FD_OPS_INPUT: return sizeof(lpr_input_backend_t);
    case LPR_FD_OPS_PIPE: return sizeof(lpr_pipe_backend_t);
    case LPR_FD_OPS_EVENT: return sizeof(lpr_event_backend_t);
    case LPR_FD_OPS_SOCKET: return sizeof(lpr_socket_backend_t);
    case LPR_FD_OPS_EPOLL: return sizeof(lpr_epoll_backend_t);
    case LPR_FD_OPS_DMABUF: return sizeof(lpr_dmabuf_backend_t);
    default: return 0;
    }
}

static uint32_t lpr_backend_rights(uint8_t ops_id, uint64_t linux_flags)
{
    const uint32_t common = LPR_FD_RIGHT_STAT | LPR_FD_RIGHT_DUP;
    const uint64_t access = linux_flags & LPR_LINUX_O_ACCMODE;
    const uint32_t readable = access != LPR_LINUX_O_WRONLY ? LPR_FD_RIGHT_READ : 0;
    const uint32_t writable = access != LPR_LINUX_O_RDONLY ? LPR_FD_RIGHT_WRITE : 0;
    switch (ops_id) {
    case LPR_FD_OPS_FILED:
        return common | readable | writable | LPR_FD_RIGHT_MMAP | LPR_FD_RIGHT_IOCTL;
    case LPR_FD_OPS_DEVICE:
    case LPR_FD_OPS_TTY:
        return common | readable | writable | LPR_FD_RIGHT_IOCTL;
    case LPR_FD_OPS_DRM:
        return common | LPR_FD_RIGHT_READ | LPR_FD_RIGHT_IOCTL | LPR_FD_RIGHT_MMAP;
    case LPR_FD_OPS_INPUT:
        return common | LPR_FD_RIGHT_READ | LPR_FD_RIGHT_IOCTL;
    case LPR_FD_OPS_PIPE:
    case LPR_FD_OPS_SOCKET:
        return common | readable | writable | LPR_FD_RIGHT_IOCTL;
    case LPR_FD_OPS_EVENT:
        return common | LPR_FD_RIGHT_READ | LPR_FD_RIGHT_WRITE | LPR_FD_RIGHT_IOCTL;
    case LPR_FD_OPS_EPOLL:
        return common | LPR_FD_RIGHT_IOCTL;
    case LPR_FD_OPS_DMABUF:
        return common | LPR_FD_RIGHT_IOCTL | LPR_FD_RIGHT_MMAP;
    default:
        return 0;
    }
}

static void *lpr_backend_map(void)
{
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        0,
        0,
        4096,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_PRIVATE | PACHAOS_MMAP_ANONYMOUS,
        0);
    if (mapped < 4096) {
        return 0;
    }
    void *state = (void *)(uintptr_t)mapped;
    lpr_memset(state, 0, 4096);
    return state;
}

void *lpr_backend_state_from_ofd(const lpr_ofd_t *ofd)
{
    if (ofd == 0 || !ofd->active ||
        ofd->backend.index >= lpr_control_fd_table.backend_count)
    {
        return 0;
    }
    const lpr_backend_record_t *backend =
        &lpr_control_fd_table.backends[ofd->backend.index];
    return backend->active && backend->generation == ofd->backend.generation ?
        backend->state : 0;
}

uint8_t lpr_ofd_ops_id(const lpr_ofd_t *ofd)
{
    if (ofd == 0 || !ofd->active ||
        ofd->backend.index >= lpr_control_fd_table.backend_count)
    {
        return LPR_FD_OPS_NONE;
    }
    const lpr_backend_record_t *backend =
        &lpr_control_fd_table.backends[ofd->backend.index];
    return backend->active && backend->generation == ofd->backend.generation ?
        backend->ops_id : LPR_FD_OPS_NONE;
}

static void *lpr_backend_state_for_fd(uint64_t fd, uint8_t ops_id)
{
    lpr_fd_arrays_init();
    if (fd >= lpr_fd_table_capacity) {
        return 0;
    }
    void *state = 0;
    lpr_fd_table_lock(&lpr_control_fd_table);
    const lpr_fd_entry_t *entry = &lpr_control_fd_table.entries[fd];
    if (entry->active && entry->ofd_index < lpr_control_fd_table.ofd_count) {
        const lpr_ofd_t *ofd = &lpr_control_fd_table.ofds[entry->ofd_index];
        if (ofd->active && ofd->generation == entry->ofd_generation &&
            lpr_ofd_ops_id(ofd) == ops_id)
        {
            state = lpr_backend_state_from_ofd(ofd);
        }
    }
    lpr_fd_table_unlock(&lpr_control_fd_table);
    return state;
}

lpr_filed_backend_t *lpr_filed_backend(uint64_t fd)
{
    return (lpr_filed_backend_t *)lpr_backend_state_for_fd(fd, LPR_FD_OPS_FILED);
}

lpr_device_backend_t *lpr_device_backend(uint64_t fd)
{
    return (lpr_device_backend_t *)lpr_backend_state_for_fd(fd, LPR_FD_OPS_DEVICE);
}

lpr_pipe_backend_t *lpr_pipe_backend(uint64_t fd)
{
    return (lpr_pipe_backend_t *)lpr_backend_state_for_fd(fd, LPR_FD_OPS_PIPE);
}

lpr_event_backend_t *lpr_event_backend(uint64_t fd)
{
    return (lpr_event_backend_t *)lpr_backend_state_for_fd(fd, LPR_FD_OPS_EVENT);
}

lpr_tty_backend_t *lpr_tty_backend(uint64_t fd)
{
    return (lpr_tty_backend_t *)lpr_backend_state_for_fd(fd, LPR_FD_OPS_TTY);
}

lpr_drm_backend_t *lpr_drm_backend(uint64_t fd)
{
    return (lpr_drm_backend_t *)lpr_backend_state_for_fd(fd, LPR_FD_OPS_DRM);
}

lpr_input_backend_t *lpr_input_backend(uint64_t fd)
{
    return (lpr_input_backend_t *)lpr_backend_state_for_fd(fd, LPR_FD_OPS_INPUT);
}

lpr_dmabuf_backend_t *lpr_dmabuf_backend(uint64_t fd)
{
    return (lpr_dmabuf_backend_t *)lpr_backend_state_for_fd(fd, LPR_FD_OPS_DMABUF);
}

lpr_socket_backend_t *lpr_socket_backend(uint64_t fd)
{
    return (lpr_socket_backend_t *)lpr_backend_state_for_fd(fd, LPR_FD_OPS_SOCKET);
}

lpr_epoll_backend_t *lpr_epoll_backend(uint64_t fd)
{
    return (lpr_epoll_backend_t *)lpr_backend_state_for_fd(fd, LPR_FD_OPS_EPOLL);
}

static int lpr_fd_ops_active(uint64_t fd, uint8_t ops_id)
{
    return lpr_backend_state_for_fd(fd, ops_id) != 0;
}

int lpr_fd_is_filed(uint64_t fd)
{
    return lpr_fd_ops_active(fd, LPR_FD_OPS_FILED);
}

int lpr_linux_device_fd_active(uint64_t fd)
{
    return lpr_fd_ops_active(fd, LPR_FD_OPS_DEVICE);
}

int lpr_linux_tty_fd_active(uint64_t fd)
{
    return lpr_fd_ops_active(fd, LPR_FD_OPS_TTY);
}

int lpr_linux_drm_fd_active(uint64_t fd)
{
    return lpr_fd_ops_active(fd, LPR_FD_OPS_DRM);
}

int lpr_linux_input_fd_active(uint64_t fd)
{
    return lpr_fd_ops_active(fd, LPR_FD_OPS_INPUT);
}

int lpr_linux_dmabuf_fd_active(uint64_t fd)
{
    return lpr_fd_ops_active(fd, LPR_FD_OPS_DMABUF);
}

int lpr_fd_shadow_offset_eligible(uint64_t fd)
{
    const lpr_filed_backend_t *filed = lpr_filed_backend(fd);
    return filed != 0 &&
        (filed->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY;
}

int lpr_linux_filed_fd_active(uint64_t fd)
{
    return lpr_fd_is_filed(fd);
}

uint64_t lpr_linux_filed_fd_handle(uint64_t fd)
{
    const lpr_filed_backend_t *filed = lpr_filed_backend(fd);
    return filed != 0 ? filed->handle : 0;
}

int lpr_runtime_reserved_fd(uint64_t fd)
{
    return fd == LPR_FILED_ENDPOINT_FD ||
        fd == LPR_NETD_ENDPOINT_FD ||
        fd == LPR_TERMD_TTY_ENDPOINT_FD ||
        fd == LPR_DRMD_DRM_ENDPOINT_FD ||
        fd == LPR_INPUTD_INPUT_ENDPOINT_FD ||
        fd == LPR_BOOTSTRAP_FD ||
        fd == LPR_SUPERVISOR_ENDPOINT_FD;
}

int lpr_native_fd_info(uint64_t fd, struct pacha_fd_info *out)
{
    if (fd > LPR_LINUX_FD_MAX || out == 0) {
        return 0;
    }
    lpr_memset(out, 0, sizeof(*out));
    return lpr_pacha_syscall2(
        PACHAOS_SYSCALL_FD_GET_INFO,
        fd,
        (uint64_t)(uintptr_t)out) == 0;
}

int64_t lpr_close_native_fd_if_open(uint64_t fd)
{
    struct pacha_fd_info info;
    if (fd > LPR_LINUX_FD_MAX || lpr_runtime_reserved_fd(fd)) {
        return 0;
    }
    if (!lpr_native_fd_info(fd, &info)) {
        return 0;
    }
    return lpr_pacha_status_to_errno(
        lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, fd));
}

int lpr_fd_local_active(uint64_t fd)
{
    return lpr_control_fd_active(fd);
}

uint32_t lpr_pipe_flags_from_info(const struct pacha_fd_info *info)
{
    uint32_t flags = 0;
    const int readable = (info->rights & PACHA_FD_RIGHT_READ) != 0;
    const int writable = (info->rights & PACHA_FD_RIGHT_WRITE) != 0;
    if (readable && writable) {
        flags |= LPR_LINUX_O_RDWR;
    } else if (writable) {
        flags |= LPR_LINUX_O_WRONLY;
    }
    if ((info->flags & PACHA_FD_FLAG_NONBLOCK) != 0) {
        flags |= LPR_LINUX_O_NONBLOCK;
    }
    if ((info->flags & PACHA_FD_FLAG_CLOEXEC) != 0) {
        flags |= LPR_LINUX_O_CLOEXEC;
    }
    return flags;
}

uint16_t lpr_control_fd_flags_from_linux(uint64_t flags)
{
    return (flags & LPR_LINUX_O_CLOEXEC) != 0 ? LPR_FD_ENTRY_CLOEXEC : 0;
}

uint16_t lpr_control_fd_flags_from_fcntl(uint64_t flags)
{
    return (flags & LPR_LINUX_FD_CLOEXEC) != 0 ? LPR_FD_ENTRY_CLOEXEC : 0;
}

uint32_t lpr_control_status_flags_from_linux(uint64_t flags)
{
    uint32_t status = 0;
    if ((flags & LPR_LINUX_O_NONBLOCK) != 0) {
        status |= LPR_OFD_NONBLOCK;
    }
    if ((flags & LPR_LINUX_O_APPEND) != 0) {
        status |= LPR_OFD_APPEND;
    }
    return status;
}

uint32_t lpr_control_status_flags_to_linux(uint32_t status)
{
    uint32_t flags = 0;
    if ((status & LPR_OFD_NONBLOCK) != 0) {
        flags |= LPR_LINUX_O_NONBLOCK;
    }
    if ((status & LPR_OFD_APPEND) != 0) {
        flags |= LPR_LINUX_O_APPEND;
    }
    return flags;
}

uint32_t lpr_control_merge_backend_flags(
    uint32_t old_flags,
    uint16_t fd_flags,
    uint32_t status_flags)
{
    uint32_t flags = old_flags & ~(uint32_t)(
        LPR_LINUX_O_CLOEXEC |
        LPR_LINUX_O_NONBLOCK |
        LPR_LINUX_O_APPEND);
    if ((fd_flags & LPR_FD_ENTRY_CLOEXEC) != 0) {
        flags |= LPR_LINUX_O_CLOEXEC;
    }
    flags |= lpr_control_status_flags_to_linux(status_flags);
    return flags;
}

int lpr_control_install_fd(
    uint64_t fd,
    uint8_t ops_id,
    uint64_t linux_flags,
    uint64_t backend_id,
    uint64_t offset)
{
    if (fd > LPR_LINUX_FD_MAX || lpr_backend_type_bytes(ops_id) == 0) {
        return -LPR_LINUX_EMFILE;
    }
    const int ensure_status = lpr_fd_table_ensure_fd(fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    uint16_t existing_flags = 0;
    if (lpr_fd_table_get_fd_flags(&lpr_control_fd_table, (uint32_t)fd, &existing_flags) == 0) {
        return -LPR_LINUX_EMFILE;
    }
    void *state = lpr_backend_map();
    if (state == 0) {
        return -LPR_LINUX_ENOMEM;
    }
    switch (ops_id) {
    case LPR_FD_OPS_FILED: {
        lpr_filed_backend_t *backend = (lpr_filed_backend_t *)state;
        backend->active = 1;
        backend->offset_valid = 1;
        backend->flags = (uint32_t)linux_flags;
        backend->handle = backend_id;
        backend->offset = offset;
        backend->lease_fd.raw = -1;
        break;
    }
    case LPR_FD_OPS_DEVICE: {
        lpr_device_backend_t *backend = (lpr_device_backend_t *)state;
        backend->active = 1;
        backend->major = (uint8_t)(backend_id >> 32u);
        backend->minor = (uint8_t)backend_id;
        backend->flags = (uint32_t)linux_flags;
        break;
    }
    case LPR_FD_OPS_TTY: {
        lpr_tty_backend_t *backend = (lpr_tty_backend_t *)state;
        backend->active = 1;
        backend->flags = (uint32_t)linux_flags;
        backend->handle = backend_id;
        backend->wait_fd.raw = -1;
        backend->lease_fd.raw = -1;
        break;
    }
    case LPR_FD_OPS_DRM: {
        lpr_drm_backend_t *backend = (lpr_drm_backend_t *)state;
        backend->active = 1;
        backend->flags = (uint32_t)linux_flags;
        backend->handle = backend_id;
        backend->wait_fd.raw = -1;
        backend->lease_fd.raw = -1;
        break;
    }
    case LPR_FD_OPS_INPUT: {
        lpr_input_backend_t *backend = (lpr_input_backend_t *)state;
        backend->active = 1;
        backend->flags = (uint32_t)linux_flags;
        backend->handle = backend_id;
        backend->wait_fd.raw = -1;
        backend->lease_fd.raw = -1;
        break;
    }
    case LPR_FD_OPS_DMABUF: {
        lpr_dmabuf_backend_t *backend = (lpr_dmabuf_backend_t *)state;
        backend->active = 1;
        backend->writable =
            (linux_flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDWR ? 1u : 0u;
        backend->flags = (uint32_t)linux_flags;
        backend->token = backend_id;
        backend->size = offset;
        backend->native.raw = -1;
        backend->lease_fd.raw = -1;
        break;
    }
    case LPR_FD_OPS_PIPE: {
        lpr_pipe_backend_t *backend = (lpr_pipe_backend_t *)state;
        backend->active = 1;
        backend->flags = (uint32_t)linux_flags;
        backend->native.raw = (int32_t)backend_id;
        break;
    }
    case LPR_FD_OPS_EVENT: {
        lpr_event_backend_t *backend = (lpr_event_backend_t *)state;
        backend->active = 1;
        backend->flags = (uint32_t)linux_flags;
        backend->counter = offset;
        backend->wait_fd.raw = -1;
        backend->notify_fd.raw = -1;
        break;
    }
    case LPR_FD_OPS_SOCKET: {
        lpr_socket_backend_t *backend = (lpr_socket_backend_t *)state;
        backend->active = 1;
        backend->flags = (uint32_t)linux_flags;
        backend->handle = backend_id;
        backend->wait_fd.raw = -1;
        backend->lease_fd.raw = -1;
        break;
    }
    case LPR_FD_OPS_EPOLL: {
        lpr_epoll_backend_t *backend = (lpr_epoll_backend_t *)state;
        backend->active = 1;
        backend->flags = (uint32_t)linux_flags;
        backend->instance = backend_id;
        backend->map_bytes = offset;
        backend->wait_fd.raw = -1;
        backend->notify_fd.raw = -1;
        break;
    }
    default:
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)state,
            4096);
        return -LPR_LINUX_EINVAL;
    }
    const lpr_fd_install_t install = {
        .ops_id = ops_id,
        .fd_flags = lpr_control_fd_flags_from_linux(linux_flags),
        .access_mode = (uint16_t)(linux_flags & LPR_LINUX_O_ACCMODE),
        .status_flags = lpr_control_status_flags_from_linux(linux_flags),
        .rights = lpr_backend_rights(ops_id, linux_flags),
        .offset = offset,
        .backend_state = state,
        .backend_state_bytes = 4096,
    };
    if (lpr_fd_table_install_at(&lpr_control_fd_table, (uint32_t)fd, &install) != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)state,
            4096);
        return -LPR_LINUX_EMFILE;
    }
    return 0;
}

void lpr_control_close_fd(uint64_t fd)
{
    if (fd < lpr_fd_table_capacity) {
        lpr_epoll_before_close(fd);
        lpr_fd_drop_t drop;
        if (lpr_fd_table_close(&lpr_control_fd_table, (uint32_t)fd, &drop) == 0 && drop.ready) {
            (void)lpr_backend_finish_drop(&drop);
        }
    }
}

int lpr_control_fd_active(uint64_t fd)
{
    if (fd >= lpr_fd_table_capacity) {
        return 0;
    }
    uint16_t fd_flags = 0;
    return lpr_fd_table_get_fd_flags(&lpr_control_fd_table, (uint32_t)fd, &fd_flags) == 0;
}

int lpr_control_require_fd(uint64_t fd)
{
    if (fd >= lpr_fd_table_capacity) {
        return -LPR_LINUX_EBADF;
    }
    uint16_t fd_flags = 0;
    if (lpr_fd_table_get_fd_flags(&lpr_control_fd_table, (uint32_t)fd, &fd_flags) == 0) {
        return 0;
    }
    return -LPR_LINUX_EBADF;
}

int lpr_control_dup_fd(uint64_t old_fd, uint64_t new_fd, uint64_t cloexec)
{
    const int64_t prepare_status = lpr_fd_prepare_dup(old_fd);
    if (prepare_status != 0) {
        return (int)prepare_status;
    }
    const uint16_t new_flags = cloexec ? LPR_FD_ENTRY_CLOEXEC : 0;
    return lpr_fd_table_dup_at(&lpr_control_fd_table, (uint32_t)old_fd, (uint32_t)new_fd, new_flags) == 0 ?
        0 :
        -LPR_LINUX_EBADF;
}

void lpr_control_sync_backend_flags(uint64_t fd)
{
    if (fd >= lpr_fd_table_capacity) {
        return;
    }
    uint32_t status_flags = 0;
    if (lpr_fd_table_get_status_flags(&lpr_control_fd_table, (uint32_t)fd, &status_flags) != 0)
    {
        return;
    }
    lpr_fd_pin_t pin;
    if (lpr_fd_table_pin(&lpr_control_fd_table, (uint32_t)fd, &pin) != 0) {
        return;
    }
    uint32_t *backend_flags = 0;
    switch (pin.ops_id) {
    case LPR_FD_OPS_FILED: backend_flags = &((lpr_filed_backend_t *)pin.state)->flags; break;
    case LPR_FD_OPS_DEVICE: backend_flags = &((lpr_device_backend_t *)pin.state)->flags; break;
    case LPR_FD_OPS_TTY: backend_flags = &((lpr_tty_backend_t *)pin.state)->flags; break;
    case LPR_FD_OPS_DRM: backend_flags = &((lpr_drm_backend_t *)pin.state)->flags; break;
    case LPR_FD_OPS_INPUT: backend_flags = &((lpr_input_backend_t *)pin.state)->flags; break;
    case LPR_FD_OPS_DMABUF: backend_flags = &((lpr_dmabuf_backend_t *)pin.state)->flags; break;
    case LPR_FD_OPS_EVENT: backend_flags = &((lpr_event_backend_t *)pin.state)->flags; break;
    case LPR_FD_OPS_PIPE: backend_flags = &((lpr_pipe_backend_t *)pin.state)->flags; break;
    case LPR_FD_OPS_SOCKET: backend_flags = &((lpr_socket_backend_t *)pin.state)->flags; break;
    case LPR_FD_OPS_EPOLL: backend_flags = &((lpr_epoll_backend_t *)pin.state)->flags; break;
    default: break;
    }
    if (backend_flags != 0) {
        *backend_flags = lpr_control_merge_backend_flags(*backend_flags, 0, status_flags);
    }
    lpr_fd_drop_t drop;
    if (lpr_fd_table_unpin(&lpr_control_fd_table, &pin, &drop) == 0 && drop.ready) {
        (void)lpr_backend_finish_drop(&drop);
    }
}

int lpr_control_set_fd_flags(uint64_t fd, uint64_t flags)
{
    const int ensure_status = lpr_control_require_fd(fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    if (lpr_fd_table_set_fd_flags(
            &lpr_control_fd_table,
            (uint32_t)fd,
            lpr_control_fd_flags_from_fcntl(flags)) != 0)
    {
        return -LPR_LINUX_EBADF;
    }
    return 0;
}

int lpr_control_set_status_flags(uint64_t fd, uint64_t flags)
{
    const int ensure_status = lpr_control_require_fd(fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    if (lpr_fd_table_set_status_flags(
            &lpr_control_fd_table,
            (uint32_t)fd,
            lpr_control_status_flags_from_linux(flags)) != 0)
    {
        return -LPR_LINUX_EBADF;
    }
    lpr_control_sync_backend_flags(fd);
    return 0;
}

int64_t lpr_control_get_fd_flags(uint64_t fd)
{
    const int ensure_status = lpr_control_require_fd(fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    uint16_t fd_flags = 0;
    if (lpr_fd_table_get_fd_flags(&lpr_control_fd_table, (uint32_t)fd, &fd_flags) != 0) {
        return -LPR_LINUX_EBADF;
    }
    return (fd_flags & LPR_FD_ENTRY_CLOEXEC) != 0 ? LPR_LINUX_FD_CLOEXEC : 0;
}

int lpr_control_fd_cloexec(uint64_t fd, int *out_cloexec)
{
    if (out_cloexec == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const int ensure_status = lpr_control_require_fd(fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    uint16_t fd_flags = 0;
    if (lpr_fd_table_get_fd_flags(&lpr_control_fd_table, (uint32_t)fd, &fd_flags) != 0) {
        return -LPR_LINUX_EBADF;
    }
    *out_cloexec = (fd_flags & LPR_FD_ENTRY_CLOEXEC) != 0;
    return 0;
}

int64_t lpr_control_get_status_flags(uint64_t fd, uint32_t access_mode)
{
    const int ensure_status = lpr_control_require_fd(fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    uint32_t status_flags = 0;
    if (lpr_fd_table_get_status_flags(&lpr_control_fd_table, (uint32_t)fd, &status_flags) != 0) {
        return -LPR_LINUX_EBADF;
    }
    return (int64_t)(access_mode | lpr_control_status_flags_to_linux(status_flags));
}

uint64_t lpr_filed_control_offset(uint64_t fd)
{
    uint64_t offset = 0;
    if (lpr_control_require_fd(fd) == 0 &&
        lpr_fd_table_get_offset(&lpr_control_fd_table, (uint32_t)fd, &offset) == 0)
    {
        return offset;
    }
    const lpr_filed_backend_t *filed = lpr_filed_backend(fd);
    return filed != 0 ? filed->offset : 0;
}

void lpr_filed_control_set_offset(uint64_t fd, uint64_t offset)
{
    if (fd < lpr_fd_table_capacity) {
        lpr_filed_backend_t *filed = lpr_filed_backend(fd);
        if (filed != 0) {
            filed->offset = offset;
        }
        if (lpr_control_require_fd(fd) == 0) {
            (void)lpr_fd_table_set_offset(&lpr_control_fd_table, (uint32_t)fd, offset);
        }
    }
}

void lpr_filed_control_advance_offset(uint64_t fd, uint64_t old_offset, uint64_t amount)
{
    const uint64_t new_offset = old_offset + amount;
    lpr_filed_control_set_offset(fd, new_offset);
    if (new_offset < old_offset) {
        lpr_filed_backend_t *filed = lpr_filed_backend(fd);
        if (filed != 0) {
            filed->offset_valid = 0;
        }
    }
}

int lpr_pipe_track_native_fd(
    uint64_t fd,
    uint64_t native_fd,
    const struct pacha_fd_info *info)
{
    if (fd > LPR_LINUX_FD_MAX || info == 0) {
        return -LPR_LINUX_EMFILE;
    }
    const int ensure_status = lpr_fd_table_ensure_fd(fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    const uint32_t flags = lpr_pipe_flags_from_info(info);
    const int control_status = lpr_control_install_fd(
        fd,
        LPR_FD_OPS_PIPE,
        flags,
        native_fd,
        0);
    if (control_status != 0) {
        return control_status;
    }
    lpr_pipe_backend_t *pipe = lpr_pipe_backend(fd);
    if (pipe == 0) {
        lpr_control_close_fd(fd);
        return -LPR_LINUX_EIO;
    }
    pipe->active = 1;
    pipe->readable = (info->rights & PACHA_FD_RIGHT_READ) != 0 ? 1u : 0u;
    pipe->writable = (info->rights & PACHA_FD_RIGHT_WRITE) != 0 ? 1u : 0u;
    pipe->flags = flags;
    pipe->native.raw = (int32_t)native_fd;
    return 0;
}

int lpr_fd_linux_visible_active(uint64_t fd)
{
    return lpr_control_fd_active(fd);
}

int lpr_fd_alloc(uint64_t handle, uint64_t flags)
{
    const int fd = lpr_fd_slot_alloc_from(3);
    if (fd < 0) {
        return fd;
    }
    const int control_status = lpr_control_install_fd(
        (uint64_t)fd,
        LPR_FD_OPS_FILED,
        flags,
        handle,
        0);
    if (control_status != 0) {
        return control_status;
    }
    lpr_filed_backend_t *filed = lpr_filed_backend((uint64_t)fd);
    if (filed == 0) {
        lpr_control_close_fd((uint64_t)fd);
        return -LPR_LINUX_EIO;
    }
    filed->active = 1;
    filed->offset_valid =
        ((flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY) ? 1u : 0u;
    filed->pread_active = 0;
    filed->flags = (uint32_t)flags;
    filed->handle = handle;
    filed->offset = 0;
    return fd;
}

int lpr_fd_slot_alloc(void)
{
    return lpr_fd_slot_alloc_from(3);
}

int lpr_fd_slot_available(uint64_t fd)
{
    lpr_fd_arrays_init();
    return fd < lpr_fd_table_capacity &&
        !lpr_runtime_reserved_fd(fd) &&
        !lpr_control_fd_active(fd);
}

int lpr_fd_slot_alloc_from(uint64_t min_fd)
{
    if (min_fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t fd = min_fd;
    for (;;) {
        if (fd >= lpr_fd_table_capacity) {
            const int status = lpr_fd_table_ensure_fd(fd);
            if (status != 0) {
                return status == -LPR_LINUX_EINVAL ? -LPR_LINUX_EMFILE : status;
            }
        }
        while (fd < lpr_fd_table_capacity) {
            if (lpr_fd_slot_available(fd)) {
                return (int)fd;
            }
            fd++;
        }
        if (lpr_fd_table_capacity >= LPR_FD_TABLE_MAX_SIZE) {
            return -LPR_LINUX_EMFILE;
        }
    }
}

int lpr_control_set_effective_rights(uint32_t fd, uint32_t rights)
{
    int status = -1;
    lpr_fd_table_lock(&lpr_control_fd_table);
    if (fd < lpr_control_fd_table.entry_count &&
        lpr_control_fd_table.entries[fd].active)
    {
        lpr_control_fd_table.entries[fd].effective_rights = rights;
        status = 0;
    }
    lpr_fd_table_unlock(&lpr_control_fd_table);
    return status;
}

static int lpr_manifest_install_first(
    uint32_t fd,
    const lpr_manifest_entry_t *entry,
    const lpr_manifest_ofd_t *ofd,
    const void *record)
{
    if (entry == 0 || ofd == 0 || record == 0 ||
        ofd->backend_id == 0 || ofd->backend_id >= LPR_FD_OPS_COUNT ||
        ofd->record_bytes != lpr_backend_type_bytes((uint8_t)ofd->backend_id) ||
        lpr_fd_table_ensure_fd(fd) != 0 || !lpr_fd_slot_available(fd))
    {
        return 0;
    }
    void *state = lpr_backend_map();
    if (state == 0) return 0;
    lpr_memcpy(state, record, ofd->record_bytes);
    const lpr_fd_install_t install = {
        .ops_id = (uint8_t)ofd->backend_id,
        .fd_flags = entry->fd_flags,
        .access_mode = ofd->access_mode,
        .status_flags = ofd->status_flags,
        .rights = ofd->rights_ceiling,
        .offset = ofd->offset,
        .backend_state = state,
        .backend_state_bytes = 4096,
    };
    if (lpr_fd_table_install_at(&lpr_control_fd_table, fd, &install) != 0) {
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)state, 4096);
        return 0;
    }
    return lpr_control_set_effective_rights(fd, (uint32_t)entry->effective_rights) == 0;
}

int lpr_install_manifest_fds(const lpr_manifest_t *manifest)
{
    if (lpr_manifest_fds_installed) return 1;
    lpr_manifest_fds_installed = 1;
    if (manifest == 0) return 0;
    const lpr_manifest_entry_t *entries =
        (const lpr_manifest_entry_t *)((const uint8_t *)manifest + manifest->entry_offset);
    const lpr_manifest_ofd_t *ofds =
        (const lpr_manifest_ofd_t *)((const uint8_t *)manifest + manifest->ofd_offset);
    const lpr_manifest_capability_t *capabilities =
        (const lpr_manifest_capability_t *)((const uint8_t *)manifest + manifest->capability_offset);
    const uint8_t *records = (const uint8_t *)manifest + manifest->record_offset;
    for (uint64_t i = 0; i < manifest->capability_count; ++i) {
        struct pacha_fd_info info;
        if (capabilities[i].ordinal != i ||
            capabilities[i].native_fd > INT32_MAX ||
            !lpr_native_fd_info(capabilities[i].native_fd, &info) ||
            (info.rights & capabilities[i].rights) != capabilities[i].rights)
        {
            return 0;
        }
    }
    for (uint64_t oi = 0; oi < manifest->ofd_count; ++oi) {
        const lpr_manifest_entry_t *first = 0;
        for (uint64_t ei = 0; ei < manifest->entry_count; ++ei) {
            if (entries[ei].ofd_index == oi) {
                first = &entries[ei];
                break;
            }
        }
        if (first == 0 || first->fd > LPR_LINUX_FD_MAX ||
            lpr_runtime_reserved_fd(first->fd) ||
            !lpr_manifest_install_first(first->fd, first, &ofds[oi],
                records + ofds[oi].record_offset))
        {
            goto rollback;
        }
        for (uint64_t ei = 0; ei < manifest->entry_count; ++ei) {
            const lpr_manifest_entry_t *entry = &entries[ei];
            if (entry == first || entry->ofd_index != oi) continue;
            if (entry->fd > LPR_LINUX_FD_MAX || lpr_runtime_reserved_fd(entry->fd) ||
                lpr_fd_table_ensure_fd(entry->fd) != 0 ||
                lpr_fd_table_dup_at(&lpr_control_fd_table, first->fd, entry->fd,
                    entry->fd_flags) != 0 ||
                lpr_control_set_effective_rights(
                    entry->fd, (uint32_t)entry->effective_rights) != 0)
            {
                goto rollback;
            }
        }
    }
    return 1;

rollback:
    for (uint64_t ei = 0; ei < manifest->entry_count; ++ei)
        if (entries[ei].fd <= LPR_LINUX_FD_MAX &&
            lpr_control_fd_active(entries[ei].fd))
            lpr_control_close_fd(entries[ei].fd);
    return 0;
}

void lpr_state_dump(const char *reason)
{
    lpr_fd_arrays_init();
    lpr_fd_table_lock(&lpr_control_fd_table);
    const uint64_t reason_id = reason != 0 ? pacha_trace_name_id(reason) : 0;
    uint64_t open_count = 0;
    uint64_t live_object_count = 0;
    for (uint64_t fd = 0; fd < lpr_fd_table_capacity; fd += 1) {
        open_count += lpr_control_fd_table.entries[fd].active ? 1u : 0u;
        live_object_count += lpr_control_fd_table.ofds[fd].active ? 1u : 0u;
    }
    uint64_t crc = lpr_control_fd_table.generation ^ open_count;
    pacha_trace6(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_PROCESS,
        PACHA_TRACE_CLASS_ERROR,
        pacha_trace_name_id("lpr.state.begin"),
        (uint64_t)(uint32_t)lpr_linux_current_pid,
        lpr_control_fd_table.generation,
        lpr_fd_table_capacity,
        open_count,
        reason_id);
    pacha_trace6(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_PROCESS,
        PACHA_TRACE_CLASS_ERROR,
        pacha_trace_name_id("lpr.state.counts"),
        (uint64_t)(uint32_t)lpr_linux_current_pid,
        lpr_control_fd_table.generation,
        open_count,
        live_object_count,
        reason_id);
    for (uint64_t fd = 0; fd < lpr_fd_table_capacity; fd += 1) {
        const lpr_fd_entry_t *slot = &lpr_control_fd_table.entries[fd];
        if (!slot->active || slot->ofd_index >= lpr_control_fd_table.ofd_count) {
            continue;
        }
        const lpr_ofd_t *object = &lpr_control_fd_table.ofds[slot->ofd_index];
        if (!object->active) {
            continue;
        }
        crc ^= (fd << 32u) ^ lpr_ofd_ops_id(object) ^ object->refcount ^ object->backend.index;
        pacha_trace6(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_LPR_PROCESS,
            PACHA_TRACE_CLASS_ERROR,
            pacha_trace_name_id("lpr.fd.entry"),
            fd,
            lpr_ofd_ops_id(object),
            slot->fd_flags,
            object->status_flags,
            object->refcount);
        switch (lpr_ofd_ops_id(object)) {
        case LPR_FD_OPS_FILED:
            pacha_trace6(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_PROCESS,
                PACHA_TRACE_CLASS_ERROR,
                pacha_trace_name_id("lpr.fd.filed"),
                fd,
                ((lpr_filed_backend_t *)lpr_backend_state_from_ofd(object))->handle,
                ((lpr_filed_backend_t *)lpr_backend_state_from_ofd(object))->offset,
                ((lpr_filed_backend_t *)lpr_backend_state_from_ofd(object))->offset_valid,
                lpr_page_cache_clock);
            break;
        case LPR_FD_OPS_PIPE:
            pacha_trace6(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_PROCESS,
                PACHA_TRACE_CLASS_ERROR,
                pacha_trace_name_id("lpr.fd.pipe"),
                fd,
                fd,
                ((lpr_pipe_backend_t *)lpr_backend_state_from_ofd(object))->readable,
                ((lpr_pipe_backend_t *)lpr_backend_state_from_ofd(object))->writable,
                ((lpr_pipe_backend_t *)lpr_backend_state_from_ofd(object))->last_wait_events);
            break;
        case LPR_FD_OPS_EVENT:
            pacha_trace3(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_PROCESS,
                PACHA_TRACE_CLASS_ERROR,
                pacha_trace_name_id("lpr.fd.eventfd"),
                fd,
                ((lpr_event_backend_t *)lpr_backend_state_from_ofd(object))->counter);
            break;
        case LPR_FD_OPS_TTY:
            pacha_trace4(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_PROCESS,
                PACHA_TRACE_CLASS_ERROR,
                pacha_trace_name_id("lpr.fd.tty"),
                fd,
                ((lpr_tty_backend_t *)lpr_backend_state_from_ofd(object))->handle,
                (uint64_t)(uint32_t)lpr_linux_current_pgrp);
            break;
        case LPR_FD_OPS_DRM:
            pacha_trace4(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_PROCESS,
                PACHA_TRACE_CLASS_ERROR,
                pacha_trace_name_id("lpr.fd.drm"),
                fd,
                ((lpr_drm_backend_t *)lpr_backend_state_from_ofd(object))->handle,
                ((lpr_drm_backend_t *)lpr_backend_state_from_ofd(object))->flags);
            break;
        case LPR_FD_OPS_INPUT:
            pacha_trace4(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_PROCESS,
                PACHA_TRACE_CLASS_ERROR,
                pacha_trace_name_id("lpr.fd.input"),
                fd,
                ((lpr_input_backend_t *)lpr_backend_state_from_ofd(object))->handle,
                ((lpr_input_backend_t *)lpr_backend_state_from_ofd(object))->flags);
            break;
        case LPR_FD_OPS_SOCKET:
            pacha_trace6(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_PROCESS,
                PACHA_TRACE_CLASS_ERROR,
                pacha_trace_name_id("lpr.fd.socket"),
                fd,
                ((lpr_socket_backend_t *)lpr_backend_state_from_ofd(object))->handle,
                ((lpr_socket_backend_t *)lpr_backend_state_from_ofd(object))->connected,
                ((lpr_socket_backend_t *)lpr_backend_state_from_ofd(object))->connecting,
                (uint64_t)(uint32_t)((lpr_socket_backend_t *)lpr_backend_state_from_ofd(object))->last_error);
            break;
        case LPR_FD_OPS_EPOLL:
            pacha_trace4(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_PROCESS,
                PACHA_TRACE_CLASS_ERROR,
                pacha_trace_name_id("lpr.fd.epoll"),
                fd,
                ((lpr_epoll_backend_t *)lpr_backend_state_from_ofd(object))->instance,
                ((lpr_epoll_backend_t *)lpr_backend_state_from_ofd(object))->map_bytes);
            break;
        default:
            break;
        }
    }
    pacha_trace3(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_PROCESS,
        PACHA_TRACE_CLASS_ERROR,
        pacha_trace_name_id("lpr.state.end"),
        lpr_control_fd_table.generation,
        crc);
    lpr_fd_table_unlock(&lpr_control_fd_table);
}

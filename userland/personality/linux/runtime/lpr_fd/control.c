#include "../lpr_filed_internal.h"

lpr_fd_object_t *lpr_fd_object_for_fd(uint64_t fd)
{
    lpr_fd_arrays_init();
    if (fd >= lpr_fd_table_capacity) {
        return 0;
    }
    return lpr_fd_table_object_for_fd(&lpr_control_fd_table, (uint32_t)fd);
}

const lpr_fd_object_t *lpr_fd_object_for_fd_const(uint64_t fd)
{
    lpr_fd_arrays_init();
    if (fd >= lpr_fd_table_capacity) {
        return 0;
    }
    return lpr_fd_table_object_for_fd_const(&lpr_control_fd_table, (uint32_t)fd);
}

lpr_filed_fd_t *lpr_fd_filed_payload(uint64_t fd)
{
    lpr_fd_object_t *object = lpr_fd_object_for_fd(fd);
    return object != 0 && object->kind == LPR_FD_TABLE_KIND_FILED ? &object->payload.filed : 0;
}

lpr_pipe_fd_t *lpr_fd_pipe_payload(uint64_t fd)
{
    lpr_fd_object_t *object = lpr_fd_object_for_fd(fd);
    return object != 0 && object->kind == LPR_FD_TABLE_KIND_PIPE ? &object->payload.pipe : 0;
}

lpr_event_fd_t *lpr_fd_event_payload(uint64_t fd)
{
    lpr_fd_object_t *object = lpr_fd_object_for_fd(fd);
    return object != 0 && object->kind == LPR_FD_TABLE_KIND_EVENT ? &object->payload.eventfd : 0;
}

lpr_tty_fd_t *lpr_fd_tty_payload(uint64_t fd)
{
    lpr_fd_object_t *object = lpr_fd_object_for_fd(fd);
    return object != 0 && object->kind == LPR_FD_TABLE_KIND_TTY ? &object->payload.tty : 0;
}

lpr_drm_fd_t *lpr_fd_drm_payload(uint64_t fd)
{
    lpr_fd_object_t *object = lpr_fd_object_for_fd(fd);
    return object != 0 && object->kind == LPR_FD_TABLE_KIND_DRM ? &object->payload.drm : 0;
}

lpr_dmabuf_fd_t *lpr_fd_dmabuf_payload(uint64_t fd)
{
    lpr_fd_object_t *object = lpr_fd_object_for_fd(fd);
    return object != 0 && object->kind == LPR_FD_TABLE_KIND_DMABUF ? &object->payload.dmabuf : 0;
}

lpr_socket_fd_t *lpr_fd_socket_payload(uint64_t fd)
{
    lpr_fd_object_t *object = lpr_fd_object_for_fd(fd);
    return object != 0 && object->kind == LPR_FD_TABLE_KIND_SOCKET ? &object->payload.socket : 0;
}

static int lpr_fd_kind_active(uint64_t fd, uint8_t kind)
{
    const lpr_fd_object_t *object = lpr_fd_object_for_fd_const(fd);
    return object != 0 && object->kind == kind;
}

int lpr_fd_is_filed(uint64_t fd)
{
    return lpr_fd_kind_active(fd, LPR_FD_TABLE_KIND_FILED);
}

int lpr_linux_tty_fd_active(uint64_t fd)
{
    return lpr_fd_kind_active(fd, LPR_FD_TABLE_KIND_TTY);
}

int lpr_linux_drm_fd_active(uint64_t fd)
{
    return lpr_fd_kind_active(fd, LPR_FD_TABLE_KIND_DRM);
}

int lpr_linux_dmabuf_fd_active(uint64_t fd)
{
    return lpr_fd_kind_active(fd, LPR_FD_TABLE_KIND_DMABUF);
}

int lpr_fd_shadow_offset_eligible(uint64_t fd)
{
    const lpr_filed_fd_t *filed = lpr_fd_filed_payload(fd);
    return filed != 0 &&
        (filed->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY;
}

int lpr_linux_filed_fd_active(uint64_t fd)
{
    return lpr_fd_is_filed(fd);
}

uint64_t lpr_linux_filed_fd_handle(uint64_t fd)
{
    const lpr_filed_fd_t *filed = lpr_fd_filed_payload(fd);
    return filed != 0 ? filed->handle : 0;
}

int lpr_runtime_reserved_fd(uint64_t fd)
{
    return fd == LPR_FILED_ENDPOINT_FD ||
        fd == LPR_NETD_ENDPOINT_FD ||
        fd == LPR_TERMD_TTY_ENDPOINT_FD ||
        fd == LPR_DRMD_DRM_ENDPOINT_FD ||
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
    const lpr_fd_object_t *object = lpr_fd_object_for_fd_const(fd);
    return object != 0 &&
        (object->kind == LPR_FD_TABLE_KIND_FILED ||
            object->kind == LPR_FD_TABLE_KIND_PIPE ||
            object->kind == LPR_FD_TABLE_KIND_EVENT ||
            object->kind == LPR_FD_TABLE_KIND_TTY ||
            object->kind == LPR_FD_TABLE_KIND_DRM ||
            object->kind == LPR_FD_TABLE_KIND_DMABUF ||
            object->kind == LPR_FD_TABLE_KIND_SOCKET ||
            object->kind == LPR_FD_TABLE_KIND_EPOLL);
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
    return (flags & LPR_LINUX_O_CLOEXEC) != 0 ? LPR_FD_TABLE_FD_CLOEXEC : 0;
}

uint16_t lpr_control_fd_flags_from_fcntl(uint64_t flags)
{
    return (flags & LPR_LINUX_FD_CLOEXEC) != 0 ? LPR_FD_TABLE_FD_CLOEXEC : 0;
}

uint32_t lpr_control_status_flags_from_linux(uint64_t flags)
{
    uint32_t status = 0;
    if ((flags & LPR_LINUX_O_NONBLOCK) != 0) {
        status |= LPR_FD_TABLE_STATUS_NONBLOCK;
    }
    if ((flags & LPR_LINUX_O_APPEND) != 0) {
        status |= LPR_FD_TABLE_STATUS_APPEND;
    }
    return status;
}

uint32_t lpr_control_status_flags_to_linux(uint32_t status)
{
    uint32_t flags = 0;
    if ((status & LPR_FD_TABLE_STATUS_NONBLOCK) != 0) {
        flags |= LPR_LINUX_O_NONBLOCK;
    }
    if ((status & LPR_FD_TABLE_STATUS_APPEND) != 0) {
        flags |= LPR_LINUX_O_APPEND;
    }
    return flags;
}

uint32_t lpr_control_merge_legacy_flags(
    uint32_t old_flags,
    uint16_t fd_flags,
    uint32_t status_flags)
{
    uint32_t flags = old_flags & ~(uint32_t)(
        LPR_LINUX_O_CLOEXEC |
        LPR_LINUX_O_NONBLOCK |
        LPR_LINUX_O_APPEND);
    if ((fd_flags & LPR_FD_TABLE_FD_CLOEXEC) != 0) {
        flags |= LPR_LINUX_O_CLOEXEC;
    }
    flags |= lpr_control_status_flags_to_linux(status_flags);
    return flags;
}

int lpr_control_install_fd(
    uint64_t fd,
    uint8_t kind,
    uint64_t linux_flags,
    uint64_t backend_id,
    uint64_t offset)
{
    if (fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EMFILE;
    }
    const int ensure_status = lpr_fd_table_ensure_fd(fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    uint16_t existing_flags = 0;
    if (lpr_fd_table_get_fd_flags(&lpr_control_fd_table, (uint32_t)fd, &existing_flags) == 0) {
        return 0;
    }
    const lpr_fd_table_install_t install = {
        .kind = kind,
        .fd_flags = lpr_control_fd_flags_from_linux(linux_flags),
        .status_flags = lpr_control_status_flags_from_linux(linux_flags),
        .rights = 0,
        .backend_id = backend_id,
        .offset = offset,
    };
    if (lpr_fd_table_install_at(&lpr_control_fd_table, (uint32_t)fd, &install) != 0) {
        return -LPR_LINUX_EMFILE;
    }
    lpr_fd_object_t *object = lpr_fd_object_for_fd(fd);
    if (object != 0) {
        switch (kind) {
        case LPR_FD_TABLE_KIND_FILED:
            object->payload.filed.flags = (uint32_t)linux_flags;
            object->payload.filed.handle = backend_id;
            object->payload.filed.offset = offset;
            break;
        case LPR_FD_TABLE_KIND_TTY:
            object->payload.tty.flags = (uint32_t)linux_flags;
            object->payload.tty.handle = backend_id;
            break;
        case LPR_FD_TABLE_KIND_DRM:
            object->payload.drm.flags = (uint32_t)linux_flags;
            object->payload.drm.handle = backend_id;
            break;
        case LPR_FD_TABLE_KIND_DMABUF:
            object->payload.dmabuf.flags = (uint32_t)linux_flags;
            object->payload.dmabuf.token = backend_id;
            object->payload.dmabuf.size = offset;
            object->payload.dmabuf.writable =
                (linux_flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDWR ? 1u : 0u;
            break;
        case LPR_FD_TABLE_KIND_PIPE:
            object->payload.pipe.flags = (uint32_t)linux_flags;
            break;
        case LPR_FD_TABLE_KIND_EVENT:
            object->payload.eventfd.flags = (uint32_t)linux_flags;
            object->payload.eventfd.counter = offset;
            break;
        case LPR_FD_TABLE_KIND_SOCKET:
            object->payload.socket.flags = (uint32_t)linux_flags;
            object->payload.socket.cloexec =
                (linux_flags & LPR_LINUX_O_CLOEXEC) != 0 ? 1u : 0u;
            object->payload.socket.handle = backend_id;
            break;
        case LPR_FD_TABLE_KIND_EPOLL:
            object->payload.epoll.flags = (uint32_t)linux_flags;
            object->payload.epoll.instance = backend_id;
            object->payload.epoll.map_bytes = offset;
            break;
        default:
            break;
        }
    }
    return 0;
}

void lpr_control_close_fd(uint64_t fd)
{
    if (fd < lpr_fd_table_capacity) {
        lpr_epoll_before_close(fd);
        (void)lpr_fd_table_close(&lpr_control_fd_table, (uint32_t)fd);
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

int lpr_control_ensure_from_legacy(uint64_t fd)
{
    if (fd >= lpr_fd_table_capacity) {
        return -LPR_LINUX_EBADF;
    }
    uint16_t fd_flags = 0;
    if (lpr_fd_table_get_fd_flags(&lpr_control_fd_table, (uint32_t)fd, &fd_flags) == 0) {
        return 0;
    }
    {
        struct pacha_fd_info info;
        if (lpr_native_pipe_fd_info(fd, &info)) {
            return lpr_control_install_fd(
                fd,
                LPR_FD_TABLE_KIND_PIPE,
                lpr_pipe_flags_from_info(&info),
                fd,
                0);
        }
    }
    return -LPR_LINUX_EBADF;
}

int lpr_control_dup_fd(uint64_t old_fd, uint64_t new_fd, uint64_t cloexec)
{
    const int ensure_status = lpr_control_ensure_from_legacy(old_fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    const uint16_t new_flags = cloexec ? LPR_FD_TABLE_FD_CLOEXEC : 0;
    return lpr_fd_table_dup2(&lpr_control_fd_table, (uint32_t)old_fd, (uint32_t)new_fd, new_flags) == 0 ?
        0 :
        -LPR_LINUX_EBADF;
}

void lpr_control_sync_legacy_flags(uint64_t fd)
{
    if (fd >= lpr_fd_table_capacity) {
        return;
    }
    uint32_t status_flags = 0;
    if (lpr_fd_table_get_status_flags(&lpr_control_fd_table, (uint32_t)fd, &status_flags) != 0)
    {
        return;
    }
    lpr_fd_object_t *object = lpr_fd_object_for_fd(fd);
    if (object == 0) {
        return;
    }
    switch (object->kind) {
    case LPR_FD_TABLE_KIND_FILED:
        object->payload.filed.flags =
            lpr_control_merge_legacy_flags(object->payload.filed.flags, 0, status_flags);
        break;
    case LPR_FD_TABLE_KIND_TTY:
        object->payload.tty.flags =
            lpr_control_merge_legacy_flags(object->payload.tty.flags, 0, status_flags);
        break;
    case LPR_FD_TABLE_KIND_DRM:
        object->payload.drm.flags =
            lpr_control_merge_legacy_flags(object->payload.drm.flags, 0, status_flags);
        break;
    case LPR_FD_TABLE_KIND_DMABUF:
        object->payload.dmabuf.flags =
            lpr_control_merge_legacy_flags(object->payload.dmabuf.flags, 0, status_flags);
        break;
    case LPR_FD_TABLE_KIND_EVENT:
        object->payload.eventfd.flags =
            lpr_control_merge_legacy_flags(object->payload.eventfd.flags, 0, status_flags);
        break;
    case LPR_FD_TABLE_KIND_PIPE:
        object->payload.pipe.flags =
            lpr_control_merge_legacy_flags(object->payload.pipe.flags, 0, status_flags);
        break;
    case LPR_FD_TABLE_KIND_SOCKET:
        object->payload.socket.flags =
            lpr_control_merge_legacy_flags(object->payload.socket.flags, 0, status_flags);
        break;
    case LPR_FD_TABLE_KIND_EPOLL:
        object->payload.epoll.flags =
            lpr_control_merge_legacy_flags(object->payload.epoll.flags, 0, status_flags);
        break;
    default:
        break;
    }
}

int lpr_control_set_fd_flags(uint64_t fd, uint64_t flags)
{
    const int ensure_status = lpr_control_ensure_from_legacy(fd);
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
    const int ensure_status = lpr_control_ensure_from_legacy(fd);
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
    lpr_control_sync_legacy_flags(fd);
    return 0;
}

int64_t lpr_control_get_fd_flags(uint64_t fd)
{
    const int ensure_status = lpr_control_ensure_from_legacy(fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    uint16_t fd_flags = 0;
    if (lpr_fd_table_get_fd_flags(&lpr_control_fd_table, (uint32_t)fd, &fd_flags) != 0) {
        return -LPR_LINUX_EBADF;
    }
    return (fd_flags & LPR_FD_TABLE_FD_CLOEXEC) != 0 ? LPR_LINUX_FD_CLOEXEC : 0;
}

int lpr_control_fd_cloexec(uint64_t fd, int *out_cloexec)
{
    if (out_cloexec == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const int ensure_status = lpr_control_ensure_from_legacy(fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    uint16_t fd_flags = 0;
    if (lpr_fd_table_get_fd_flags(&lpr_control_fd_table, (uint32_t)fd, &fd_flags) != 0) {
        return -LPR_LINUX_EBADF;
    }
    *out_cloexec = (fd_flags & LPR_FD_TABLE_FD_CLOEXEC) != 0;
    return 0;
}

int64_t lpr_control_get_status_flags(uint64_t fd, uint32_t access_mode)
{
    const int ensure_status = lpr_control_ensure_from_legacy(fd);
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
    if (lpr_control_ensure_from_legacy(fd) == 0 &&
        lpr_fd_table_get_offset(&lpr_control_fd_table, (uint32_t)fd, &offset) == 0)
    {
        return offset;
    }
    const lpr_filed_fd_t *filed = lpr_fd_filed_payload(fd);
    return filed != 0 ? filed->offset : 0;
}

void lpr_filed_control_set_offset(uint64_t fd, uint64_t offset)
{
    if (fd < lpr_fd_table_capacity) {
        lpr_filed_fd_t *filed = lpr_fd_filed_payload(fd);
        if (filed != 0) {
            filed->offset = offset;
        }
        if (lpr_control_ensure_from_legacy(fd) == 0) {
            (void)lpr_fd_table_set_offset(&lpr_control_fd_table, (uint32_t)fd, offset);
        }
    }
}

void lpr_filed_control_advance_offset(uint64_t fd, uint64_t old_offset, uint64_t amount)
{
    const uint64_t new_offset = old_offset + amount;
    lpr_filed_control_set_offset(fd, new_offset);
    if (new_offset < old_offset) {
        lpr_filed_fd_t *filed = lpr_fd_filed_payload(fd);
        if (filed != 0) {
            filed->offset_valid = 0;
        }
    }
}

int lpr_pipe_track_native_fd(uint64_t fd, const struct pacha_fd_info *info)
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
        LPR_FD_TABLE_KIND_PIPE,
        flags,
        fd,
        0);
    if (control_status != 0) {
        return control_status;
    }
    lpr_pipe_fd_t *pipe = lpr_fd_pipe_payload(fd);
    if (pipe == 0) {
        lpr_control_close_fd(fd);
        return -LPR_LINUX_EIO;
    }
    pipe->active = 1;
    pipe->readable = (info->rights & PACHA_FD_RIGHT_READ) != 0 ? 1u : 0u;
    pipe->writable = (info->rights & PACHA_FD_RIGHT_WRITE) != 0 ? 1u : 0u;
    pipe->flags = flags;
    return 0;
}

int lpr_fd_linux_visible_active(uint64_t fd)
{
    if (lpr_fd_local_active(fd) || lpr_linux_socket_fd_active(fd)) {
        return 1;
    }
    struct pacha_fd_info info;
    return lpr_native_fd_info(fd, &info) && info.kind == PACHA_FD_KIND_PIPE;
}

int lpr_fd_alloc(uint64_t handle, uint64_t flags)
{
    const int fd = lpr_fd_slot_alloc_from(3);
    if (fd < 0) {
        return fd;
    }
    const int control_status = lpr_control_install_fd(
        (uint64_t)fd,
        LPR_FD_TABLE_KIND_FILED,
        flags,
        handle,
        0);
    if (control_status != 0) {
        return control_status;
    }
    lpr_filed_fd_t *filed = lpr_fd_filed_payload((uint64_t)fd);
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
    struct pacha_fd_info native_info;
    return fd < lpr_fd_table_capacity &&
        !lpr_runtime_reserved_fd(fd) &&
        !lpr_control_fd_active(fd) &&
        !lpr_native_fd_info(fd, &native_info);
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

int lpr_bootstrap_fd_desc_valid_common(const lpr_bootstrap_fd_t *desc, uint64_t *out_fd)
{
    if (desc == 0 || out_fd == 0) {
        return 0;
    }
    const uint64_t fd = desc->fd;
    if (fd > LPR_LINUX_FD_MAX || lpr_runtime_reserved_fd(fd)) {
        return 0;
    }
    if (lpr_fd_table_ensure_fd(fd) != 0) {
        return 0;
    }
    *out_fd = fd;
    return 1;
}

int lpr_restore_bootstrap_filed_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd)
{
    if (desc->handle == 0 || !lpr_fd_slot_available(fd)) {
        return 0;
    }
    if (lpr_control_install_fd(
        fd,
        LPR_FD_TABLE_KIND_FILED,
        desc->flags,
        desc->handle,
        desc->offset_or_counter) != 0)
    {
        return 0;
    }
    lpr_filed_fd_t *filed = lpr_fd_filed_payload(fd);
    if (filed == 0) {
        lpr_control_close_fd(fd);
        return 0;
    }
    filed->active = 1;
    filed->offset_valid =
        ((desc->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY) ? 1u : 0u;
    filed->pread_active = 0;
    filed->flags = desc->flags;
    filed->handle = desc->handle;
    filed->offset = desc->offset_or_counter;
    return 1;
}

int lpr_restore_bootstrap_tty_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd)
{
    if (desc->handle == 0 || !lpr_fd_slot_available(fd)) {
        return 0;
    }
    for (uint64_t existing = 0; existing < lpr_fd_table_capacity; existing += 1) {
        if (existing == fd || !lpr_linux_tty_fd_active(existing)) {
            continue;
        }
        const lpr_tty_fd_t *existing_tty = lpr_fd_tty_payload(existing);
        if (existing_tty == 0 || existing_tty->handle != desc->handle) {
            continue;
        }
        if (lpr_control_dup_fd(
                existing,
                fd,
                (desc->flags & LPR_LINUX_O_CLOEXEC) != 0) != 0)
        {
            return 0;
        }
        (void)lpr_control_set_status_flags(fd, desc->flags);
        lpr_tty_fd_t *tty = lpr_fd_tty_payload(fd);
        if (tty != 0) {
            tty->flags = desc->flags;
            tty->handle = desc->handle;
        }
        return 1;
    }
    if (lpr_control_install_fd(
        fd,
        LPR_FD_TABLE_KIND_TTY,
        desc->flags,
        desc->handle,
        0) != 0)
    {
        return 0;
    }
    lpr_tty_fd_t *tty = lpr_fd_tty_payload(fd);
    if (tty == 0) {
        lpr_control_close_fd(fd);
        return 0;
    }
    tty->active = 1;
    tty->flags = desc->flags;
    tty->handle = desc->handle;
    return 1;
}

int lpr_restore_bootstrap_pipe_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd)
{
    struct pacha_fd_info info;
    if (!lpr_native_pipe_slot_claimable(fd, &info)) {
        return 0;
    }
    if (lpr_pipe_track_native_fd(fd, &info) != 0) {
        return 0;
    }
    const uint64_t pacha_flags =
        ((desc->flags & LPR_LINUX_O_CLOEXEC) != 0 ? PACHA_FD_FLAG_CLOEXEC : 0) |
        ((desc->flags & LPR_LINUX_O_NONBLOCK) != 0 ? PACHA_FD_FLAG_NONBLOCK : 0);
    (void)lpr_pacha_syscall3(
        PACHAOS_SYSCALL_FD_SET_FLAGS,
        fd,
        pacha_flags,
        PACHA_FD_FLAG_CLOEXEC | PACHA_FD_FLAG_NONBLOCK);
    (void)lpr_control_set_fd_flags(
        fd,
        (desc->flags & LPR_LINUX_O_CLOEXEC) != 0 ? LPR_LINUX_FD_CLOEXEC : 0);
    (void)lpr_control_set_status_flags(fd, desc->flags);
    return 1;
}

int lpr_restore_bootstrap_drm_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd)
{
    if (desc->handle == 0 || !lpr_fd_slot_available(fd)) {
        return 0;
    }
    if (lpr_control_install_fd(fd, LPR_FD_TABLE_KIND_DRM, desc->flags, desc->handle, 0) != 0) {
        return 0;
    }
    lpr_drm_fd_t *drm = lpr_fd_drm_payload(fd);
    if (drm == 0) {
        lpr_control_close_fd(fd);
        return 0;
    }
    drm->active = 1;
    drm->flags = desc->flags;
    drm->handle = desc->handle;
    return 1;
}

int lpr_restore_bootstrap_dmabuf_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd)
{
    struct pacha_fd_info info;
    if (desc->handle == 0 || lpr_runtime_reserved_fd(fd) || lpr_control_fd_active(fd) ||
        !lpr_native_fd_info(fd, &info) || info.kind != PACHA_FD_KIND_VMO ||
        info.size < desc->offset_or_counter) {
        return 0;
    }
    if (lpr_control_install_fd(
        fd, LPR_FD_TABLE_KIND_DMABUF, desc->flags, desc->handle,
        desc->offset_or_counter) != 0) {
        return 0;
    }
    lpr_dmabuf_fd_t *dmabuf = lpr_fd_dmabuf_payload(fd);
    if (dmabuf == 0) {
        lpr_control_close_fd(fd);
        return 0;
    }
    dmabuf->active = 1;
    dmabuf->writable = (desc->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDWR ? 1u : 0u;
    dmabuf->flags = desc->flags;
    dmabuf->token = desc->handle;
    dmabuf->size = desc->offset_or_counter;
    return 1;
}

int lpr_restore_bootstrap_event_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd)
{
    if (!lpr_fd_slot_available(fd)) {
        return 0;
    }
    if (lpr_control_install_fd(
        fd,
        LPR_FD_TABLE_KIND_EVENT,
        desc->flags,
        fd,
        desc->offset_or_counter) != 0)
    {
        return 0;
    }
    lpr_event_fd_t *event = lpr_fd_event_payload(fd);
    if (event == 0) {
        lpr_control_close_fd(fd);
        return 0;
    }
    event->active = 1;
    event->flags = desc->flags;
    event->counter = desc->offset_or_counter;
    return 1;
}

int lpr_restore_bootstrap_socket_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd)
{
    if (desc->handle == 0 || !lpr_fd_slot_available(fd)) {
        return 0;
    }
    if (lpr_control_install_fd(
        fd,
        LPR_FD_TABLE_KIND_SOCKET,
        desc->flags,
        desc->handle,
        0) != 0)
    {
        return 0;
    }
    lpr_socket_fd_t *socket = lpr_fd_socket_payload(fd);
    if (socket == 0) {
        lpr_control_close_fd(fd);
        return 0;
    }
    socket->active = 1;
    socket->flags = desc->flags;
    socket->handle = desc->handle;
    socket->cloexec = (desc->flags & LPR_LINUX_O_CLOEXEC) != 0 ? 1u : 0u;
    socket->type = (uint8_t)desc->offset_or_counter;
    socket->sndbuf = 256u * 1024u;
    socket->rcvbuf = 256u * 1024u;
    return 1;
}

int lpr_restore_bootstrap_fd_desc(const lpr_bootstrap_fd_t *desc)
{
    uint64_t fd = 0;
    if (!lpr_bootstrap_fd_desc_valid_common(desc, &fd)) {
        return 0;
    }
    switch (desc->kind) {
    case LPR_BOOTSTRAP_FD_FILED:
        return lpr_restore_bootstrap_filed_fd(desc, fd);
    case LPR_BOOTSTRAP_FD_TTY:
        return lpr_restore_bootstrap_tty_fd(desc, fd);
    case LPR_BOOTSTRAP_FD_DRM:
        return lpr_restore_bootstrap_drm_fd(desc, fd);
    case LPR_BOOTSTRAP_FD_DMABUF:
        return lpr_restore_bootstrap_dmabuf_fd(desc, fd);
    case LPR_BOOTSTRAP_FD_PIPE:
        return lpr_restore_bootstrap_pipe_fd(desc, fd);
    case LPR_BOOTSTRAP_FD_EVENT:
        return lpr_restore_bootstrap_event_fd(desc, fd);
    case LPR_BOOTSTRAP_FD_SOCKET:
        return lpr_restore_bootstrap_socket_fd(desc, fd);
    default:
        return 0;
    }
}

int lpr_install_local_fd_descs(const lpr_bootstrap_fd_t *descs, uint64_t count)
{
    if (count != 0 && descs == 0) {
        return 0;
    }
    for (uint64_t i = 0; i < count; ++i) {
        if (!lpr_restore_bootstrap_fd_desc(&descs[i])) {
            return 0;
        }
    }
    return 1;
}

int lpr_install_bootstrap_local_fds(const lpr_bootstrap_fd_t *descs, uint64_t count)
{
    if (lpr_bootstrap_local_fds_installed) {
        return 1;
    }
    lpr_bootstrap_local_fds_installed = 1;
    return lpr_install_local_fd_descs(descs, count);
}

void lpr_state_dump(const char *reason)
{
    lpr_fd_arrays_init();
    lpr_fd_table_lock(&lpr_control_fd_table);
    const uint64_t reason_id = reason != 0 ? pacha_trace_name_id(reason) : 0;
    uint64_t open_count = 0;
    uint64_t live_object_count = 0;
    for (uint64_t fd = 0; fd < lpr_fd_table_capacity; fd += 1) {
        open_count += lpr_control_fd_table.slots[fd].active ? 1u : 0u;
        live_object_count += lpr_control_fd_table.files[fd].active ? 1u : 0u;
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
        const lpr_fd_table_slot_t *slot = &lpr_control_fd_table.slots[fd];
        if (!slot->active || slot->file_index >= lpr_control_fd_table.file_count) {
            continue;
        }
        const lpr_fd_object_t *object = &lpr_control_fd_table.files[slot->file_index];
        if (!object->active) {
            continue;
        }
        crc ^= (fd << 32u) ^ object->kind ^ object->refcount ^ object->backend_id;
        pacha_trace6(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_LPR_PROCESS,
            PACHA_TRACE_CLASS_ERROR,
            pacha_trace_name_id("lpr.fd.entry"),
            fd,
            object->kind,
            slot->fd_flags,
            object->status_flags,
            object->refcount);
        switch (object->kind) {
        case LPR_FD_TABLE_KIND_FILED:
            pacha_trace6(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_PROCESS,
                PACHA_TRACE_CLASS_ERROR,
                pacha_trace_name_id("lpr.fd.filed"),
                fd,
                object->payload.filed.handle,
                object->payload.filed.offset,
                object->payload.filed.offset_valid,
                lpr_page_cache_clock);
            break;
        case LPR_FD_TABLE_KIND_PIPE:
            pacha_trace6(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_PROCESS,
                PACHA_TRACE_CLASS_ERROR,
                pacha_trace_name_id("lpr.fd.pipe"),
                fd,
                fd,
                object->payload.pipe.readable,
                object->payload.pipe.writable,
                object->payload.pipe.last_wait_events);
            break;
        case LPR_FD_TABLE_KIND_EVENT:
            pacha_trace3(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_PROCESS,
                PACHA_TRACE_CLASS_ERROR,
                pacha_trace_name_id("lpr.fd.eventfd"),
                fd,
                object->payload.eventfd.counter);
            break;
        case LPR_FD_TABLE_KIND_TTY:
            pacha_trace4(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_PROCESS,
                PACHA_TRACE_CLASS_ERROR,
                pacha_trace_name_id("lpr.fd.tty"),
                fd,
                object->payload.tty.handle,
                (uint64_t)(uint32_t)lpr_linux_current_pgrp);
            break;
        case LPR_FD_TABLE_KIND_DRM:
            pacha_trace4(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_PROCESS,
                PACHA_TRACE_CLASS_ERROR,
                pacha_trace_name_id("lpr.fd.drm"),
                fd,
                object->payload.drm.handle,
                object->payload.drm.flags);
            break;
        case LPR_FD_TABLE_KIND_SOCKET:
            pacha_trace6(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_PROCESS,
                PACHA_TRACE_CLASS_ERROR,
                pacha_trace_name_id("lpr.fd.socket"),
                fd,
                object->payload.socket.handle,
                object->payload.socket.connected,
                object->payload.socket.connecting,
                (uint64_t)(uint32_t)object->payload.socket.last_error);
            break;
        case LPR_FD_TABLE_KIND_EPOLL:
            pacha_trace4(
                PACHA_TRACE_COMPONENT_LPR,
                PACHA_TRACE_EVENT_LPR_PROCESS,
                PACHA_TRACE_CLASS_ERROR,
                pacha_trace_name_id("lpr.fd.epoll"),
                fd,
                object->payload.epoll.instance,
                object->payload.epoll.map_bytes);
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

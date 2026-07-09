static int lpr_fd_is_filed(uint64_t fd)
{
    lpr_fd_arrays_init();
    return fd < lpr_fd_table_capacity && lpr_fds[fd].active != 0;
}

int lpr_linux_tty_fd_active(uint64_t fd)
{
    lpr_fd_arrays_init();
    return fd < lpr_fd_table_capacity && lpr_tty_fds[fd].active != 0;
}

static int lpr_fd_shadow_offset_eligible(uint64_t fd)
{
    return lpr_fd_is_filed(fd) &&
        (lpr_fds[fd].flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY;
}

int lpr_linux_filed_fd_active(uint64_t fd)
{
    return lpr_fd_is_filed(fd);
}

uint64_t lpr_linux_filed_fd_handle(uint64_t fd)
{
    return lpr_fd_is_filed(fd) ? lpr_fds[fd].handle : 0;
}

static int lpr_runtime_reserved_fd(uint64_t fd)
{
    return fd == LPR_FILED_ENDPOINT_FD ||
        fd == LPR_NETD_ENDPOINT_FD ||
        fd == LPR_TERMD_TTY_ENDPOINT_FD ||
        fd == LPR_BOOTSTRAP_FD ||
        fd == LPR_SUPERVISOR_ENDPOINT_FD;
}

static int lpr_native_fd_info(uint64_t fd, struct pacha_fd_info *out)
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

static int64_t lpr_close_native_fd_if_open(uint64_t fd)
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

static int lpr_fd_local_active(uint64_t fd)
{
    return lpr_fd_is_filed(fd) ||
        lpr_pipe_fd_is_active(fd) ||
        lpr_linux_eventfd_active(fd) ||
        lpr_linux_tty_fd_active(fd);
}

static uint32_t lpr_pipe_flags_from_info(const struct pacha_fd_info *info)
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

static uint16_t lpr_control_fd_flags_from_linux(uint64_t flags)
{
    return (flags & LPR_LINUX_O_CLOEXEC) != 0 ? LPR_FD_TABLE_FD_CLOEXEC : 0;
}

static uint16_t lpr_control_fd_flags_from_fcntl(uint64_t flags)
{
    return (flags & LPR_LINUX_FD_CLOEXEC) != 0 ? LPR_FD_TABLE_FD_CLOEXEC : 0;
}

static uint32_t lpr_control_status_flags_from_linux(uint64_t flags)
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

static uint32_t lpr_control_status_flags_to_linux(uint32_t status)
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

static uint32_t lpr_control_merge_legacy_flags(
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

static int lpr_control_install_fd(
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
    return lpr_fd_table_install_at(&lpr_control_fd_table, (uint32_t)fd, &install) == 0 ?
        0 :
        -LPR_LINUX_EMFILE;
}

static void lpr_control_close_fd(uint64_t fd)
{
    if (fd < lpr_fd_table_capacity) {
        (void)lpr_fd_table_close(&lpr_control_fd_table, (uint32_t)fd);
    }
}

static int lpr_control_fd_active(uint64_t fd)
{
    if (fd >= lpr_fd_table_capacity) {
        return 0;
    }
    uint16_t fd_flags = 0;
    return lpr_fd_table_get_fd_flags(&lpr_control_fd_table, (uint32_t)fd, &fd_flags) == 0;
}

static int lpr_control_ensure_from_legacy(uint64_t fd)
{
    if (fd >= lpr_fd_table_capacity) {
        return -LPR_LINUX_EBADF;
    }
    uint16_t fd_flags = 0;
    if (lpr_fd_table_get_fd_flags(&lpr_control_fd_table, (uint32_t)fd, &fd_flags) == 0) {
        return 0;
    }
    if (lpr_fds[fd].active) {
        return lpr_control_install_fd(
            fd,
            LPR_FD_TABLE_KIND_FILED,
            lpr_fds[fd].flags,
            lpr_fds[fd].handle,
            lpr_fds[fd].offset);
    }
    if (lpr_tty_fds[fd].active) {
        return lpr_control_install_fd(
            fd,
            LPR_FD_TABLE_KIND_TTY,
            lpr_tty_fds[fd].flags,
            lpr_tty_fds[fd].handle,
            0);
    }
    if (lpr_event_fds[fd].active) {
        return lpr_control_install_fd(
            fd,
            LPR_FD_TABLE_KIND_EVENT,
            lpr_event_fds[fd].flags,
            fd,
            lpr_event_fds[fd].counter);
    }
    if (lpr_pipe_fds[fd].active) {
        return lpr_control_install_fd(
            fd,
            LPR_FD_TABLE_KIND_PIPE,
            lpr_pipe_fds[fd].flags,
            fd,
            0);
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

static int lpr_control_dup_fd(uint64_t old_fd, uint64_t new_fd, uint64_t cloexec)
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

static void lpr_control_sync_legacy_flags(uint64_t fd)
{
    if (fd >= lpr_fd_table_capacity) {
        return;
    }
    uint16_t fd_flags = 0;
    uint32_t status_flags = 0;
    if (lpr_fd_table_get_fd_flags(&lpr_control_fd_table, (uint32_t)fd, &fd_flags) != 0 ||
        lpr_fd_table_get_status_flags(&lpr_control_fd_table, (uint32_t)fd, &status_flags) != 0)
    {
        return;
    }
    if (lpr_fds[fd].active) {
        lpr_fds[fd].flags = lpr_control_merge_legacy_flags(lpr_fds[fd].flags, fd_flags, status_flags);
    } else if (lpr_tty_fds[fd].active) {
        lpr_tty_fds[fd].flags = lpr_control_merge_legacy_flags(lpr_tty_fds[fd].flags, fd_flags, status_flags);
    } else if (lpr_event_fds[fd].active) {
        lpr_event_fds[fd].flags = lpr_control_merge_legacy_flags(lpr_event_fds[fd].flags, fd_flags, status_flags);
    } else if (lpr_pipe_fds[fd].active) {
        lpr_pipe_fds[fd].flags = lpr_control_merge_legacy_flags(lpr_pipe_fds[fd].flags, fd_flags, status_flags);
    }
}

static int lpr_control_set_fd_flags(uint64_t fd, uint64_t flags)
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
    lpr_control_sync_legacy_flags(fd);
    return 0;
}

static int lpr_control_set_status_flags(uint64_t fd, uint64_t flags)
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

static int64_t lpr_control_get_fd_flags(uint64_t fd)
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

static int lpr_control_fd_cloexec(uint64_t fd, int *out_cloexec)
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

static int64_t lpr_control_get_status_flags(uint64_t fd, uint32_t access_mode)
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

static uint64_t lpr_filed_control_offset(uint64_t fd)
{
    uint64_t offset = 0;
    if (lpr_control_ensure_from_legacy(fd) == 0 &&
        lpr_fd_table_get_offset(&lpr_control_fd_table, (uint32_t)fd, &offset) == 0)
    {
        return offset;
    }
    return lpr_fds[fd].offset;
}

static uint64_t lpr_control_fd_flags_to_lprs(uint64_t fd)
{
    uint16_t fd_flags = 0;
    if (fd >= lpr_fd_table_capacity ||
        lpr_fd_table_get_fd_flags(&lpr_control_fd_table, (uint32_t)fd, &fd_flags) != 0)
    {
        return 0;
    }
    return (fd_flags & LPR_FD_TABLE_FD_CLOEXEC) != 0 ? LPRS_V2_FD_CLOEXEC : 0;
}

static uint64_t lpr_control_status_flags_to_lprs(uint64_t fd)
{
    uint32_t status_flags = 0;
    if (fd >= lpr_fd_table_capacity ||
        lpr_fd_table_get_status_flags(&lpr_control_fd_table, (uint32_t)fd, &status_flags) != 0)
    {
        return 0;
    }
    uint64_t out = 0;
    if ((status_flags & LPR_FD_TABLE_STATUS_NONBLOCK) != 0) {
        out |= LPRS_V2_FILE_NONBLOCK;
    }
    if ((status_flags & LPR_FD_TABLE_STATUS_APPEND) != 0) {
        out |= LPRS_V2_FILE_APPEND;
    }
    return out;
}

static uint64_t lpr_lprs_fd_flags_to_linux(uint64_t fd_flags)
{
    return (fd_flags & LPRS_V2_FD_CLOEXEC) != 0 ? LPR_LINUX_O_CLOEXEC : 0;
}

static uint64_t lpr_lprs_status_flags_to_linux(uint64_t status_flags)
{
    uint64_t out = 0;
    if ((status_flags & LPRS_V2_FILE_NONBLOCK) != 0) {
        out |= LPR_LINUX_O_NONBLOCK;
    }
    if ((status_flags & LPRS_V2_FILE_APPEND) != 0) {
        out |= LPR_LINUX_O_APPEND;
    }
    return out;
}

static void lpr_filed_control_set_offset(uint64_t fd, uint64_t offset)
{
    if (fd < lpr_fd_table_capacity) {
        lpr_fds[fd].offset = offset;
        if (lpr_control_ensure_from_legacy(fd) == 0) {
            (void)lpr_fd_table_set_offset(&lpr_control_fd_table, (uint32_t)fd, offset);
        }
    }
}

static void lpr_filed_control_advance_offset(uint64_t fd, uint64_t old_offset, uint64_t amount)
{
    const uint64_t new_offset = old_offset + amount;
    lpr_filed_control_set_offset(fd, new_offset);
    if (new_offset < old_offset) {
        lpr_fds[fd].offset_valid = 0;
    }
}

static int lpr_pipe_track_native_fd(uint64_t fd, const struct pacha_fd_info *info)
{
    if (fd > LPR_LINUX_FD_MAX || info == 0) {
        return -LPR_LINUX_EMFILE;
    }
    const int ensure_status = lpr_fd_table_ensure_fd(fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    lpr_memset(&lpr_pipe_fds[fd], 0, sizeof(lpr_pipe_fds[fd]));
    lpr_pipe_fds[fd].active = 1;
    lpr_pipe_fds[fd].readable = (info->rights & PACHA_FD_RIGHT_READ) != 0 ? 1u : 0u;
    lpr_pipe_fds[fd].writable = (info->rights & PACHA_FD_RIGHT_WRITE) != 0 ? 1u : 0u;
    lpr_pipe_fds[fd].flags = lpr_pipe_flags_from_info(info);
    const int control_status = lpr_control_install_fd(
        fd,
        LPR_FD_TABLE_KIND_PIPE,
        lpr_pipe_fds[fd].flags,
        fd,
        0);
    if (control_status != 0) {
        lpr_memset(&lpr_pipe_fds[fd], 0, sizeof(lpr_pipe_fds[fd]));
        return control_status;
    }
    return 0;
}

static int lpr_fd_linux_visible_active(uint64_t fd)
{
    if (lpr_fd_local_active(fd) || lpr_linux_socket_fd_active(fd)) {
        return 1;
    }
    struct pacha_fd_info info;
    return lpr_native_fd_info(fd, &info) && info.kind == PACHA_FD_KIND_PIPE;
}

static int lpr_fd_alloc(uint64_t handle, uint64_t flags)
{
    const int fd = lpr_fd_slot_alloc_from(3);
    if (fd < 0) {
        return fd;
    }
    lpr_fds[(uint64_t)fd].active = 1;
    lpr_fds[(uint64_t)fd].offset_valid =
        ((flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY) ? 1u : 0u;
    lpr_fds[(uint64_t)fd].pread_active = 0;
    lpr_fds[(uint64_t)fd].flags = (uint32_t)flags;
    lpr_fds[(uint64_t)fd].handle = handle;
    lpr_fds[(uint64_t)fd].offset = 0;
    const int control_status = lpr_control_install_fd(
        (uint64_t)fd,
        LPR_FD_TABLE_KIND_FILED,
        flags,
        handle,
        0);
    if (control_status != 0) {
        lpr_memset(&lpr_fds[(uint64_t)fd], 0, sizeof(lpr_fds[(uint64_t)fd]));
        return control_status;
    }
    return fd;
}

static int lpr_fd_slot_alloc(void)
{
    return lpr_fd_slot_alloc_from(3);
}

static int lpr_fd_slot_available(uint64_t fd)
{
    lpr_fd_arrays_init();
    struct pacha_fd_info native_info;
    return fd < lpr_fd_table_capacity &&
        !lpr_runtime_reserved_fd(fd) &&
        !lpr_fd_local_active(fd) &&
        !lpr_control_fd_active(fd) &&
        !lpr_linux_socket_fd_active(fd) &&
        !lpr_native_fd_info(fd, &native_info);
}

static int lpr_fd_slot_alloc_from(uint64_t min_fd)
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

static int lpr_bootstrap_fd_desc_valid_common(const lpr_bootstrap_fd_t *desc, uint64_t *out_fd)
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

static int lpr_restore_bootstrap_filed_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd)
{
    if (desc->handle == 0 || !lpr_fd_slot_available(fd)) {
        return 0;
    }
    lpr_fds[fd].active = 1;
    lpr_fds[fd].offset_valid =
        ((desc->flags & LPR_LINUX_O_ACCMODE) == LPR_LINUX_O_RDONLY) ? 1u : 0u;
    lpr_fds[fd].pread_active = 0;
    lpr_fds[fd].flags = desc->flags;
    lpr_fds[fd].handle = desc->handle;
    lpr_fds[fd].offset = desc->offset_or_counter;
    return lpr_control_install_fd(
        fd,
        LPR_FD_TABLE_KIND_FILED,
        desc->flags,
        desc->handle,
        desc->offset_or_counter) == 0;
}

static int lpr_restore_bootstrap_tty_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd)
{
    if (desc->handle == 0 || !lpr_fd_slot_available(fd)) {
        return 0;
    }
    lpr_tty_fds[fd].active = 1;
    lpr_tty_fds[fd].flags = desc->flags;
    lpr_tty_fds[fd].handle = desc->handle;
    return lpr_control_install_fd(
        fd,
        LPR_FD_TABLE_KIND_TTY,
        desc->flags,
        desc->handle,
        0) == 0;
}

static int lpr_restore_bootstrap_pipe_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd)
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

static int lpr_restore_bootstrap_event_fd(const lpr_bootstrap_fd_t *desc, uint64_t fd)
{
    if (!lpr_fd_slot_available(fd)) {
        return 0;
    }
    lpr_event_fds[fd].active = 1;
    lpr_event_fds[fd].flags = desc->flags;
    lpr_event_fds[fd].counter = desc->offset_or_counter;
    return lpr_control_install_fd(
        fd,
        LPR_FD_TABLE_KIND_EVENT,
        desc->flags,
        fd,
        desc->offset_or_counter) == 0;
}

static int lpr_restore_bootstrap_fd_desc(const lpr_bootstrap_fd_t *desc)
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
    case LPR_BOOTSTRAP_FD_PIPE:
        return lpr_restore_bootstrap_pipe_fd(desc, fd);
    case LPR_BOOTSTRAP_FD_EVENT:
        return lpr_restore_bootstrap_event_fd(desc, fd);
    default:
        return 0;
    }
}

static int lpr_install_local_fd_descs(const lpr_bootstrap_fd_t *descs, uint64_t count)
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

static int lpr_install_bootstrap_local_fds(const lpr_bootstrap_fd_t *descs, uint64_t count)
{
    if (lpr_bootstrap_local_fds_installed) {
        return 1;
    }
    lpr_bootstrap_local_fds_installed = 1;
    return lpr_install_local_fd_descs(descs, count);
}


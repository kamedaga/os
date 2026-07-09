static void lpr_pipe_close_fd(uint64_t fd)
{
    if (lpr_pipe_fd_is_active(fd)) {
        const uint64_t mode =
            (lpr_pipe_fds[fd].readable ? 1u : 0u) |
            (lpr_pipe_fds[fd].writable ? 2u : 0u) |
            ((uint64_t)lpr_pipe_fds[fd].flags << 8u);
        const int64_t status = lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, fd);
        lpr_trace_process_event("pipe_close", fd, mode, status);
    }
    lpr_memset(&lpr_pipe_fds[fd], 0, sizeof(lpr_pipe_fds[fd]));
    lpr_control_close_fd(fd);
}

static void lpr_pipe_after_fork_child(void)
{
    /* Kernel fd-table clone owns native pipe endpoint refcounts. */
    lpr_cwd_init();
    if (lpr_cwd_handle != 0) {
        uint64_t dup_handle = 0;
        const int64_t status = lpr_filed_dup_handle(lpr_cwd_handle, 0, &dup_handle);
        lpr_trace_process_event("fork_dup_cwd", lpr_cwd_handle, dup_handle, status);
        if (status == 0 && dup_handle != 0) {
            lpr_cwd_handle = dup_handle;
        }
    }
    lpr_fd_arrays_init();
    for (uint64_t fd = 0; fd < lpr_fd_table_capacity; fd += 1) {
        if (lpr_linux_tty_fd_active(fd)) {
            uint64_t dup_handle = 0;
            const int64_t status = lpr_termd_call_handle(
                TERMD_V2_OP_HANDLE_DUP,
                lpr_tty_fds[fd].handle,
                &dup_handle);
            lpr_trace_process_event("fork_dup_tty", fd, dup_handle, status);
            if (status == 0 && dup_handle != 0) {
                lpr_tty_fds[fd].handle = dup_handle;
            }
        }
        if (lpr_fd_is_filed(fd)) {
            void *page = 0;
            const int page_fd = lpr_create_wire_page(&page);
            if (page_fd < 0) {
                lpr_trace_process_event("fork_dup_filed_page", fd, 0, page_fd);
                continue;
            }
            filed_v2_handle_flags_t *flags = (filed_v2_handle_flags_t *)page;
            lpr_memset(flags, 0, sizeof(*flags));
            flags->handle = lpr_fds[fd].handle;
            flags->fd_flags =
                (lpr_fds[fd].flags & LPR_LINUX_O_CLOEXEC) != 0 ? FILED_V2_FD_CLOEXEC : 0;
            uint64_t dup_handle = 0;
            const int64_t status = lpr_filed_call(FILED_V2_OP_VFS_DUP, page_fd, 0, &dup_handle);
            lpr_destroy_wire_page(page_fd, page);
            lpr_trace_process_event("fork_dup_filed", fd, dup_handle, status);
            if (status == 0 && dup_handle != 0) {
                lpr_fds[fd].handle = dup_handle;
            }
        }
    }
}

void lpr_linux_apply_pending_fork_child(void)
{
    if (lpr_linux_pending_child_pid <= 0) {
        return;
    }
    const int32_t child_pid = lpr_linux_pending_child_pid;
    const int32_t child_ppid = lpr_linux_pending_child_ppid;
    const int32_t child_sid = lpr_linux_pending_child_sid;
    const int32_t child_pgrp = lpr_linux_pending_child_pgrp;
    const uint64_t child_token = lpr_supervisor_pending_child_token;
    lpr_linux_process_state_checked = 1;
    if (child_token != 0) {
        lpr_supervisor_token = child_token;
        lpr_supervisor_enabled = 1;
        (void)lpr_supervisor_call_token(
            LPRS_V2_OP_PROCESS_FORK_CHILD_READY,
            lpr_supervisor_token,
            -1,
            0);
    }
    lpr_linux_current_pid = child_pid;
    lpr_linux_current_ppid = child_ppid;
    lpr_linux_current_sid = child_sid > 0 ? child_sid : child_pid;
    lpr_linux_current_pgrp = child_pgrp > 0 ? child_pgrp : child_pid;
    lpr_linux_pending_child_pid = 0;
    lpr_linux_pending_child_ppid = 0;
    lpr_linux_pending_child_sid = 0;
    lpr_linux_pending_child_pgrp = 0;
    lpr_supervisor_pending_child_token = 0;
    lpr_linux_process_clear_children();
    lpr_pipe_after_fork_child();
    lpr_trace_process_event("fork_child_state", (uint64_t)(uint32_t)child_pid, (uint64_t)(uint32_t)child_ppid, 0);
}

int64_t lpr_linux_pipe2(uint64_t fds_raw, uint64_t flags)
{
    lpr_trace_process_event("pipe2_begin", fds_raw, flags, 0);
    const uint64_t known_flags = LPR_LINUX_O_CLOEXEC | LPR_LINUX_O_NONBLOCK;
    if (fds_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t pair[2] = { 0, 0 };
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_PIPE_CREATE,
        (uint64_t)(uintptr_t)pair,
        lpr_pipe_flags_to_pacha(flags));
    if (status != 0) {
        return lpr_pacha_status_to_errno(status);
    }
    const uint64_t read_fd = pair[0];
    const uint64_t write_fd = pair[1];
    struct pacha_fd_info read_info;
    struct pacha_fd_info write_info;
    if (!lpr_native_pipe_slot_claimable(read_fd, &read_info) ||
        !lpr_native_pipe_slot_claimable(write_fd, &write_info))
    {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, read_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, write_fd);
        return -LPR_LINUX_EMFILE;
    }
    const int read_track = lpr_pipe_track_native_fd(read_fd, &read_info);
    const int write_track = read_track == 0 ? lpr_pipe_track_native_fd(write_fd, &write_info) : read_track;
    if (write_track != 0) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, read_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, write_fd);
        if (read_fd < lpr_fd_table_capacity) {
            lpr_memset(&lpr_pipe_fds[read_fd], 0, sizeof(lpr_pipe_fds[read_fd]));
        }
        if (write_fd < lpr_fd_table_capacity) {
            lpr_memset(&lpr_pipe_fds[write_fd], 0, sizeof(lpr_pipe_fds[write_fd]));
        }
        return write_track;
    }

    int *fds = (int *)(uintptr_t)fds_raw;
    fds[0] = (int)(uint32_t)read_fd;
    fds[1] = (int)(uint32_t)write_fd;
    lpr_trace_process_event("pipe2_end", read_fd, write_fd, 0);
    return 0;
}

int64_t lpr_linux_eventfd2(uint64_t initval, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_O_CLOEXEC | LPR_LINUX_O_NONBLOCK;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const int fd = lpr_fd_slot_alloc();
    if (fd < 0) {
        return fd;
    }
    lpr_event_fds[fd].active = 1;
    lpr_event_fds[fd].flags = (uint32_t)flags;
    lpr_event_fds[fd].counter = initval;
    const int control_status = lpr_control_install_fd(
        (uint64_t)fd,
        LPR_FD_TABLE_KIND_EVENT,
        flags,
        (uint64_t)fd,
        initval);
    if (control_status != 0) {
        lpr_memset(&lpr_event_fds[fd], 0, sizeof(lpr_event_fds[fd]));
        return control_status;
    }
    return fd;
}

static int64_t lpr_linux_dup_into(uint64_t fd, int target_fd, uint64_t min_fd, uint64_t cloexec)
{
    int dup_fd = target_fd;
    lpr_trace_process_event(
        "dup_into_begin",
        fd,
        target_fd >= 0 ? (uint64_t)(uint32_t)target_fd : min_fd,
        target_fd >= 0 ? 1 : 0);
    if (dup_fd < 0) {
        dup_fd = lpr_fd_slot_alloc_from(min_fd);
        if (dup_fd < 0) {
            lpr_trace_process_event("dup_into_alloc_error", fd, min_fd, dup_fd);
            return dup_fd;
        }
    } else {
        if ((uint64_t)(uint32_t)dup_fd > LPR_LINUX_FD_MAX) {
            return -LPR_LINUX_EINVAL;
        }
        const int ensure_status = lpr_fd_table_ensure_fd((uint64_t)(uint32_t)dup_fd);
        if (ensure_status != 0) {
            return ensure_status;
        }
        if (!lpr_fd_slot_available((uint64_t)(uint32_t)dup_fd)) {
            lpr_trace_process_event("dup_into_target_busy", fd, (uint64_t)(uint32_t)dup_fd, -LPR_LINUX_EBADF);
            return -LPR_LINUX_EBADF;
        }
    }

    if (lpr_linux_eventfd_active(fd)) {
        const int control_status = lpr_control_dup_fd(fd, (uint64_t)(uint32_t)dup_fd, cloexec);
        if (control_status != 0) {
            return control_status;
        }
        lpr_event_fds[dup_fd] = lpr_event_fds[fd];
        if (cloexec) {
            lpr_event_fds[dup_fd].flags |= LPR_LINUX_O_CLOEXEC;
        } else {
            lpr_event_fds[dup_fd].flags &= ~LPR_LINUX_O_CLOEXEC;
        }
        return dup_fd;
    }
    if (lpr_pipe_fd_is_active(fd)) {
        uint64_t dup_flags = lpr_pipe_flags_to_pacha(lpr_pipe_fds[fd].flags & LPR_LINUX_O_NONBLOCK);
        if (cloexec) {
            dup_flags |= PACHA_FD_FLAG_CLOEXEC;
        }
        const uint64_t rights = lpr_pipe_rights(lpr_pipe_fds[fd].readable != 0);
        const int64_t native_dup = lpr_pacha_syscall4(
            PACHAOS_SYSCALL_FD_DUP,
            fd,
            (uint64_t)(uint32_t)dup_fd,
            rights,
            dup_flags);
        lpr_trace_process_event("dup_into_pipe_result", fd, (uint64_t)(uint32_t)dup_fd, native_dup);
        if (native_dup < 0) {
            return lpr_pacha_status_to_errno(native_dup);
        }
        const uint64_t native_dup_fd = (uint64_t)native_dup;
        if (native_dup_fd > LPR_LINUX_FD_MAX ||
            (target_fd >= 0 && native_dup_fd != (uint64_t)(uint32_t)dup_fd))
        {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, native_dup_fd);
            return target_fd >= 0 ? -LPR_LINUX_EINVAL : -LPR_LINUX_EMFILE;
        }
        struct pacha_fd_info dup_info;
        if (!lpr_native_pipe_slot_claimable(native_dup_fd, &dup_info)) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, native_dup_fd);
            return -LPR_LINUX_EMFILE;
        }
        const int track_status = lpr_pipe_track_native_fd(native_dup_fd, &dup_info);
        if (track_status != 0) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, native_dup_fd);
            return track_status;
        }
        const int control_status = lpr_control_dup_fd(fd, native_dup_fd, cloexec);
        if (control_status != 0) {
            lpr_pipe_close_fd(native_dup_fd);
            return control_status;
        }
        lpr_control_sync_legacy_flags(native_dup_fd);
        return (int64_t)native_dup_fd;
    }
    if (lpr_linux_tty_fd_active(fd)) {
        uint64_t dup_handle = 0;
        const int64_t status = lpr_termd_call_handle(
            TERMD_V2_OP_HANDLE_DUP,
            lpr_tty_fds[fd].handle,
            &dup_handle);
        if (status != 0) {
            return status;
        }
        const int control_status = lpr_control_dup_fd(fd, (uint64_t)(uint32_t)dup_fd, cloexec);
        if (control_status != 0) {
            (void)lpr_termd_call_handle(TERMD_V2_OP_HANDLE_CLOSE, dup_handle, 0);
            return control_status;
        }
        lpr_tty_fds[dup_fd].active = 1;
        lpr_tty_fds[dup_fd].flags =
            (lpr_tty_fds[fd].flags & ~LPR_LINUX_O_CLOEXEC) |
            (cloexec ? LPR_LINUX_O_CLOEXEC : 0);
        lpr_tty_fds[dup_fd].handle = dup_handle;
        return dup_fd;
    }
    if (lpr_fd_is_filed(fd)) {
        void *page = 0;
        const int page_fd = lpr_create_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        filed_v2_handle_flags_t *flags = (filed_v2_handle_flags_t *)page;
        lpr_memset(flags, 0, sizeof(*flags));
        flags->handle = lpr_fds[fd].handle;
        flags->fd_flags = cloexec ? FILED_V2_FD_CLOEXEC : 0;
        uint64_t dup_handle = 0;
        const int64_t status = lpr_filed_call(FILED_V2_OP_VFS_DUP, page_fd, 0, &dup_handle);
        lpr_destroy_wire_page(page_fd, page);
        if (status != 0) {
            return status;
        }
        const int control_status = lpr_control_dup_fd(fd, (uint64_t)(uint32_t)dup_fd, cloexec);
        if (control_status != 0) {
            (void)lpr_filed_close_handle(dup_handle);
            return control_status;
        }
        lpr_fds[dup_fd].active = 1;
        lpr_fds[dup_fd].offset_valid = lpr_fds[fd].offset_valid;
        lpr_fds[dup_fd].pread_active = lpr_fds[fd].pread_active;
        lpr_fds[dup_fd].flags =
            (lpr_fds[fd].flags & ~LPR_LINUX_O_CLOEXEC) |
            (cloexec ? LPR_LINUX_O_CLOEXEC : 0);
        lpr_fds[dup_fd].handle = dup_handle;
        lpr_fds[dup_fd].offset = lpr_filed_control_offset(fd);
        lpr_control_sync_legacy_flags((uint64_t)(uint32_t)dup_fd);
        return dup_fd;
    }
    {
        struct pacha_fd_info info;
        if (lpr_native_pipe_fd_info(fd, &info)) {
            uint64_t dup_flags = info.flags & PACHA_FD_FLAG_NONBLOCK;
            if (cloexec) {
                dup_flags |= PACHA_FD_FLAG_CLOEXEC;
            }
            const int64_t native_dup = lpr_pacha_syscall4(
                PACHAOS_SYSCALL_FD_DUP,
                fd,
                (uint64_t)(uint32_t)dup_fd,
                info.rights,
                dup_flags);
            lpr_trace_process_event("dup_into_native_pipe_result", fd, (uint64_t)(uint32_t)dup_fd, native_dup);
            if (native_dup < 0) {
                return lpr_pacha_status_to_errno(native_dup);
            }
            const uint64_t native_dup_fd = (uint64_t)native_dup;
            if (native_dup_fd > LPR_LINUX_FD_MAX ||
                (target_fd >= 0 && native_dup_fd != (uint64_t)(uint32_t)dup_fd))
            {
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, native_dup_fd);
                return target_fd >= 0 ? -LPR_LINUX_EINVAL : -LPR_LINUX_EMFILE;
            }
            struct pacha_fd_info dup_info;
            if (!lpr_native_pipe_slot_claimable(native_dup_fd, &dup_info)) {
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, native_dup_fd);
                return -LPR_LINUX_EMFILE;
            }
            const int track_status = lpr_pipe_track_native_fd(native_dup_fd, &dup_info);
            if (track_status != 0) {
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, native_dup_fd);
                return track_status;
            }
            const int control_status = lpr_control_dup_fd(fd, native_dup_fd, cloexec);
            if (control_status != 0) {
                lpr_pipe_close_fd(native_dup_fd);
                return control_status;
            }
            lpr_control_sync_legacy_flags(native_dup_fd);
            return (int64_t)native_dup_fd;
        }
    }
    if (target_fd >= 0) {
        return -LPR_LINUX_EBADF;
    }
    return lpr_pacha_syscall4(PACHAOS_SYSCALL_FD_FCNTL, fd, PACHA_FD_FCNTL_DUP, 0, 0);
}

int64_t lpr_linux_dup(uint64_t fd, uint64_t min_fd, uint64_t cloexec)
{
    return lpr_linux_dup_into(fd, -1, min_fd, cloexec);
}

int64_t lpr_linux_dup2(uint64_t old_fd, uint64_t new_fd, uint64_t flags)
{
    lpr_trace_process_event("dup2_begin", old_fd, new_fd, (int64_t)flags);
    const uint64_t known_flags = LPR_LINUX_O_CLOEXEC;
    if ((flags & ~known_flags) != 0 || new_fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    if (!lpr_fd_is_filed(old_fd) &&
        !lpr_linux_tty_fd_active(old_fd) &&
        !lpr_pipe_fd_is_active(old_fd) &&
        !lpr_linux_eventfd_active(old_fd))
    {
        struct pacha_fd_info info;
        if (!lpr_native_pipe_fd_info(old_fd, &info)) {
            return -LPR_LINUX_EBADF;
        }
    }
    if (old_fd == new_fd) {
        return flags == 0 ? (int64_t)new_fd : -LPR_LINUX_EINVAL;
    }
    const int ensure_status = lpr_fd_table_ensure_fd(new_fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    if (lpr_fd_is_filed(new_fd) ||
        lpr_linux_tty_fd_active(new_fd) ||
        lpr_pipe_fd_is_active(new_fd) ||
        lpr_linux_eventfd_active(new_fd))
    {
        const int64_t close_status = lpr_linux_close(new_fd);
        if (close_status != 0 && close_status != -LPR_LINUX_EBADF) {
            lpr_trace_process_event("dup2_target_close_error", old_fd, new_fd, close_status);
        }
    } else {
        struct pacha_fd_info info;
        if (lpr_native_pipe_fd_info(new_fd, &info)) {
            const int64_t close_status = lpr_linux_close(new_fd);
            if (close_status != 0 && close_status != -LPR_LINUX_EBADF) {
                lpr_trace_process_event("dup2_target_close_error", old_fd, new_fd, close_status);
            }
        } else if (!lpr_runtime_reserved_fd(new_fd) &&
            lpr_native_fd_info(new_fd, &info))
        {
            const int64_t close_status =
                lpr_pacha_status_to_errno(lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, new_fd));
            if (close_status != 0 && close_status != -LPR_LINUX_EBADF) {
                lpr_trace_process_event("dup2_target_close_error", old_fd, new_fd, close_status);
            }
        }
    }
    const int64_t residual_close_status = lpr_close_native_fd_if_open(new_fd);
    lpr_trace_process_event("dup2_residual_close", old_fd, new_fd, residual_close_status);
    if (residual_close_status != 0 && residual_close_status != -LPR_LINUX_EBADF) {
        return residual_close_status;
    }
    const int64_t dup_status = lpr_linux_dup_into(old_fd, (int)new_fd, 0, (flags & LPR_LINUX_O_CLOEXEC) != 0);
    lpr_trace_process_event("dup2_end", old_fd, new_fd, dup_status);
    return dup_status;
}


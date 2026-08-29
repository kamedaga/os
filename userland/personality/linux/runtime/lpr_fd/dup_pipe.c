#include "../lpr_filed_internal.h"

void lpr_fd_after_fork_child(void)
{
    /* Native endpoints and prepared service leases were committed by the
     * fork transaction before the child becomes visible to Linux callers. */
    lpr_reset_fork_child_rpc_state();
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
    lpr_fd_after_fork_child();
    lpr_linux_process_state_checked = 1;
    if (child_token != 0) {
        lpr_supervisor_token = child_token;
        lpr_supervisor_enabled = 1;
        const int64_t ready_status = lpr_supervisor_call_token(
            LPRS_OP_PROCESS_FORK_CHILD_READY,
            lpr_supervisor_token,
            -1,
            0);
        if (ready_status != 0) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, 127);
            for (;;) {
            }
        }
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
    lpr_linux_signal_after_fork_child();
    lpr_thread_after_fork_child();
    lpr_trace_process_event(
        "fork_child_state",
        (uint64_t)(uint32_t)child_pid,
        (uint64_t)(uint32_t)child_ppid,
        0);
}

int64_t lpr_linux_pipe2(uint64_t fds_raw, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_O_CLOEXEC | LPR_LINUX_O_NONBLOCK;
    if (fds_raw == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t pair[2] = {0, 0};
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_PIPE_CREATE,
        (uint64_t)(uintptr_t)pair,
        lpr_pipe_flags_to_pacha(flags));
    if (status != 0) {
        return lpr_pacha_status_to_errno(status);
    }
    struct pacha_fd_info read_info;
    struct pacha_fd_info write_info;
    if (!lpr_native_fd_info(pair[0], &read_info) || read_info.kind != PACHA_FD_KIND_PIPE ||
        !lpr_native_fd_info(pair[1], &write_info) || write_info.kind != PACHA_FD_KIND_PIPE)
    {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        return -LPR_LINUX_EIO;
    }
    const int read_fd = lpr_fd_slot_alloc_from(3);
    if (read_fd < 0) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        return read_fd;
    }
    int install_status = lpr_pipe_track_native_fd(
        (uint64_t)(uint32_t)read_fd,
        pair[0],
        &read_info);
    if (install_status != 0) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        return install_status;
    }
    const int write_fd = lpr_fd_slot_alloc_from(3);
    if (write_fd < 0) {
        lpr_control_close_fd((uint64_t)(uint32_t)read_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        return write_fd;
    }
    install_status = lpr_pipe_track_native_fd(
        (uint64_t)(uint32_t)write_fd,
        pair[1],
        &write_info);
    if (install_status != 0) {
        lpr_control_close_fd((uint64_t)(uint32_t)read_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        return install_status;
    }
    int *fds = (int *)(uintptr_t)fds_raw;
    fds[0] = read_fd;
    fds[1] = write_fd;
    return 0;
}

int64_t lpr_linux_eventfd2(uint64_t initval, uint64_t flags)
{
    const uint64_t known_flags =
        LPR_LINUX_O_CLOEXEC | LPR_LINUX_O_NONBLOCK | LPR_LINUX_EFD_SEMAPHORE;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const int fd = lpr_fd_slot_alloc();
    if (fd < 0) {
        return fd;
    }
    int wait_fd = -1;
    int notify_fd = -1;
    const int pair_status = lpr_native_wait_pair(&wait_fd, &notify_fd);
    if (pair_status != 0) return pair_status;
    const int status = lpr_control_install_fd(
        (uint64_t)(uint32_t)fd,
        LPR_FD_OPS_EVENT,
        flags & ~((uint64_t)LPR_LINUX_EFD_SEMAPHORE),
        0,
        initval);
    if (status != 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)notify_fd);
        return status;
    }
    lpr_event_backend_t *event = lpr_event_backend((uint64_t)(uint32_t)fd);
    if (event == 0) {
        lpr_control_close_fd((uint64_t)(uint32_t)fd);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)notify_fd);
        return -LPR_LINUX_EIO;
    }
    event->subtype = LPR_EVENT_BACKEND_EVENTFD;
    event->reserved1 =
        (flags & LPR_LINUX_EFD_SEMAPHORE) != 0 ? LPR_LINUX_EFD_SEMAPHORE : 0;
    event->wait_fd.raw = wait_fd;
    event->notify_fd.raw = notify_fd;
    return fd;
}

void lpr_event_backend_notify(lpr_event_backend_t *event)
{
    if (event == 0 || event->notify_fd.raw < 16) return;
    const uint8_t pending = __atomic_exchange_n(
        &event->notify_pending, 1u, __ATOMIC_ACQ_REL);
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    if (__atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u) {
        lpr_glycin_diag_event(
            "event.notify.enter",
            (uint64_t)(uintptr_t)event,
            __atomic_load_n(&event->counter, __ATOMIC_ACQUIRE),
            pending,
            event->notify_fd.raw);
    }
#endif
    if (pending != 0)
        return;
    const struct pacha_ipc_msg message = {0};
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_SEND,
        (uint64_t)(uint32_t)event->notify_fd.raw,
        (uint64_t)(uintptr_t)&message);
    if (status != 0)
        __atomic_store_n(&event->notify_pending, 0u, __ATOMIC_RELEASE);
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    if (__atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u) {
        lpr_glycin_diag_event(
            "event.notify.exit",
            (uint64_t)(uintptr_t)event,
            __atomic_load_n(&event->counter, __ATOMIC_ACQUIRE),
            __atomic_load_n(&event->notify_pending, __ATOMIC_ACQUIRE),
            status);
    }
#endif
}

int lpr_eventfd_native_wait_fd(uint64_t fd)
{
    lpr_event_backend_t *event = lpr_event_backend(fd);
    return event != 0 ? event->wait_fd.raw : -1;
}

void lpr_eventfd_drain_wait(uint64_t fd)
{
    lpr_event_backend_t *event = lpr_event_backend(fd);
    if (event == 0 || event->wait_fd.raw < 16) return;
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    if (__atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u) {
        lpr_glycin_diag_event(
            "event.drain.enter",
            fd,
            __atomic_load_n(&event->counter, __ATOMIC_ACQUIRE),
            __atomic_load_n(&event->notify_pending, __ATOMIC_ACQUIRE),
            event->wait_fd.raw);
    }
#endif
    __atomic_store_n(&event->notify_pending, 0u, __ATOMIC_RELEASE);
    lpr_native_wait_drain(event->wait_fd.raw);
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    if (__atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u) {
        lpr_glycin_diag_event(
            "event.drain.exit",
            fd,
            __atomic_load_n(&event->counter, __ATOMIC_ACQUIRE),
            __atomic_load_n(&event->notify_pending, __ATOMIC_ACQUIRE),
            event->wait_fd.raw);
    }
#endif
}

int64_t lpr_linux_dup_into(uint64_t fd, int target_fd, uint64_t min_fd, uint64_t cloexec)
{
    if (fd > LPR_LINUX_FD_MAX || lpr_control_require_fd(fd) != 0) {
        return -LPR_LINUX_EBADF;
    }
    if (target_fd < 0) {
        if (min_fd > LPR_LINUX_FD_MAX) {
            return -LPR_LINUX_EINVAL;
        }
        const int new_fd = lpr_fd_slot_alloc_from(min_fd);
        if (new_fd < 0) {
            return new_fd;
        }
        if (lpr_control_dup_fd(fd, (uint64_t)(uint32_t)new_fd, cloexec) != 0) {
            return -LPR_LINUX_EMFILE;
        }
        return new_fd;
    }
    const uint64_t new_fd = (uint64_t)(uint32_t)target_fd;
    if (target_fd < 0 || new_fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    if (fd == new_fd) {
        return (int64_t)new_fd;
    }
    const int ensure_status = lpr_fd_table_ensure_fd(new_fd);
    if (ensure_status != 0) {
        return ensure_status;
    }
    if (lpr_control_fd_active(new_fd)) {
        const int64_t close_status = lpr_linux_close(new_fd);
        if (close_status != 0) {
            return close_status;
        }
    }
    return lpr_control_dup_fd(fd, new_fd, cloexec) == 0 ?
        (int64_t)new_fd : -LPR_LINUX_EBADF;
}

int64_t lpr_linux_dup(uint64_t fd, uint64_t min_fd, uint64_t cloexec)
{
    return lpr_linux_dup_into(fd, -1, min_fd, cloexec);
}

int64_t lpr_linux_dup2(uint64_t old_fd, uint64_t new_fd, uint64_t flags)
{
    const uint64_t known_flags = LPR_LINUX_O_CLOEXEC;
    if ((flags & ~known_flags) != 0 || new_fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    if (old_fd == new_fd) {
        return flags == 0 && lpr_control_require_fd(old_fd) == 0 ?
            (int64_t)new_fd : -LPR_LINUX_EINVAL;
    }
    return lpr_linux_dup_into(
        old_fd,
        (int)new_fd,
        0,
        (flags & LPR_LINUX_O_CLOEXEC) != 0);
}

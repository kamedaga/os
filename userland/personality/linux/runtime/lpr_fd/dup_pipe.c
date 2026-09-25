#include "../lpr_filed_internal.h"
#include "allocate.h"

void lpr_fd_after_fork_child(void)
{
    /* Native endpoints and prepared service leases were committed by the
     * fork transaction before the child becomes visible to Linux callers. */
    lpr_reset_fork_child_rpc_state();
    /* The diagnostic slot pointer was copied from the parent and belongs to
     * the parent's process record, so it must not be written through here.
     * The child is left marked as already attempted rather than attaching on
     * its next call: the fork is not complete until the child reports ready,
     * and issuing a supervisor request before that point stalls the parent
     * inside fork.  A child that goes on to exec attaches from the new image
     * instead, which covers everything /proc is read for. */
    lpr_diag_slot = 0;
    lpr_diag_attach_attempted = 1;
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
    /* The immutable credential snapshot is inherited with the process image,
     * matching the supervisor's fork copy. Exec reloads it from GET_STATE. */
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
    lpr_pipe_backend_t *backends[2] = {0};
    const struct pacha_fd_info *infos[2] = {&read_info, &write_info};
    lpr_fd_install_t installs[2];
    int install_status = 0;
    for (unsigned i = 0; i < 2; ++i) {
        backends[i] = lpr_backend_state_alloc(sizeof(*backends[i]));
        if (!backends[i]) { install_status = -LPR_LINUX_ENOMEM; break; }
        const uint32_t linux_flags = lpr_pipe_flags_from_info(infos[i]);
        *backends[i] = (lpr_pipe_backend_t){
            .active = 1, .readable = i == 0, .writable = i == 1,
            .flags = linux_flags, .native.raw = (int32_t)pair[i],
        };
        installs[i] = (lpr_fd_install_t){
            .ops_id = LPR_FD_OPS_PIPE,
            .fd_flags = (flags & LPR_LINUX_O_CLOEXEC) ? LPR_FD_ENTRY_CLOEXEC : 0,
            .access_mode = i == 0 ? LPR_LINUX_O_RDONLY : LPR_LINUX_O_WRONLY,
            .status_flags = (flags & LPR_LINUX_O_NONBLOCK) ? LPR_OFD_NONBLOCK : 0,
            .rights = LPR_FD_RIGHT_STAT | LPR_FD_RIGHT_DUP | LPR_FD_RIGHT_IOCTL |
                (i == 0 ? LPR_FD_RIGHT_READ : LPR_FD_RIGHT_WRITE),
            .backend_state = backends[i], .backend_state_bytes = sizeof(*backends[i]),
        };
    }
    lpr_linux_fd_t installed[2];
    if (!install_status)
        install_status = lpr_fd_alloc_initialized_batch(installs, 2, installed);
    if (install_status != 0) {
        for (unsigned i = 0; i < 2; ++i) {
            if (backends[i]) (void)lpr_backend_state_free(backends[i], sizeof(*backends[i]));
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[i]);
        }
        return install_status;
    }
    int *fds = (int *)(uintptr_t)fds_raw;
    fds[0] = (int)installed[0];
    fds[1] = (int)installed[1];
    return 0;
}

int64_t lpr_linux_eventfd2(uint64_t initval, uint64_t flags)
{
    const uint64_t known_flags =
        LPR_LINUX_O_CLOEXEC | LPR_LINUX_O_NONBLOCK | LPR_LINUX_EFD_SEMAPHORE;
    if ((flags & ~known_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    int wait_fd = -1;
    int notify_fd = -1;
    const int pair_status = lpr_native_wait_pair(&wait_fd, &notify_fd);
    if (pair_status != 0) return pair_status;
    lpr_event_backend_t *event = lpr_backend_state_alloc(sizeof(*event));
    if (event == 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)notify_fd);
        return -LPR_LINUX_ENOMEM;
    }
    *event = (lpr_event_backend_t){
        .active = 1, .subtype = LPR_EVENT_BACKEND_EVENTFD,
        .reserved1 = (flags & LPR_LINUX_EFD_SEMAPHORE) ? LPR_LINUX_EFD_SEMAPHORE : 0,
        .flags = (uint32_t)(flags & ~((uint64_t)LPR_LINUX_EFD_SEMAPHORE)),
        .counter = initval, .wait_fd.raw = wait_fd, .notify_fd.raw = notify_fd,
    };
    const lpr_fd_install_t install = {
        .ops_id = LPR_FD_OPS_EVENT,
        .fd_flags = (flags & LPR_LINUX_O_CLOEXEC) ? LPR_FD_ENTRY_CLOEXEC : 0,
        .status_flags = (flags & LPR_LINUX_O_NONBLOCK) ? LPR_OFD_NONBLOCK : 0,
        .rights = LPR_FD_RIGHT_STAT | LPR_FD_RIGHT_DUP | LPR_FD_RIGHT_READ |
            LPR_FD_RIGHT_WRITE | LPR_FD_RIGHT_IOCTL,
        .offset = initval, .backend_state = event, .backend_state_bytes = sizeof(*event),
    };
    const int fd = lpr_fd_alloc_initialized(&install);
    if (fd < 0) {
        (void)lpr_backend_state_free(event, sizeof(*event));
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)notify_fd);
    }
    return fd;
}

void lpr_event_backend_notify(lpr_event_backend_t *event)
{
    if (event == 0 || event->notify_fd.raw < 16) return;
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    if (__atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u) {
        lpr_glycin_diag_event(
            "event.notify.send.enter",
            (uint64_t)(uintptr_t)event,
            __atomic_load_n(&event->counter, __ATOMIC_ACQUIRE),
            __atomic_load_n(&event->notify_pending, __ATOMIC_ACQUIRE),
            event->notify_fd.raw);
    }
#endif
    const struct pacha_ipc_msg message = {0};
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_SEND,
        (uint64_t)(uint32_t)event->notify_fd.raw,
        (uint64_t)(uintptr_t)&message);
    (void)status;
#if defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG
    if (__atomic_load_n(&lpr_glycin_diag_armed, __ATOMIC_ACQUIRE) != 0u) {
        lpr_glycin_diag_event(
            "event.notify.send.exit",
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
    /* Readability is owned by the native channel queue, while the Linux
     * counter/deadline is the durable logical state.  Do not gate receives on
     * a second userspace state machine: several waiters can be released by one
     * channel message, and a stale waiter may otherwise consume a producer's
     * pre-send state transition without consuming the message itself.
     * Consume at most one token so a continuously active producer cannot keep
     * this thread here instead of returning it to the logical-state scan. */
    struct pacha_ipc_msg message = {0};
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_RECV,
        (uint64_t)(uint32_t)event->wait_fd.raw,
        (uint64_t)(uintptr_t)&message);
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
        for (;;) {
            const int64_t prepared = lpr_fd_prepare_dup(fd);
            if (prepared) return prepared;
            lpr_linux_fd_t new_fd;
            if (!lpr_fd_table_dup_excluding(&lpr_control_fd_table,
                    (uint32_t)fd, (uint32_t)min_fd, cloexec ? LPR_FD_ENTRY_CLOEXEC : 0,
                    lpr_fd_allocation_excluded, sizeof(lpr_fd_allocation_excluded) /
                        sizeof(lpr_fd_allocation_excluded[0]), &new_fd))
                return new_fd;
            const uint64_t capacity = lpr_fd_table_capacity;
            if (capacity >= LPR_FD_TABLE_MAX_SIZE ||
                lpr_fd_table_ensure_capacity(min_fd >= capacity ? min_fd + 1 : capacity + 1))
                return -LPR_LINUX_EMFILE;
        }
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
    const int64_t prepared = lpr_fd_prepare_dup(fd);
    if (prepared) return prepared;
    lpr_fd_drop_t drop;
    if (lpr_fd_table_dup_replace(&lpr_control_fd_table, (uint32_t)fd,
            (uint32_t)new_fd, cloexec ? LPR_FD_ENTRY_CLOEXEC : 0, &drop))
        return -LPR_LINUX_EBADF;
    if (drop.ready) (void)lpr_backend_finish_drop(&drop);
    return (int64_t)new_fd;
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

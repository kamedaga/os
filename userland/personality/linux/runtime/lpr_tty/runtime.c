#include "../lpr_filed_internal.h"

static const char *lpr_take_boot_ctty_env(void)
{
    if (lpr_load_manifest() &&
        (lpr_process_manifest.flags & LPR_MANIFEST_FLAG_DEFAULT_STDIO) != 0 &&
        lpr_process_manifest.ctty[0] != 0)
    {
        return lpr_process_manifest.ctty;
    }
    return 0;
}

int64_t lpr_install_stdio_fd_from_tty(uint64_t tty_fd, uint64_t target_fd)
{
    if (lpr_fd_linux_visible_active(target_fd)) {
        return 0;
    }
    if (tty_fd == target_fd) {
        return 0;
    }
    return lpr_linux_dup2(tty_fd, target_fd, 0);
}

void lpr_linux_ensure_default_stdio(void)
{
    if (lpr_default_stdio_checked) {
        return;
    }
    lpr_default_stdio_checked = 1;

    const char *path = lpr_take_boot_ctty_env();
    if (path == 0 || path[0] == 0) {
        return;
    }
    const int need_stdin = !lpr_fd_linux_visible_active(0);
    const int need_stdout = !lpr_fd_linux_visible_active(1);
    const int need_stderr = !lpr_fd_linux_visible_active(2);
    if (!need_stdin && !need_stdout && !need_stderr) {
        return;
    }

    const int64_t tty_fd = lpr_tty_open_path(path, LPR_LINUX_O_RDWR);
    if (tty_fd < 0) {
        return;
    }
    if (need_stdin &&
        lpr_install_stdio_fd_from_tty((uint64_t)(uint32_t)tty_fd, 0) < 0)
    {
        (void)lpr_linux_close((uint64_t)(uint32_t)tty_fd);
        return;
    }
    if (need_stdout &&
        lpr_install_stdio_fd_from_tty((uint64_t)(uint32_t)tty_fd, 1) < 0)
    {
        (void)lpr_linux_close((uint64_t)(uint32_t)tty_fd);
        return;
    }
    if (need_stderr &&
        lpr_install_stdio_fd_from_tty((uint64_t)(uint32_t)tty_fd, 2) < 0)
    {
        (void)lpr_linux_close((uint64_t)(uint32_t)tty_fd);
        return;
    }
    if (tty_fd > 2) {
        (void)lpr_linux_close((uint64_t)(uint32_t)tty_fd);
    }
}

int64_t lpr_tty_io(uint64_t op, uint64_t fd, uint64_t buf, uint64_t count)
{
    if (!lpr_linux_tty_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    if (buf == 0 && count != 0) {
        return -LPR_LINUX_EFAULT;
    }
    if (count == 0) {
        return 0;
    }

    const uint32_t wait_events =
        op == TERMD_OP_HANDLE_WRITE ? TERMD_POLLOUT : TERMD_POLLIN;
    const uint8_t tty_type = lpr_tty_backend(fd)->reserved0;
    for (;;) {
        void *page = 0;
        const int page_fd = lpr_create_tty_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        termd_io_request_t *io = (termd_io_request_t *)lpr_termd_payload(page);
        lpr_memset(io, 0, sizeof(*io));
        io->handle = lpr_tty_backend(fd)->handle;
        io->length = count > TERMD_IO_BYTES ? TERMD_IO_BYTES : count;
        io->flags = TERMD_IO_F_NOWAIT;
        lpr_fill_termd_caller(&io->tty.session_id, &io->tty.process_id, &io->tty.pgrp_id);
        lpr_fill_termd_signal_state(&io->tty.signal_mask, &io->tty.signal_ignored);
        if (op == TERMD_OP_HANDLE_WRITE && io->length != 0) {
            lpr_memcpy(io->data, (const void *)(uintptr_t)buf, (size_t)io->length);
        }
        uint64_t result = 0;
        const int64_t status = lpr_termd_call(op, page_fd, page, sizeof(*io), &result);
        if (status == 0 && op == TERMD_OP_HANDLE_READ && result != 0) {
            lpr_memcpy((void *)(uintptr_t)buf, io->data, (size_t)result);
        }
        lpr_destroy_tty_wire_page(page_fd, page);
        if (status == 0) {
            return (int64_t)result;
        }
        if (status == -LPR_LINUX_EINTR) {
            const int interrupted = lpr_linux_pump_tty_signals();
            if (op == TERMD_OP_HANDLE_READ &&
                tty_type == LPR_TTY_BACKEND_PTY_MASTER)
            {
                continue;
            }
            if (interrupted ||
                (op == TERMD_OP_HANDLE_READ &&
                 tty_type == LPR_TTY_BACKEND_PTY_SLAVE))
            {
                return status;
            }
            continue;
        }
        if (status != -LPR_LINUX_EAGAIN ||
            (lpr_tty_backend(fd)->flags & LPR_LINUX_O_NONBLOCK) != 0)
        {
            return status;
        }
        const int64_t wait_status = lpr_tty_wait(fd, wait_events);
        if (wait_status != 0) {
            return wait_status;
        }
    }
}

int64_t lpr_tty_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
    if (!lpr_linux_tty_fd_active(fd)) {
        return -LPR_LINUX_EBADF;
    }
    request = (uint32_t)request;
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    termd_ioctl_request_t *ioctl_req = (termd_ioctl_request_t *)lpr_termd_payload(page);
    lpr_memset(ioctl_req, 0, sizeof(*ioctl_req));
    ioctl_req->handle = lpr_tty_backend(fd)->handle;
    ioctl_req->request = request;
    lpr_fill_termd_caller(
        &ioctl_req->tty.session_id,
        &ioctl_req->tty.process_id,
        &ioctl_req->tty.pgrp_id);
    lpr_fill_termd_signal_state(
        &ioctl_req->tty.signal_mask,
        &ioctl_req->tty.signal_ignored);

    switch (request) {
    case LPR_LINUX_TCSETS:
    case LPR_LINUX_TCSETSW:
    case LPR_LINUX_TCSETSF:
        if (arg == 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EFAULT;
        }
        lpr_memcpy(ioctl_req->data, (const void *)(uintptr_t)arg, LPR_LINUX_TERMIOS_BYTES);
        break;
    case LPR_LINUX_TIOCSWINSZ:
        if (arg == 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EFAULT;
        }
        ioctl_req->arg0 = *(const uint16_t *)(uintptr_t)arg;
        ioctl_req->arg1 = *(const uint16_t *)((uintptr_t)arg + 2u);
        break;
    case LPR_LINUX_TIOCSPGRP:
    case LPR_LINUX_TIOCSPTLCK:
        if (arg == 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EFAULT;
        }
        ioctl_req->arg0 = (uint64_t)*(const int *)(uintptr_t)arg;
        lpr_memcpy(ioctl_req->data, (const void *)(uintptr_t)arg, sizeof(int));
        break;
    default:
        break;
    }

    uint64_t result = 0;
    const int64_t status =
        lpr_termd_call(TERMD_OP_HANDLE_IOCTL, page_fd, page, sizeof(*ioctl_req), &result);
    if (status == 0) {
        switch (request) {
        case LPR_LINUX_TCGETS:
            if (arg == 0) {
                lpr_destroy_tty_wire_page(page_fd, page);
                return -LPR_LINUX_EFAULT;
            }
            lpr_memcpy((void *)(uintptr_t)arg, ioctl_req->data, LPR_LINUX_TERMIOS_BYTES);
            break;
        case LPR_LINUX_TIOCGWINSZ:
            if (arg == 0) {
                lpr_destroy_tty_wire_page(page_fd, page);
                return -LPR_LINUX_EFAULT;
            }
            *(uint16_t *)(uintptr_t)arg = (uint16_t)ioctl_req->result0;
            *(uint16_t *)((uintptr_t)arg + 2u) = (uint16_t)ioctl_req->result1;
            *(uint16_t *)((uintptr_t)arg + 4u) = 0;
            *(uint16_t *)((uintptr_t)arg + 6u) = 0;
            break;
        case LPR_LINUX_TIOCGPGRP:
        case LPR_LINUX_TIOCGPTN:
        case LPR_LINUX_FIONREAD:
            if (arg == 0) {
                lpr_destroy_tty_wire_page(page_fd, page);
                return -LPR_LINUX_EFAULT;
            }
            *(int *)(uintptr_t)arg = (int)ioctl_req->result0;
            break;
        default:
            break;
        }
    }
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}

int64_t lpr_iov_scalar_io(uint64_t fd, uint64_t iov_raw, uint64_t iov_count, int write)
{
    const lpr_linux_iovec_t *iov = (const lpr_linux_iovec_t *)(uintptr_t)iov_raw;
    int64_t total = 0;
    for (uint64_t i = 0; i < iov_count; i += 1) {
        if (iov[i].len == 0) {
            continue;
        }
        const int64_t n = write ?
            lpr_linux_write(fd, iov[i].base, iov[i].len) :
            lpr_linux_read(fd, iov[i].base, iov[i].len);
        if (n < 0) {
            return total != 0 ? total : n;
        }
        if (n > INT64_MAX - total) {
            return -LPR_LINUX_EINVAL;
        }
        total += n;
        if ((uint64_t)n < iov[i].len) {
            break;
        }
    }
    return total;
}

uint32_t lpr_linux_tty_poll_events(uint64_t fd, uint32_t events)
{
    if (!lpr_linux_tty_fd_active(fd)) {
        return 0;
    }
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return 0;
    }
    lpr_tty_backend_t *tty = lpr_tty_backend(fd);
    if (tty != 0 && tty->wait_fd.raw >= 16) {
        lpr_native_wait_drain(tty->wait_fd.raw);
    }
    termd_poll_request_t *poll_req = (termd_poll_request_t *)lpr_termd_payload(page);
    lpr_memset(poll_req, 0, sizeof(*poll_req));
    poll_req->handle = lpr_tty_backend(fd)->handle;
    poll_req->events = events;
    lpr_fill_termd_caller(
        &poll_req->tty.session_id,
        &poll_req->tty.process_id,
        &poll_req->tty.pgrp_id);
    lpr_fill_termd_signal_state(
        &poll_req->tty.signal_mask,
        &poll_req->tty.signal_ignored);
    uint64_t result = 0;
    const int64_t status =
        lpr_termd_call(TERMD_OP_HANDLE_POLL, page_fd, page, sizeof(*poll_req), &result);
    uint32_t revents = status == 0 ? poll_req->revents : TERMD_POLLERR;
    lpr_destroy_tty_wire_page(page_fd, page);
    return revents;
}

int64_t lpr_tty_wait(uint64_t fd, uint32_t events)
{
    lpr_tty_backend_t *tty = lpr_tty_backend(fd);
    if (tty == 0) {
        return -LPR_LINUX_EBADF;
    }
    if (tty->wait_fd.raw < 16) {
        return -LPR_LINUX_EIO;
    }

    lpr_wait_deadline_t deadline;
    int64_t status = lpr_wait_deadline_init(&deadline, -1);
    if (status != 0) return status;
    for (;;) {
        const uint32_t revents = lpr_linux_tty_poll_events(
            fd,
            events | TERMD_POLLERR | TERMD_POLLHUP);
        if ((revents & events) != 0) {
            return 0;
        }
        if ((revents & TERMD_POLLERR) != 0) {
            return -LPR_LINUX_EIO;
        }
        if ((revents & TERMD_POLLHUP) != 0) {
            return 0;
        }
        if (lpr_linux_pump_tty_signals())
            return LPR_WAIT_RESTART_SYSCALL;
        lpr_wait_graph_t graph;
        lpr_wait_graph_init(&graph);
        status = lpr_wait_graph_add_fd(&graph, fd, events);
        if (status != 0) return status;
        status = lpr_wait_graph_block(&graph, &deadline);
        if (status != 0) return status;
    }
}

int lpr_linux_signal_process_fd(int process_fd, uint32_t signo)
{
    if (process_fd < 16 || signo == 0 || signo > LPR_LINUX_SIGNAL_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    if (signo == LPR_LINUX_SIGKILL) {
        return lpr_pacha_status_to_errno(lpr_pacha_syscall2(
            PACHAOS_SYSCALL_PROCESS_KILL,
            (uint64_t)(uint32_t)process_fd,
            signo));
    }
    if (signo == LPR_LINUX_SIGSTOP) {
        return lpr_pacha_status_to_errno(lpr_pacha_syscall2(
            PACHAOS_SYSCALL_PROCESS_STOP,
            (uint64_t)(uint32_t)process_fd,
            signo));
    }
    return lpr_pacha_status_to_errno(lpr_pacha_syscall2(
        PACHAOS_SYSCALL_PROCESS_SIGNAL,
        (uint64_t)(uint32_t)process_fd,
        signo));
}

int lpr_linux_signal_pgrp(int32_t pgrp, uint32_t signo)
{
    if (pgrp <= 0 || signo > LPR_LINUX_SIGNAL_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_supervisor_enabled) {
        const int64_t status = lpr_supervisor_kill_pid(-pgrp, signo, 0);
        return status == 0 ? 0 : (int)status;
    }
    int delivered = 0;
    if (lpr_linux_current_pgrp == pgrp) {
        if (signo != 0) {
            lpr_linux_queue_signal(signo);
        }
        delivered = 1;
    }
    for (uint64_t i = 0; i < LPR_LINUX_PROCESS_TABLE_SIZE; i++) {
        lpr_linux_process_entry_t *entry = &lpr_linux_processes[i];
        if (!entry->active || entry->process_fd < 16 || entry->linux_pgrp != pgrp) {
            continue;
        }
        if (signo != 0) {
            (void)lpr_linux_signal_process_fd(entry->process_fd, signo);
        }
        delivered = 1;
    }
    return delivered ? 0 : -LPR_LINUX_ESRCH;
}

static int lpr_linux_tty_signal_interrupts_current(int32_t pgrp, uint32_t signo)
{
    if (pgrp != lpr_linux_current_pgrp) {
        return 0;
    }
    const uint64_t bit = lpr_linux_signal_bit(signo);
    if (bit == 0 || (lpr_linux_signal_mask & bit) != 0) {
        return 0;
    }
    const lpr_linux_sigaction_record_t *action = &lpr_linux_sigactions[signo];
    return action->handler != LPR_LINUX_SIG_IGN &&
        !(action->handler == LPR_LINUX_SIG_DFL &&
          lpr_linux_default_signal_ignored(signo));
}

int lpr_linux_pump_tty_signals(void)
{
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return 0;
    }
    int interrupted = 0;
    for (uint64_t i = 0; i < 8u; i++) {
        termd_signal_request_t *signal_req = (termd_signal_request_t *)lpr_termd_payload(page);
        lpr_memset(signal_req, 0, sizeof(*signal_req));
        uint64_t result = 0;
        const int64_t status =
            lpr_termd_call(TERMD_OP_SIGNAL_TAKE, page_fd, page, sizeof(*signal_req), &result);
        if (status != 0 || result == 0 || signal_req->signo == 0) {
            break;
        }
        if (lpr_linux_tty_signal_interrupts_current(
                (int32_t)signal_req->pgrp_id, signal_req->signo))
        {
            interrupted = 1;
        }
        (void)lpr_linux_signal_pgrp((int32_t)signal_req->pgrp_id, signal_req->signo);
    }
    lpr_destroy_tty_wire_page(page_fd, page);
    return interrupted;
}

uint32_t lpr_linux_eventfd_poll_events(uint64_t fd, uint32_t events)
{
    if (lpr_linux_timerfd_active(fd)) {
        return lpr_linux_timerfd_poll_events(fd, events);
    }
    if (lpr_linux_signalfd_active(fd)) {
        return lpr_linux_signalfd_poll_events(fd, events);
    }
    if (!lpr_linux_eventfd_active(fd)) {
        return 0;
    }
    uint32_t revents = 0;
    if ((events & 0x0001u) != 0 && lpr_event_backend(fd)->counter != 0) {
        revents |= 0x0001u;
    }
    if ((events & 0x0004u) != 0 &&
        lpr_event_backend(fd)->counter < UINT64_MAX - 1u) {
        revents |= 0x0004u;
    }
    return revents;
}

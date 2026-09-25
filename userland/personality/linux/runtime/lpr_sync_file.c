#include "lpr_filed_internal.h"
#include "lpr_fd/allocate.h"

int64_t lpr_sync_file_install_wait(int wait_fd)
{
    struct pacha_fd_info info;
    lpr_memset(&info, 0, sizeof(info));
    if (wait_fd < 16 ||
        !lpr_native_fd_info((uint64_t)(uint32_t)wait_fd, &info) ||
        info.kind != PACHA_FD_KIND_CHANNEL ||
        (info.rights & (PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL |
            PACHA_FD_RIGHT_CLOSE)) !=
            (PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL |
                PACHA_FD_RIGHT_CLOSE)) {
        if (wait_fd >= 16) {
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
        }
        return -LPR_LINUX_EBADF;
    }

    const uint64_t flags = LPR_LINUX_O_RDONLY | LPR_LINUX_O_CLOEXEC;
    lpr_sync_file_backend_t *sync_file = lpr_backend_state_alloc(sizeof(*sync_file));
    if (sync_file == 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
        return -LPR_LINUX_ENOMEM;
    }
    *sync_file = (lpr_sync_file_backend_t){
        .active = 1, .flags = (uint32_t)flags, .wait_fd.raw = wait_fd,
    };
    const lpr_fd_install_t install = {
        .ops_id = LPR_FD_OPS_SYNC_FILE, .fd_flags = LPR_FD_ENTRY_CLOEXEC,
        .access_mode = LPR_LINUX_O_RDONLY,
        .rights = LPR_FD_RIGHT_STAT | LPR_FD_RIGHT_DUP,
        .backend_state = sync_file, .backend_state_bytes = sizeof(*sync_file),
    };
    /* Selecting a free number and publishing its initialized backend must
     * be atomic with respect to other threads (including SCM_RIGHTS import).
     * Do not report EMFILE merely because a previously observed slot moved. */
    const int fd = lpr_fd_alloc_initialized(&install);
    if (fd < 0) {
        (void)lpr_backend_state_free(sync_file, sizeof(*sync_file));
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
    }
    return fd;
}

int64_t lpr_sync_file_create_signaled(void)
{

    int wait_fd = -1;
    int notify_fd = -1;
    const int pair_status = lpr_native_wait_pair(&wait_fd, &notify_fd);
    if (pair_status != 0) return pair_status;

    const struct pacha_ipc_msg message = {0};
    const int64_t signal_status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_SEND,
        (uint64_t)(uint32_t)notify_fd,
        (uint64_t)(uintptr_t)&message);
    (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)notify_fd);
    if (signal_status != 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
        return lpr_pacha_status_to_errno(signal_status);
    }
    return lpr_sync_file_install_wait(wait_fd);
}

int lpr_sync_file_native_wait_fd(uint64_t fd)
{
    const lpr_sync_file_backend_t *sync_file = lpr_sync_file_backend(fd);
    return sync_file != 0 ? sync_file->wait_fd.raw : -1;
}

int lpr_sync_file_duplicate_wait(uint64_t fd)
{
    const int wait_fd = lpr_sync_file_native_wait_fd(fd);
    struct pacha_fd_info info;
    lpr_memset(&info, 0, sizeof(info));
    if (wait_fd < 16 ||
        !lpr_native_fd_info((uint64_t)(uint32_t)wait_fd, &info) ||
        (info.rights & (PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER)) !=
            (PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER))
        return -LPR_LINUX_EBADF;
    const int64_t duplicate = lpr_pacha_syscall4(
        PACHA_FD_SYSCALL_FCNTL,
        (uint64_t)(uint32_t)wait_fd,
        PACHA_FD_FCNTL_DUP,
        16,
        info.rights);
    return duplicate >= 16 ? (int)duplicate :
        (int)lpr_pacha_status_to_errno(duplicate);
}

uint32_t lpr_sync_file_poll_events(uint64_t fd, uint32_t events)
{
    const int wait_fd = lpr_sync_file_native_wait_fd(fd);
    if (wait_fd < 16) return 0;
    struct pacha_pollfd pollfd = {
        .fd = wait_fd,
        .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP,
    };
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_FD_POLL,
        (uint64_t)(uintptr_t)&pollfd,
        1);
    if (status < 0)
        return 0;
    /* A queued completion stays readable after the notifier closes. A bare
     * hangup instead means backend loss/failed work, never successful signal. */
    if (pollfd.revents & PACHA_FD_EVENT_READABLE)
        return events & 0x0001u;
    return pollfd.revents & PACHA_FD_EVENT_HANGUP ? 0x0008u : 0;
}

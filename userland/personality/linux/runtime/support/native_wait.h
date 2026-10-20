#pragma once

#include "syscall.h"
#include <pacha/abi.h>
#include <pacha/ipc.h>
#include <pacha/status.h>
#include <pachaos/abi.h>
#include <stdint.h>

static inline int lpr_native_wait_pair_flags(
    pacha_fd_t *out_local, pacha_fd_t *out_remote, uint64_t flags)
{
    if (out_local == 0 || out_remote == 0) return -22;
    uint64_t pair[2] = {0, 0};
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV;
    const int64_t status = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_IPC_CHANNEL_CREATE,
        (uint64_t)(uintptr_t)pair,
        rights,
        flags);
    if (status != 0 || pair[0] < 16 || pair[1] < 16 ||
        pair[0] >= PACHA_THREAD_SELF_FD || pair[1] >= PACHA_THREAD_SELF_FD)
    {
        if (pair[0] >= 16) (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        if (pair[1] >= 16) (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        return status == 0 ? -PACHA_LINUX_EMFILE : (int)pacha_kernel_status_to_errno(status);
    }
    *out_local = (pacha_fd_t)pair[0];
    *out_remote = (pacha_fd_t)pair[1];
    return 0;
}

static inline int lpr_native_wait_pair(pacha_fd_t *out_local, pacha_fd_t *out_remote)
{
    return lpr_native_wait_pair_flags(
        out_local, out_remote, PACHA_FD_FLAG_INHERIT);
}

static inline uint64_t lpr_native_wait_drain_events(pacha_fd_t fd)
{
    if (fd < 16) return 0;
    uint64_t events = 0;
    /* NetD emits at most one queued message per readiness bit.  Drain all
     * currently coalesced bits so the Linux-side cache and acknowledgement
     * describe the same observation. */
    for (unsigned i = 0; i < 8; ++i) {
        struct pacha_ipc_msg message = {0};
        const int64_t status = lpr_ipc_message(PACHAOS_SYSCALL_IPC_RECV, (uint64_t)fd, 0, (uint64_t)(uintptr_t)((uint64_t)(uintptr_t)&message), 0, 0, 0);
        if (status != 0) break;
        events |= message.word0;
    }
    return events;
}

static inline void lpr_native_wait_drain(pacha_fd_t fd)
{
    (void)lpr_native_wait_drain_events(fd);
}

static inline int64_t lpr_native_ipc_recv_wait(
    uint64_t fd,
    struct pacha_ipc_msg *message)
{
    int64_t status;
    do {
        status = lpr_ipc_message(PACHAOS_SYSCALL_IPC_RECV_WAIT, fd, 0, (uint64_t)(uintptr_t)((uint64_t)(uintptr_t)message), UINT64_MAX, PACHA_WAIT_UNINTERRUPTIBLE, 0);
        // Internal channel waits defer signals. A racing readiness edge may
        // require another receive on the same channel.
    } while (status == PACHA_SYSCALL_ERR_NOT_READY);
    return status;
}

static inline int64_t lpr_native_fd_wait_writable(uint64_t fd)
{
    for (;;) {
        struct pacha_pollfd pollfd = {
            .fd = (pacha_fd_t)fd,
            .events = PACHA_FD_EVENT_WRITABLE,
            .revents = 0,
        };
        const int64_t status = lpr_pacha_syscall4(
            PACHA_FD_SYSCALL_WAIT_MANY,
            (uint64_t)(uintptr_t)&pollfd,
            1,
            PACHA_FD_WAIT_FOREVER,
            PACHA_WAIT_UNINTERRUPTIBLE);
        if (status > 0 && pollfd.revents != 0) {
            return 0;
        }
        if (status != PACHA_SYSCALL_ERR_NOT_READY &&
            status != -PACHA_SYSCALL_ERR_NOT_READY)
        {
            return status != 0 ? status : PACHA_SYSCALL_ERR_NOT_READY;
        }
    }
}

static inline int64_t lpr_native_ipc_send_wait(
    uint64_t fd,
    const struct pacha_ipc_msg *message)
{
    for (;;) {
        const int64_t status = lpr_ipc_message(PACHAOS_SYSCALL_IPC_SEND, fd, (uint64_t)(uintptr_t)((uint64_t)(uintptr_t)message), 0, 0, 0, 0);
        if (status == 0) {
            return 0;
        }
        if (status != PACHA_SYSCALL_ERR_NOT_READY &&
            status != -PACHA_SYSCALL_ERR_NOT_READY)
        {
            return status;
        }
        const int64_t wait_status = lpr_native_fd_wait_writable(fd);
        if (wait_status != 0) {
            return wait_status;
        }
    }
}

static inline int64_t lpr_native_ipc_call_wait(
    uint64_t fd, const struct pacha_ipc_msg *message,
    struct pacha_ipc_msg *reply, int *published)
{
    if (published) *published = 0;
    for (;;) {
        int sent = 0;
        const int64_t status = lpr_ipc_message(
            PACHAOS_SYSCALL_IPC_CALL, fd, (uint64_t)(uintptr_t)message,
            (uint64_t)(uintptr_t)reply, UINT64_MAX,
            PACHA_WAIT_UNINTERRUPTIBLE, &sent);
        if (published) *published = sent;
        if (sent || (status != PACHA_SYSCALL_ERR_NOT_READY &&
                     status != -PACHA_SYSCALL_ERR_NOT_READY)) return status;
        const int64_t wait_status = lpr_native_fd_wait_writable(fd);
        if (wait_status != 0) return wait_status;
    }
}

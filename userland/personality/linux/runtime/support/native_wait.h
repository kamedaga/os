#pragma once

#include "syscall.h"
#include <pacha/abi.h>
#include <pacha/ipc.h>
#include <pacha/status.h>
#include <pachaos/abi.h>
#include <stdint.h>

static inline int lpr_native_wait_pair(int *out_local, int *out_remote)
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
        PACHA_FD_FLAG_INHERIT);
    if (status != 0 || pair[0] < 16 || pair[1] < 16 ||
        pair[0] > INT32_MAX || pair[1] > INT32_MAX)
    {
        if (pair[0] >= 16) (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        if (pair[1] >= 16) (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
        return status == 0 ? -PACHA_LINUX_EMFILE : (int)pacha_kernel_status_to_errno(status);
    }
    *out_local = (int)pair[0];
    *out_remote = (int)pair[1];
    return 0;
}

static inline void lpr_native_wait_drain(int fd)
{
    if (fd < 16) return;
    struct pacha_ipc_msg message = {0};
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_RECV,
        (uint64_t)(uint32_t)fd,
        (uint64_t)(uintptr_t)&message);
}

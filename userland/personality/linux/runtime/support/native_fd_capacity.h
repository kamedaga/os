#pragma once

#include "syscall.h"
#include <pacha/ipc.h>
#include <pachaos/abi.h>

/* Linux descriptors and native transport/backing descriptors are different
 * tables. A few dozen Linux sockets may already consume 256 native slots.
 * Grow through the existing API before operations that can receive/create
 * capabilities, never by replaying an IPC operation after it has run. */
static inline void lpr_native_fd_prepare(uint64_t nr)
{
    switch (nr) {
    case PACHAOS_SYSCALL_PROCESS_CREATE:
    case PACHAOS_SYSCALL_PROCESS_CLONE:
    case PACHAOS_SYSCALL_THREAD_CREATE:
    case PACHAOS_SYSCALL_FD_DUP:
    case PACHAOS_SYSCALL_EVENTFD_CREATE:
    case PACHAOS_SYSCALL_PIPE_CREATE:
    case PACHAOS_SYSCALL_TIMERFD_CREATE:
    case PACHAOS_SYSCALL_VMO_CREATE:
    case PACHAOS_SYSCALL_VMO_CREATE_PAGE_VIEW:
    case PACHAOS_SYSCALL_IPC_ENDPOINT_CREATE:
    case PACHAOS_SYSCALL_IPC_CHANNEL_CREATE:
    case PACHAOS_SYSCALL_IPC_RECV:
    case PACHAOS_SYSCALL_IPC_CALL:
    case PACHAOS_SYSCALL_IPC_RECV_WAIT:
        break;
    default:
        return;
    }

    struct pacha_fd_table_info info = {0};
    /* FD_TABLE is not in the switch above, so this cannot recurse. The free
     * count is only a snapshot: the original operation still handles races
     * and true exhaustion normally. No lock is held across a blocking IPC. */
    if (lpr_pacha_syscall2(PACHAOS_SYSCALL_FD_TABLE, 0,
            (uint64_t)(uintptr_t)&info) != 0 ||
        info.free_slots >= PACHA_IPC_MAX_TRANSFER_FDS + 2u ||
        info.capacity >= info.maximum || !info.capacity ||
        info.maximum > PACHAOS_FD_TABLE_LIMIT)
        return;
    const uint64_t target = info.capacity > info.maximum / 2
        ? info.maximum : info.capacity * 2;
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_FD_TABLE, target,
        (uint64_t)(uintptr_t)&info);
}

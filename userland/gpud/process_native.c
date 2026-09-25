/* SPDX-License-Identifier: MIT */
#include "process_native.h"
#include <pacha/ipc.h>
#include <pacha/syscall.h>
#include <errno.h>

_Static_assert(sizeof(struct gpud_process_exit) == 32, "PROCESS_WAIT layout");

static int native_status(long result) {
    switch (result) {
    case 0: return 0;
    case PACHA_SYSCALL_ERR_NOT_READY: return -EAGAIN;
    case PACHA_SYSCALL_ERR_INVALID: return -EINVAL;
    case PACHA_SYSCALL_ERR_ALLOC: return -ENOMEM;
    case PACHA_SYSCALL_ERR_MAP: return -EIO;
    case PACHA_SYSCALL_ERR_CLOSED: return -EBADF;
    default: return -EPROTO;
    }
}

static int check_generation(const struct gpud_native_process *process, uint64_t generation) {
    if (!process || !generation) return -EINVAL;
    return process->generation == generation ? 0 : -ESTALE;
}

int gpud_native_process_adopt(struct gpud_native_process *process, int fd,
    uint64_t generation) {
    if (!process || fd < 16 || fd >= PACHA_FD_TABLE_LIMIT || !generation) return -EINVAL;
    if (process->fd) return -EBUSY;
    if (generation <= process->generation) return -ESTALE;
    struct pacha_fd_info info = {0};
    int result = native_status(pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO,
        (uint64_t)fd, (uintptr_t)&info));
    if (result) return result;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_KILL | PACHA_FD_RIGHT_CLOSE;
    if (info.kind != PACHA_FD_KIND_PROCESS || (info.rights & rights) != rights)
        return -EACCES;
    *process = (struct gpud_native_process){.generation = generation, .fd = fd};
    return 0;
}

int gpud_native_process_poll(struct gpud_native_process *process, uint64_t generation) {
    int result = check_generation(process, generation);
    if (result) return result;
    if (!process->fd) return -EBADF;
    if (process->terminal) return 0;
    struct gpud_process_exit observed = {0};
    result = native_status(pacha_syscall2(PACHA_PROCESS_SYSCALL_WAIT,
        (uint64_t)process->fd, (uintptr_t)&observed));
    if (result) return result;
    if ((observed.state != GPUD_PROCESS_EXITED && observed.state != GPUD_PROCESS_KILLED) ||
        observed.reserved) return -EPROTO;
    process->exit = observed;
    process->terminal = 1;
    return 0;
}

int gpud_native_process_terminate(struct gpud_native_process *process,
    uint64_t generation, uint32_t code) {
    int result = gpud_native_process_poll(process, generation);
    if (result != -EAGAIN) return result;
    if (!process->kill_accepted) {
        int killed = native_status(pacha_syscall2(PACHA_PROCESS_SYSCALL_KILL,
            (uint64_t)process->fd, code));
        if (!killed) process->kill_accepted = 1;
        /* KILL can race normal exit, including returning INVALID after the
         * process became terminal. Keep the original error unless WAIT proves
         * that the target really exited. Failed copyout is not that proof. */
        result = gpud_native_process_poll(process, generation);
        if (!result) return 0;
        return killed ? killed : result;
    }
    return -EAGAIN;
}

int gpud_native_process_release(struct gpud_native_process *process, uint64_t generation) {
    int result = check_generation(process, generation);
    if (result) return result;
    if (!process->fd) return 0;
    if (!process->terminal) return -EBUSY;
    result = native_status(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, (uint64_t)process->fd));
    if (result) return result;
    process->fd = 0;
    return 0;
}

#include "lpr_linux_syscall.h"
#include "lpr_pacha_syscall.h"
#include <pachaos/abi.h>

int64_t lpr_dispatch_syscall(uint64_t nr,
                             uint64_t a0,
                             uint64_t a1,
                             uint64_t a2,
                             uint64_t a3,
                             uint64_t a4,
                             uint64_t a5) {
    (void)a3;
    (void)a4;
    (void)a5;
    switch (nr) {
    case LPR_LINUX_SYS_GETPID:
        return lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID);
    case LPR_LINUX_SYS_GETTID:
        return lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID);
    case LPR_LINUX_SYS_WRITE:
        return lpr_pacha_syscall3(PACHAOS_SYSCALL_FD_WRITE, a0, a1, a2);
    case LPR_LINUX_SYS_EXIT:
    case LPR_LINUX_SYS_EXIT_GROUP:
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, a0);
        for (;;) {
        }
    default:
        return -LPR_LINUX_ENOSYS;
    }
}

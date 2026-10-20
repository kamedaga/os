/* Host-only protocol adapter. Production LPR packs these struct API arguments
 * into registers in support/syscall.c; this adapter never executes SYSCALL. */
#include "../userland/personality/linux/runtime/support/syscall.h"
#include <pacha/abi.h>
#include <stdlib.h>
static int64_t test_lpr_ipc_noncall(uint64_t nr, uint64_t fd, uint64_t send,
    uint64_t receive, uint64_t timeout, uint64_t flags, int *committed) {
    if (committed) *committed = 0;
    if (nr == PACHA_IPC_SYSCALL_SEND) return lpr_pacha_syscall2(nr, fd, send);
    if (nr == PACHA_IPC_SYSCALL_RECV) return lpr_pacha_syscall2(nr, fd, receive);
    if (nr == PACHA_IPC_SYSCALL_RECV_WAIT) return lpr_pacha_syscall4(nr, fd, receive, timeout, flags);
    abort();
}

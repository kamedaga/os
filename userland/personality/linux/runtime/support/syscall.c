#include "pacha/poll.h"
#include "syscall.h"
#include "native_fd_capacity.h"

int64_t lpr_pacha_syscall0(uint64_t nr)
{
    lpr_native_fd_prepare(nr, 0);
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr)
                     : "rcx", "r11", "memory");
    return ret;
}

int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t a0)
{
    lpr_native_fd_prepare(nr, 0);
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0)
                     : "rcx", "r11", "memory");
    return ret;
}

int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    if (nr == 51 || nr == 52) return pacha_poll_registers(nr, a0, a1, 0, 0);
    lpr_native_fd_prepare(nr, a1);
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1)
                     : "rcx", "r11", "memory");
    return ret;
}

int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2)
{
    if (nr == 51 || nr == 52) return pacha_poll_registers(nr, a0, a1, a2, 0);
    lpr_native_fd_prepare(nr, a1);
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}

int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    if (nr == 51 || nr == 52) return pacha_poll_registers(nr, a0, a1, a2, a3);
    lpr_native_fd_prepare(nr, a1);
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

int64_t lpr_pacha_syscall5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    if (nr == 51 || nr == 52) return pacha_poll_registers(nr, a0, a1, a2, a3);
    lpr_native_fd_prepare(nr, a1);
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = a4;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}

int64_t lpr_pacha_syscall6(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    if (nr == 51 || nr == 52) return pacha_poll_registers(nr, a0, a1, a2, a3);
    lpr_native_fd_prepare(nr, a1);
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = a4;
    register uint64_t r9 __asm__("r9") = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

/* Named callee-saved register operands make the compiler preserve RBX/R12/R13/R15
 * around the syscall while treating every payload register as an output. */
static long lpr_ipc_registers(uint64_t nr, uint64_t fd, uint64_t flags,
    uint64_t timeout, uint64_t words[4], struct pacha_ipc_extension *extension,
    int *committed) {
    register uint64_t r10 __asm__("r10") = words[0];
    register uint64_t r8 __asm__("r8") = words[1];
    register uint64_t r9 __asm__("r9") = words[2];
    register uint64_t r12 __asm__("r12") = words[3];
    /* Counts above the native limit are rejected before packing: truncating
     * them could turn an invalid FD transfer into a successful empty one. */
    if (extension && (extension->send_count > 19 || extension->receive_capacity > 19)) {
        extension->received_count = 0;
        if (committed) *committed = 0;
        return 1; /* native INVALID */
    }
    register uint64_t r13 __asm__("r13") = extension ? extension->send_array : 0;
    register uint64_t rbx __asm__("rbx") = extension ? extension->receive_array : 0;
    register uint64_t r15 __asm__("r15") = extension ?
        extension->send_count | (extension->receive_capacity << 32) : 0;
    __asm__ volatile("syscall"
        : "+a"(nr), "+d"(timeout), "+r"(r10), "+r"(r8), "+r"(r9), "+r"(r12), "+r"(r15)
        : "D"(fd), "S"(flags), "r"(r13), "r"(rbx)
        : "rcx", "r11", "memory");
    words[0] = r10; words[1] = r8; words[2] = r9; words[3] = r12;
    if (extension) extension->received_count = nr == 0 ? r15 : 0;
    if (committed) *committed = (int)timeout;
    return (long)nr;
}
static inline long lpr_pack_ipc(uint64_t nr, uint64_t fd,
    const struct pacha_ipc_msg *send, struct pacha_ipc_msg *receive,
    uint64_t timeout, uint64_t flags, int *committed) {
    uint64_t words[4] = {0, 0, 0, 0};
    struct pacha_ipc_extension extension = {0};
    if (send) {
        words[0] = send->word0; words[1] = send->word1;
        words[2] = send->word2; words[3] = send->word3;
        extension.send_array = (uintptr_t)send->fds;
        extension.send_count = send->fd_count;
    }
    if (receive) {
        extension.receive_array = (uintptr_t)receive->fds;
        extension.receive_capacity = receive->fd_capacity;
    }
    long status = lpr_ipc_registers(nr, fd, flags, timeout, words,
        (extension.send_count || extension.receive_capacity) ? &extension : 0, committed);
    if (receive) {
        receive->word0 = words[0]; receive->word1 = words[1];
        receive->word2 = words[2]; receive->word3 = words[3];
        receive->fd_count = extension.received_count;
    }
    return status;
}


int64_t lpr_ipc_message(uint64_t nr, uint64_t fd, uint64_t send_va,
    uint64_t receive_va, uint64_t timeout, uint64_t flags, int *committed) {
    /* Capacity preparation is userspace policy, before the machine boundary. */
    if (receive_va) lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_RECV, receive_va);
    return lpr_pack_ipc(nr, fd, (const void *)(uintptr_t)send_va,
        (void *)(uintptr_t)receive_va, timeout, flags, committed);
}

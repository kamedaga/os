#include "pacha/poll.h"
#include "pacha/syscall.h"
#include "pacha/abi.h"
#include "pacha/native_syscall_retry.h"

long pacha_clock_read(uint64_t clock_id, uint64_t *nanoseconds) {
    const struct pacha_native_result result = pacha_native_syscall_retry6(
        PACHA_RUNTIME_SYSCALL_CLOCK_READ, clock_id, 0, 0, 0, 0, 0);
    *nanoseconds = result.rdx;
    return result.status;
}

long pacha_futex_wake_wait(uint64_t wake_va, uint64_t wait_va,
    uint32_t expected, uint64_t timeout_ticks, uint64_t flags, uint64_t *wake_count) {
    const struct pacha_native_result result = pacha_native_syscall_retry6(
        PACHA_RUNTIME_SYSCALL_FUTEX_WAKE_WAIT, wake_va, wait_va, 0,
        expected, timeout_ticks, flags);
    *wake_count = result.rdx;
    return result.status;
}

long pacha_pager_attach(uint64_t port_fd, uint64_t vmo_fd, uint64_t key,
    uint64_t flags, uint64_t *generation) {
    const struct pacha_native_result result = pacha_native_syscall_retry6(
        PACHA_PAGER_SYSCALL_ATTACH, port_fd, vmo_fd, key, flags, 0, 0);
    if (result.status == 0 && generation) *generation = result.rdx;
    return result.status;
}

long pacha_syscall0(uint64_t nr) {
    return pacha_native_syscall_retry6(nr, 0, 0, 0, 0, 0, 0).status;
}
long pacha_syscall1(uint64_t nr, uint64_t a0) {
    return pacha_native_syscall_retry6(nr, a0, 0, 0, 0, 0, 0).status;
}
long pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1) {
    return pacha_poll_registers(nr, a0, a1, 0, 0);
}
long pacha_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    return pacha_poll_registers(nr, a0, a1, a2, 0);
}
long pacha_syscall4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    return pacha_poll_registers(nr, a0, a1, a2, a3);
}
long pacha_syscall5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    if (nr == PACHA_FD_SYSCALL_POLL || nr == PACHA_FD_SYSCALL_WAIT_MANY) return pacha_poll_registers(nr, a0, a1, a2, a3);
    return pacha_native_syscall_retry6(nr, a0, a1, a2, a3, a4, 0).status;
}
long pacha_syscall6(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    if (nr == PACHA_FD_SYSCALL_POLL || nr == PACHA_FD_SYSCALL_WAIT_MANY) return pacha_poll_registers(nr, a0, a1, a2, a3);
    return pacha_native_syscall_retry6(nr, a0, a1, a2, a3, a4, a5).status;
}

int pacha_status_to_int(long status) {
    return status == 0 ? 0 : -(int)status;
}

pacha_fd_t pacha_fd_result(long result) {
    if (result >= 16 && (uint64_t)result < PACHA_THREAD_SELF_FD) return result;
    /* Kernel status is positive 1..6; neither reserved FD is an object. */
    return result > 0 && result <= 6 ? -result : -PACHA_SYSCALL_ERR_INVALID;
}



/* Named callee-saved register operands make the compiler preserve RBX/R12/R13/R15
 * around the syscall while treating every payload register as an output. */
long pacha_ipc_registers(uint64_t nr, uint64_t fd, uint64_t flags,
    uint64_t timeout, uint64_t words[4], struct pacha_ipc_extension *extension,
    int *committed) {
    /* Counts above the native limit are rejected before packing: truncating
     * them could turn an invalid FD transfer into a successful empty one. */
    if (extension && (extension->send_count > 19 || extension->receive_capacity > 19)) {
        extension->received_count = 0;
        if (committed) *committed = 0;
        return 1; /* native INVALID */
    }
    for (;;) {
        uint64_t status = nr, result = timeout;
        register uint64_t r10 __asm__("r10") = words[0];
        register uint64_t r8 __asm__("r8") = words[1];
        register uint64_t r9 __asm__("r9") = words[2];
        register uint64_t r12 __asm__("r12") = words[3];
        register uint64_t r13 __asm__("r13") = extension ? extension->send_array : 0;
        register uint64_t rbx __asm__("rbx") = extension ? extension->receive_array : 0;
        register uint64_t r15 __asm__("r15") = extension ?
            extension->send_count | (extension->receive_capacity << 32) : 0;
        __asm__ volatile("syscall"
            : "+a"(status), "+d"(result), "+r"(r10), "+r"(r8), "+r"(r9), "+r"(r12), "+r"(r15)
            : "D"(fd), "S"(flags), "r"(r13), "r"(rbx)
            : "rcx", "r11", "memory");
        if ((long)status == PACHA_SYSCALL_ERR_PAGER_ABSENT) {
            pacha_pager_retry_read(result);
            continue;
        }
        words[0] = r10; words[1] = r8; words[2] = r9; words[3] = r12;
        if (extension) extension->received_count = status == 0 ? r15 : 0;
        if (committed) *committed = (int)result;
        return (long)status;
    }
}

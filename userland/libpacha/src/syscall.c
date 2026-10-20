#include "pacha/poll.h"
#include "pacha/syscall.h"
#include "pacha/abi.h"

long pacha_clock_read(uint64_t clock_id, uint64_t *nanoseconds) {
    uint64_t status, value;
    __asm__ volatile("syscall"
        : "=a"(status), "=d"(value)
        : "a"((uint64_t)PACHA_RUNTIME_SYSCALL_CLOCK_READ), "D"(clock_id)
        : "rcx", "r11", "memory");
    *nanoseconds = value;
    return (long)status;
}

long pacha_futex_wake_wait(uint64_t wake_va, uint64_t wait_va,
    uint32_t expected, uint64_t timeout_ticks, uint64_t flags, uint64_t *wake_count) {
    uint64_t status, count;
    register uint64_t r10 __asm__("r10") = expected;
    register uint64_t r8 __asm__("r8") = timeout_ticks;
    register uint64_t r9 __asm__("r9") = flags;
    __asm__ volatile("syscall"
        : "=a"(status), "=d"(count)
        : "a"((uint64_t)PACHA_RUNTIME_SYSCALL_FUTEX_WAKE_WAIT),
          "D"(wake_va), "S"(wait_va), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    *wake_count = count;
    return (long)status;
}

long pacha_syscall0(uint64_t nr) {
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr)
        : "rcx", "r11", "memory");
    return (long)ret;
}

long pacha_syscall1(uint64_t nr, uint64_t a0) {
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0)
        : "rcx", "r11", "memory");
    return (long)ret;
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
    if (nr == 51 || nr == 52) return pacha_poll_registers(nr, a0, a1, a2, a3);
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = a4;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (long)ret;
}

long pacha_syscall6(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    if (nr == 51 || nr == 52) return pacha_poll_registers(nr, a0, a1, a2, a3);
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = a4;
    register uint64_t r9 __asm__("r9") = a5;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return (long)ret;
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

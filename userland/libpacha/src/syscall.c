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
    uint32_t expected, uint64_t timeout_ticks, uint64_t *wake_count) {
    uint64_t status, count;
    register uint64_t r10 __asm__("r10") = expected;
    register uint64_t r8 __asm__("r8") = timeout_ticks;
    __asm__ volatile("syscall"
        : "=a"(status), "=d"(count)
        : "a"((uint64_t)PACHA_RUNTIME_SYSCALL_FUTEX_WAKE_WAIT),
          "D"(wake_va), "S"(wait_va), "r"(r10), "r"(r8)
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
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "r11", "memory");
    return (long)ret;
}

long pacha_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory");
    return (long)ret;
}

long pacha_syscall4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
        : "rcx", "r11", "memory");
    return (long)ret;
}

long pacha_syscall5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
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

int pacha_fd_result_to_int(long result) {
    return result >= 16 ? (int)result : -(int)result;
}

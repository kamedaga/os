#ifndef PACHA_NATIVE_SYSCALL_RETRY_H
#define PACHA_NATIVE_SYSCALL_RETRY_H

#include <stdint.h>
#include "abi.h"

struct pacha_native_result {
    long status;
    uint64_t rdx;
};

/* The kernel has published demand before returning this VA. A volatile read
 * enters the ordinary user #PF path and waits without writing a private COW
 * page. The caller reissues the exact original register arguments afterward. */
static inline void pacha_pager_retry_read(uint64_t va)
{
    volatile const uint8_t *page = (volatile const uint8_t *)(uintptr_t)va;
    uint8_t value = *page;
    __asm__ volatile("" : : "r"(value) : "memory");
}

static inline struct pacha_native_result pacha_native_syscall_retry6(
    uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4, uint64_t a5)
{
    for (;;) {
#ifdef PACHA_NATIVE_SYSCALL_TEST_INVOKE
        struct pacha_native_result test = PACHA_NATIVE_SYSCALL_TEST_INVOKE(
            nr, a0, a1, a2, a3, a4, a5);
        if (test.status != PACHA_SYSCALL_ERR_PAGER_ABSENT) return test;
        pacha_pager_retry_read(test.rdx);
#else
        uint64_t status = nr, arg0 = a0, arg1 = a1, arg2 = a2;
        register uint64_t r10 __asm__("r10") = a3;
        register uint64_t r8 __asm__("r8") = a4;
        register uint64_t r9 __asm__("r9") = a5;
        __asm__ volatile("syscall"
            : "+a"(status), "+D"(arg0), "+S"(arg1), "+d"(arg2),
              "+r"(r10), "+r"(r8), "+r"(r9)
            :
            : "rcx", "r11", "memory");
        if ((long)status != PACHA_SYSCALL_ERR_PAGER_ABSENT)
            return (struct pacha_native_result){ (long)status, arg2 };
        pacha_pager_retry_read(arg2);
#endif
    }
}

#endif

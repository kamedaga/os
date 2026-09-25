/* SPDX-License-Identifier: GPL-2.0-only */
/* Extra machine-entry coverage around the unchanged common ELF workload. */
#include <errno.h>
#include <stdint.h>
#include <sys/syscall.h>
#include "../kobox2/linux-sandbox/kobox/boot/exec_gate.h"

int __real_kobox_elf_client_test(unsigned long *stack);

static long linux_call(uint64_t number, uint64_t a0, uint64_t a1,
    uint64_t a2, uint64_t a3)
{
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = 0;
    register uint64_t r9 __asm__("r9") = 0;

    __asm__ volatile("syscall" : "+a"(number) :
        "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9) :
        "rcx", "r11", "memory");
    return number;
}

int __wrap_kobox_elf_client_test(unsigned long *stack)
{
    /* These numbers include the shim, unmapped low addresses, negative and
     * noncanonical CALL targets. Linux must receive the original integer. */
    const uint64_t numbers[] = {512, 4095, 4096, UINT64_MAX,
        UINT64_C(0x80000000ffff), UINT64_C(0x40000000) | SYS_getpid};

    for (unsigned int index = 0; index < sizeof(numbers) / sizeof(numbers[0]); ++index) {
        long result = linux_call(numbers[index], 0, 0, 0, 0);

        if (result != -ENOSYS) {
            struct kobox_exec_failure failure = {
                .pid = linux_call(SYS_getpid, 0, 0, 0, 0),
                .line = __LINE__, .error = result,
            };

            linux_call(SYS_pwrite64, 19, (uintptr_t)&failure,
                sizeof(failure), KOBOX_EXEC_FAILURE_OFFSET);
            return 1;
        }
    }
    return __real_kobox_elf_client_test(stack);
}

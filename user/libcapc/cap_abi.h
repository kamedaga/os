#ifndef CAP_ABILITY_OS_CAP_ABI_H
#define CAP_ABILITY_OS_CAP_ABI_H

/*
 * CapabilityOS syscall ABI (current path)
 *
 * Instruction:
 *   int $0x80
 *
 * Register contract:
 *   rax: syscall number
 *   rdi, rsi, rdx, r10, r8, r9: up to 6 args
 *   rax: return status (0 = success, non-zero = kernel error code)
 *
 * Clobbers:
 *   rcx, r11, memory
 */

#define CAP_SYSCALL_INT_VECTOR 0x80

#endif /* CAP_ABILITY_OS_CAP_ABI_H */

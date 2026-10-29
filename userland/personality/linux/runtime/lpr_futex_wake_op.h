#ifndef LPR_FUTEX_WAKE_OP_H
#define LPR_FUTEX_WAKE_OP_H
#include <stdint.h>
#include <pacha/abi.h>
#include "support/syscall.h"

/* Linux encodings and quirks belong here; the native operation owns the
 * atomic word update and both wake selections under its existing queue lock. */
static int64_t lpr_futex_wake_op(uint64_t first, uint64_t second,
    uint64_t first_limit, uint64_t second_limit, uint64_t encoded)
{
    if ((first | second) & 3u) return -LPR_LINUX_EINVAL;
    if (!first || !second) return -LPR_LINUX_EFAULT;
    const uint32_t raw = (uint32_t)encoded;
    const unsigned operation = (raw >> 28) & 7u;
    const unsigned comparison = (raw >> 24) & 15u;
    if (operation > 4) return -LPR_LINUX_ENOSYS;
    const int32_t argument = (int32_t)(raw << 8) >> 20;
    const int32_t compare_argument = (int32_t)(raw << 20) >> 20;
    static const uint32_t operations[] = {PACHA_FUTEX_OP_SET, PACHA_FUTEX_OP_ADD,
        PACHA_FUTEX_OP_OR, PACHA_FUTEX_OP_AND_NOT, PACHA_FUTEX_OP_XOR};
    static const uint32_t comparisons[] = {PACHA_FUTEX_CMP_EQ, PACHA_FUTEX_CMP_NE,
        PACHA_FUTEX_CMP_LT, PACHA_FUTEX_CMP_LE, PACHA_FUTEX_CMP_GT, PACHA_FUTEX_CMP_GE};
    const struct pacha_futex_wake_op update = {
        .operation = operations[operation],
        /* Linux masks out-of-range shift counts, including negative counts. */
        .operand = raw & 0x80000000u ? 1u << ((uint32_t)argument & 31u) : (uint32_t)argument,
        .comparison = comparison < 6 ? comparisons[comparison] : PACHA_FUTEX_CMP_EQ,
        .compare_operand = (uint32_t)compare_argument,
    };
    /* The Linux WAKE_OP queue loops visit at least one waiter even for a
     * nonpositive signed limit. An invalid comparison instead commits only
     * the atomic update and reports ENOSYS before either queue is visited. */
    const uint64_t first_count = comparison < 6 ? ((int32_t)first_limit > 0 ? (uint32_t)first_limit : 1u) : 0;
    const uint64_t second_count = comparison < 6 ? ((int32_t)second_limit > 0 ? (uint32_t)second_limit : 1u) : 0;
    const int64_t result = lpr_pacha_syscall5(PACHA_RUNTIME_SYSCALL_FUTEX_WAKE_OP,
        first, second, first_count, second_count, (uintptr_t)&update);
    if (result == -PACHA_SYSCALL_ERR_MAP) return -LPR_LINUX_EFAULT;
    if (result < 0) return lpr_pacha_status_to_errno(result);
    return comparison < 6 ? result : -LPR_LINUX_ENOSYS;
}
#endif

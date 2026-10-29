#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <stdint.h>
#include <stdio.h>
#define LPR_LINUX_EINVAL EINVAL
#define LPR_LINUX_EFAULT EFAULT
#define LPR_LINUX_ENOSYS ENOSYS
static int64_t lpr_pacha_status_to_errno(int64_t value) { return value == -2 ? -EAGAIN : -EINVAL; }
#include "../userland/personality/linux/runtime/lpr_futex_wake_op.h"
static struct pacha_futex_wake_op captured;
static uint64_t counts[2];
static unsigned calls;
static int64_t answer;
int64_t lpr_pacha_syscall5(uint64_t nr, uint64_t first, uint64_t second,
    uint64_t a, uint64_t b, uint64_t descriptor)
{
    assert(nr == PACHA_RUNTIME_SYSCALL_FUTEX_WAKE_OP && first == 0x1000 && second == 0x2000);
    captured = *(const struct pacha_futex_wake_op *)(uintptr_t)descriptor;
    counts[0] = a; counts[1] = b; ++calls;
    return answer;
}
int main(void)
{
    _Static_assert(sizeof(captured) == 16, "native futex descriptor");
    const uint32_t qt = FUTEX_OP(FUTEX_OP_OR, 0, FUTEX_OP_CMP_NE, 0);
    for (answer = 0; answer <= 6; ++answer)
        assert(lpr_futex_wake_op(0x1000, 0x2000, 1, INT_MAX, qt) == answer);
    assert(captured.operation == PACHA_FUTEX_OP_OR && captured.operand == 0);
    assert(captured.comparison == PACHA_FUTEX_CMP_NE && captured.compare_operand == 0);
    assert(counts[0] == 1 && counts[1] == INT_MAX);
    answer = 0;
    assert(!lpr_futex_wake_op(0x1000, 0x2000, 2, 3,
        FUTEX_OP(FUTEX_OP_ADD, -1, FUTEX_OP_CMP_GE, -2)));
    assert(captured.operation == PACHA_FUTEX_OP_ADD && captured.operand == UINT32_MAX);
    assert(captured.comparison == PACHA_FUTEX_CMP_GE && captured.compare_operand == UINT32_MAX - 1);
    const unsigned native_cmp[] = {PACHA_FUTEX_CMP_EQ, PACHA_FUTEX_CMP_NE, PACHA_FUTEX_CMP_LT,
        PACHA_FUTEX_CMP_LE, PACHA_FUTEX_CMP_GT, PACHA_FUTEX_CMP_GE};
    for (unsigned comparison = 0; comparison < 6; ++comparison) {
        assert(!lpr_futex_wake_op(0x1000, 0x2000, 1, 1,
            FUTEX_OP(FUTEX_OP_SET, 0, comparison, 0)));
        assert(captured.comparison == native_cmp[comparison]);
    }
    assert(!lpr_futex_wake_op(0x1000, 0x2000, 1, 1,
        FUTEX_OP((uint32_t)(FUTEX_OP_SET | FUTEX_OP_OPARG_SHIFT), -1, FUTEX_OP_CMP_EQ, 0)));
    assert(captured.operand == 0x80000000u);
    assert(!lpr_futex_wake_op(0x1000, 0x2000, 0, UINT32_MAX, qt));
    assert(counts[0] == 1 && counts[1] == 1);
    assert(lpr_futex_wake_op(0x1000, 0x2000, 9, 9,
        FUTEX_OP(FUTEX_OP_XOR, 3, 15, 0)) == -ENOSYS);
    assert(captured.operation == PACHA_FUTEX_OP_XOR && counts[0] == 0 && counts[1] == 0);
    unsigned before = calls;
    assert(lpr_futex_wake_op(0x1000, 0x2000, 1, 1, FUTEX_OP(7, 0, 0, 0)) == -ENOSYS);
    assert(lpr_futex_wake_op(0x1001, 0x2000, 1, 1, qt) == -EINVAL);
    assert(lpr_futex_wake_op(0x1000, 0, 1, 1, qt) == -EFAULT);
    assert(calls == before);
    answer = -PACHA_SYSCALL_ERR_MAP;
    assert(lpr_futex_wake_op(0x1000, 0x2000, 1, 1, qt) == -EFAULT);
    answer = -2;
    assert(lpr_futex_wake_op(0x1000, 0x2000, 1, 1, qt) == -EAGAIN);
    puts("Linux FUTEX_WAKE_OP encoding, native count/errors, and invalid-comparison update contract PASS");
}

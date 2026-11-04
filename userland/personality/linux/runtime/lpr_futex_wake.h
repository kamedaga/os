#ifndef LPR_FUTEX_WAKE_H
#define LPR_FUTEX_WAKE_H
#include <stdint.h>
#include <pachaos/abi.h>
/* Linux FUTEX_WAKE makes waiters runnable; it does not require handing the
 * caller's timeslice to them. Native WAKE(1) may synchronously hand the CPU to the peer. Native
 * REQUEUE(wake=1, move=0) instead publishes the same single wake without moving
 * any waiter/key or inspecting/modifying either userspace word. The native
 * queue lock still serializes this operation with WAIT registration.
 * Restrict this choice to private one-wake; all other Linux operations retain
 * their existing translation. Never substitute wake(2), which can over-wake.
 * Distinct internal targets satisfy REQUEUE's non-alias rule. They are not
 * queues of ours: move=0 guarantees no waiter can be installed there. */
static inline int lpr_futex_wake_only_target(
    uint64_t address, uint64_t count, uint64_t flags, uint64_t *target)
{
    static uint32_t targets[2];
    if (count != 1 || flags != 128 || address == 0 || (address & 3)) return 0;
    *target = (uint64_t)(uintptr_t)&targets[address == (uint64_t)(uintptr_t)&targets[0]];
    return 1;
}
typedef int64_t (*lpr_futex_call2_fn)(uint64_t, uint64_t, uint64_t);
typedef int64_t (*lpr_futex_call4_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static inline int64_t lpr_futex_wake_native(uint64_t address, uint64_t count,
    uint64_t flags, lpr_futex_call2_fn call2, lpr_futex_call4_fn call4)
{
    uint64_t target;
    if (lpr_futex_wake_only_target(address, count, flags, &target)) {
        const int64_t result = call4(PACHAOS_SYSCALL_FUTEX_REQUEUE,
            address, count, 0, target);
        /* PI-key rejection precedes any wake publication in REQUEUE. Only
         * that no-effect error may fall back; other errors must not replay
         * an operation that could have made a waiter runnable. */
        if (result != -(int64_t)PACHAOS_SYSCALL_ERR_INVALID) return result;
    }
    return call2(PACHAOS_SYSCALL_FUTEX_WAKE, address, count);
}
#endif

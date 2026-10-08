#ifndef PACHA_SYSCALL_H
#define PACHA_SYSCALL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

long pacha_syscall0(uint64_t nr);
long pacha_syscall1(uint64_t nr, uint64_t a0);
long pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1);
long pacha_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2);
long pacha_syscall4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
long pacha_syscall5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
long pacha_syscall6(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

/* A value-return clock does not lend a caller pointer to the kernel. */
long pacha_clock_read(uint64_t clock_id, uint64_t *nanoseconds);

/* The count reports outbound wake even when the inbound wait is interrupted.
 * UINT64_MAX means wake was unperformed/failed, not a successful zero wake. */
long pacha_futex_wake_wait(uint64_t wake_va, uint64_t wait_va,
    uint32_t expected, uint64_t timeout_ticks, uint64_t *wake_count);

int pacha_status_to_int(long status);
int pacha_fd_result_to_int(long result);

#ifdef __cplusplus
}
#endif

#endif

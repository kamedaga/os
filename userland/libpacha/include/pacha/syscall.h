#ifndef PACHA_SYSCALL_H
#define PACHA_SYSCALL_H

#include <stdint.h>
#include "pacha/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Userspace packing helper only; no address of this struct crosses the ABI.
 * R13=send array, RBX=receive array, R15=send count|(capacity<<32),
 * R15 returns received count. R12/RBX/R13/R15 follow C callee-save rules. */
struct pacha_ipc_extension {
    uint64_t send_array, send_count;
    uint64_t receive_array, receive_capacity, received_count;
};
long pacha_ipc_registers(uint64_t nr, uint64_t fd, uint64_t flags,
    uint64_t timeout, uint64_t words[4], struct pacha_ipc_extension *extension,
    int *committed);


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
    uint32_t expected, uint64_t timeout_ticks, uint64_t flags, uint64_t *wake_count);

/* PAGER_ATTACH returns an opaque generation in RDX on success. */
long pacha_pager_attach(uint64_t port_fd, uint64_t vmo_fd, uint64_t key,
    uint64_t flags, uint64_t *generation);

int pacha_status_to_int(long status);
pacha_fd_t pacha_fd_result(long result);

#ifdef __cplusplus
}
#endif

#endif

/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_PROCESS_NATIVE_H
#define PACHA_GPUD_PROCESS_NATIVE_H

#include <stdint.h>

/* Native PROCESS_WAIT layout, not Linux wait status or a shared wire type. */
struct gpud_process_exit {
    uint64_t state, code, principal, reserved;
};

enum { GPUD_PROCESS_EXITED = 2, GPUD_PROCESS_KILLED = 3 };

struct gpud_native_process {
    uint64_t generation;
    int fd;
    unsigned int kill_accepted, terminal;
    struct gpud_process_exit exit;
};

/* Single owner, initially zero. Successful adoption takes the process FD;
 * failure leaves it with the caller. No external close/reassignment is allowed.
 * Thread handles and launch mappings remain the launch owner's responsibility.
 * Release retains a generation watermark even if the FD number is reused.
 * This component contains no controller or GPL dependencies. */
int gpud_native_process_adopt(struct gpud_native_process *process, int fd,
    uint64_t generation);
/* EAGAIN means no terminal observation. Every other error also retains the FD
 * and must NOT be interpreted as death. Only successful PROCESS_WAIT with a
 * terminal status publishes exit; HUP, ESRCH and timeouts are not proof. */
int gpud_native_process_poll(struct gpud_native_process *process, uint64_t generation);
/* Nonblocking KILL followed by WAIT. An accepted KILL is not completion.
 * A concurrent normal exit is resolved by WAIT, never by swallowing KILL errors. */
int gpud_native_process_terminate(struct gpud_native_process *process,
    uint64_t generation, uint32_t code);
/* Must be called only at REAP, after resource revoke/reset. Refuses a live
 * process. Failed closes retain ownership for retry; success is idempotent. */
int gpud_native_process_release(struct gpud_native_process *process, uint64_t generation);

#endif

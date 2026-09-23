#pragma once
#include "../lpr_filed_internal.h"

#ifndef LPR_UNIX_DIAG
#define LPR_UNIX_DIAG 3
#endif
#ifndef LPR_UNIX_DIAG_MAX_RECORDS
#define LPR_UNIX_DIAG_MAX_RECORDS 64u
#endif

/* Default: errors only, bounded per translation unit/process. Success and
 * ordinary retries return before counters, formatting or syscalls. Set
 * LPR_UNIX_DIAG=0 to disable; verbose modes require an explicit build flag.
 * Never use the socket being diagnosed for logging. Values are hexadecimal,
 * headed by the Linux PID. */
static inline void lpr_unix_diag(uint64_t op, uint64_t socket, uint64_t a, uint64_t b, uint64_t c)
{
#if defined(LPR_UNIX_DIAG) && LPR_UNIX_DIAG
#if LPR_UNIX_DIAG > 1
#ifdef LPR_UNIX_DIAG_MIN_PID
    if (lpr_linux_current_pid < LPR_UNIX_DIAG_MIN_PID) return;
#endif
    if (op != 'R' && op != 'W' && op != 'N' && op != 'E' && op != 8) return;
    if (a == UINT64_MAX - 10) return; /* Repeated EAGAIN is not a data event. */
#if LPR_UNIX_DIAG > 2
    if ((op == 'R' || op == 'W' || op == 8) &&
        ((int64_t)a >= 0 || (int64_t)a == -4 ||
         (op == 8 && ((int64_t)a == -115 || (int64_t)a == -114)))) return;
#endif
#endif
    static unsigned records;
    if (__atomic_load_n(&records, __ATOMIC_RELAXED) >= LPR_UNIX_DIAG_MAX_RECORDS ||
        __atomic_fetch_add(&records, 1, __ATOMIC_RELAXED) >= LPR_UNIX_DIAG_MAX_RECORDS) return;
    const uint64_t values[] = { lpr_linux_current_pid, op, socket, a, b, c };
    char line[128] = "[unix]";
    unsigned n = 6;
    for (unsigned i = 0; i < 6; i++) {
        line[n++] = ' ';
        for (int shift = 60; shift >= 0; shift -= 4)
            line[n++] = "0123456789abcdef"[(values[i] >> shift) & 15];
    }
    line[n++] = '\n';
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_LOG, (uint64_t)(uintptr_t)line, n);
#else
    (void)op; (void)socket; (void)a; (void)b; (void)c;
#endif
}

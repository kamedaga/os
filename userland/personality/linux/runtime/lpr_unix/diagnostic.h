#pragma once
#include "../lpr_filed_internal.h"

/* Opt-in, bounded native-serial diagnostics: never use the socket being
 * diagnosed for logging. Values are hexadecimal, headed by the Linux PID. */
static inline void lpr_unix_diag(uint64_t op, uint64_t socket, uint64_t a, uint64_t b, uint64_t c)
{
#if defined(LPR_UNIX_DIAG) && LPR_UNIX_DIAG
#if LPR_UNIX_DIAG > 1
#ifdef LPR_UNIX_DIAG_MIN_PID
    if (lpr_linux_current_pid < LPR_UNIX_DIAG_MIN_PID) return;
#endif
    if (op != 'R' && op != 'W' && op != 'N' && op != 'E' && op != 8) return;
    if (a == UINT64_MAX - 10) return; /* Repeated EAGAIN is not a data event. */
#endif
    static unsigned records;
    if (__atomic_fetch_add(&records, 1, __ATOMIC_RELAXED) >= 20000) return;
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

#pragma once
#if defined(LPR_BROWSER_DIAG) && LPR_BROWSER_DIAG
static inline void lpr_browser_diag(const char *stage, uint64_t a, uint64_t b, uint64_t c)
{
    char line[192];
    unsigned n = 0;
    const char *prefix = "[lpr-browser] ";
    while (*prefix) line[n++] = *prefix++;
    while (*stage && n < 100) line[n++] = *stage++;
    uint64_t values[] = {(uint64_t)lpr_linux_current_pid,
        (uint64_t)lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID), a, b, c};
    for (unsigned i = 0; i < 5; ++i) {
        line[n++] = ' ';
        for (int shift = 60; shift >= 0; shift -= 4)
            line[n++] = "0123456789abcdef"[(values[i] >> shift) & 15];
    }
    line[n++] = '\n';
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_LOG, (uint64_t)(uintptr_t)line, n);
}
#else
#define lpr_browser_diag(stage, a, b, c) ((void)0)
#endif

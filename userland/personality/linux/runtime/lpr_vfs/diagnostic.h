#ifndef LPR_FS_DIAGNOSTIC_H
#define LPR_FS_DIAGNOSTIC_H

/* Queue failures cannot be diagnosed through the filesystem being tested.
 * Explicit probe builds use native LOG, preserving the original errno. */
static inline void lpr_fs_diag(uint64_t stage, int64_t status, uint64_t value)
{
#ifdef PH_FS_CHANNEL_DIAGNOSTICS
    const uint64_t values[] = {lpr_linux_current_pid, stage, (uint64_t)status, value};
    char line[80] = "[lpr-fs]";
    unsigned n = 8;
    for (unsigned i = 0; i < 4; ++i) {
        line[n++] = ' ';
        for (int shift = 60; shift >= 0; shift -= 4)
            line[n++] = "0123456789abcdef"[(values[i] >> shift) & 15];
    }
    line[n++] = '\n';
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_LOG, (uint64_t)(uintptr_t)line, n);
#else
    (void)stage; (void)status; (void)value;
#endif
}
#endif

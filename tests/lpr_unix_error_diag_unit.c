#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* Exercise the real formatter/filter with a native-log stub, not the IPC stack. */
#define LPR_FILED_INTERNAL_H
#ifndef LPR_UNIX_DIAG
#define TEST_DEFAULT_ERROR_MODE 1
#endif
#ifndef TEST_DEFAULT_CAP
#define LPR_UNIX_DIAG_MAX_RECORDS 4
#endif
#define PACHAOS_SYSCALL_LOG 42
static const uint64_t lpr_linux_current_pid = 7;
static unsigned logs;
static int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t text, uint64_t length)
{
    assert(nr == PACHAOS_SYSCALL_LOG && length == 109);
    assert(!memcmp((void *)(uintptr_t)text, "[unix] ", 7));
    ++logs;
    return 0;
}
#include "../userland/personality/linux/runtime/lpr_unix/diagnostic.h"
#ifdef TEST_DEFAULT_ERROR_MODE
_Static_assert(LPR_UNIX_DIAG == 3, "normal builds must log only errors");
#endif
#ifdef TEST_DEFAULT_CAP
_Static_assert(LPR_UNIX_DIAG_MAX_RECORDS == 64, "normal error cap must stay small");
#endif
int main(void)
{
    (void)lpr_linux_current_pid;
    (void)lpr_pacha_syscall2;
    for (unsigned i = 0; i < 100000; ++i) {
        lpr_unix_diag('R', 1, 100, 0, 0);
        lpr_unix_diag('W', 1, 0, 0, 0);
        lpr_unix_diag('R', 1, (uint64_t)-11, 0, 0);
        lpr_unix_diag('W', 1, (uint64_t)-4, 0, 0);
        lpr_unix_diag(8, 1, 0, 0, 0);
        lpr_unix_diag(8, 1, (uint64_t)-115, 0, 0);
        lpr_unix_diag(8, 1, (uint64_t)-114, 0, 0);
    }
    assert(!logs);
    lpr_unix_diag('R', 1, (uint64_t)-5, 0, 0);
    lpr_unix_diag(8, 1, (uint64_t)-5, 0, 0);
    lpr_unix_diag('E', 1, 109, 0, 1); /* truncated rights */
    lpr_unix_diag('N', 1, (uint64_t)-5, 0, 0);
#if LPR_UNIX_DIAG
    assert(logs == 4);
#else
    assert(logs == 0);
#endif
    for (unsigned i = 0; i < 100000; ++i) lpr_unix_diag('E', 1, 109, 0, 1);
#if LPR_UNIX_DIAG
    assert(logs == LPR_UNIX_DIAG_MAX_RECORDS);
#else
    assert(logs == 0);
#endif
    puts("unix error diagnostics: quiet success/retry/connect and bounded errors PASS");
    return 0;
}

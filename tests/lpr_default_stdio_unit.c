#include "../userland/personality/linux/runtime/lpr_tty/runtime.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

lpr_state_t lpr_state;
static int active[16], opened, duplicated, closed, have_manifest;
static const char *expected_path;
static uint64_t expected_flags;
static unsigned log_calls, snapshot_calls;
static int64_t log_result;
static char last_log[257];
void lpr_boot_diagnostic_other_tu(void);

int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t address, uint64_t bytes)
{
    assert(nr == PACHAOS_SYSCALL_LOG && bytes > 0 && bytes <= 256);
    memcpy(last_log, (const void *)(uintptr_t)address, bytes);
    last_log[bytes] = 0;
    assert(last_log[bytes - 1] == '\n');
    ++log_calls;
    return log_result;
}
int lpr_fd_table_pin(lpr_fd_table_t *table, lpr_linux_fd_t fd, lpr_fd_pin_t *pin)
{
    (void)table;
    assert(fd < 3);
    ++snapshot_calls;
    if (!active[fd]) return -1;
    *pin = (lpr_fd_pin_t){.fd = fd, .ops_id = LPR_FD_OPS_TTY,
        .effective_rights = LPR_FD_RIGHT_READ | LPR_FD_RIGHT_WRITE |
            LPR_FD_RIGHT_IOCTL | LPR_FD_RIGHT_DUP | LPR_FD_RIGHT_STAT};
    return 0;
}
void lpr_fd_unpin(const lpr_fd_pin_t *pin) { (void)pin; }

int lpr_load_manifest(void) { return have_manifest; }
int lpr_fd_linux_visible_active(uint64_t fd)
{
    assert(fd < 16);
    return active[fd];
}
int64_t lpr_linux_openat(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t mode, unsigned *wait_not_ready __attribute__((unused)))
{
    assert(dirfd == (uint64_t)LPR_LINUX_AT_FDCWD && mode == 0);
    assert(!strcmp((const char *)(uintptr_t)path, expected_path));
    assert(flags == expected_flags);
    opened++;
    for (unsigned fd = 0; fd < 16; fd++) {
        if (!active[fd]) { active[fd] = 1; return fd; }
    }
    assert(!"unexpected full table");
    return -1;
}
int64_t lpr_linux_dup2(uint64_t old_fd, uint64_t new_fd, uint64_t flags)
{
    assert(old_fd < 16 && active[old_fd] && new_fd < 16 && flags == 0);
    active[new_fd] = 1;
    duplicated++;
    return new_fd;
}
int64_t lpr_linux_close(uint64_t fd)
{
    assert(fd < 16 && active[fd]);
    active[fd] = 0;
    closed++;
    return 0;
}
static void reset(uint64_t flags, const char *ctty)
{
    memset(&lpr_state, 0, sizeof(lpr_state));
    memset(active, 0, sizeof(active));
    opened = duplicated = closed = 0;
    have_manifest = 1;
    lpr_process_manifest.linux_pid = 1;
    lpr_process_manifest.flags = flags;
    strcpy(lpr_process_manifest.ctty, ctty);
    expected_path = ctty[0] ? ctty : "/dev/null";
    expected_flags = LPR_LINUX_O_RDWR |
        ((flags & LPR_MANIFEST_FLAG_STDIO_NOCTTY) ? LPR_LINUX_O_NOCTTY : 0);
    log_calls = snapshot_calls = 0;
    log_result = 0;
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
    memset(&lpr_boot_diagnostic_state, 0, sizeof(lpr_boot_diagnostic_state));
#endif
}
int main(void)
{
    reset(LPR_MANIFEST_FLAG_DEFAULT_STDIO, "");
    lpr_linux_ensure_default_stdio(0);
    assert(active[0] && active[1] && active[2]);
    assert(opened == 1 && duplicated == 2 && !closed);
    /* Closing stdin for an async child must not trigger another bootstrap. */
    active[0] = 0;
    lpr_linux_ensure_default_stdio(0);
    assert(!active[0] && opened == 1);

    reset(LPR_MANIFEST_FLAG_DEFAULT_STDIO | LPR_MANIFEST_FLAG_STDIO_NOCTTY, "/dev/pts/0");
    active[0] = active[2] = 1;
    lpr_linux_ensure_default_stdio(0);
    assert(active[0] && active[1] && active[2] && opened == 1 && !duplicated);

    reset(LPR_MANIFEST_FLAG_DEFAULT_STDIO, "");
    active[0] = active[1] = active[2] = 1;
    lpr_linux_ensure_default_stdio(0);
    assert(!opened);

    /* An ordinary exec inherits its table, including intentionally absent
     * stdio; it must not receive the service-launch defaults. */
    reset(0, "");
    lpr_linux_ensure_default_stdio(0);
    assert(!opened && !active[0] && !active[1] && !active[2]);
    reset(LPR_MANIFEST_FLAG_DEFAULT_STDIO, "");
    have_manifest = 0;
    lpr_linux_ensure_default_stdio(0);
    assert(!opened);

#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
    reset(LPR_MANIFEST_FLAG_DEFAULT_STDIO, "/dev/hvc0");
    log_result = 19;
    lpr_linux_ensure_default_stdio(0);
    assert(active[0] && active[1] && active[2] && duplicated == 2);
    assert(log_calls == 1 && lpr_boot_diagnostic_state.last_log_status == 19);
    assert(lpr_boot_diagnostic_state.failed == 1);
    lpr_boot_diagnostic_emit("suppressed", 0, 0, 0, 0, 0);
    assert(log_calls == 1);

    reset(0, "");
    lpr_boot_diagnostic_emit("max-fields", UINT64_MAX, UINT64_MAX, UINT64_MAX,
        INT64_MIN, INT64_MAX);
    assert(strstr(last_log, "status=-9223372036854775808 raw=9223372036854775807"));
    lpr_boot_diagnostic_other_tu();
    assert(log_calls == 2 && lpr_boot_diagnostic_state.records == 2);
    char huge_stage[1024];
    memset(huge_stage, 'x', sizeof(huge_stage) - 1);
    huge_stage[sizeof(huge_stage) - 1] = 0;
    for (unsigned i = 0; i < 1000; ++i)
        lpr_boot_diagnostic_emit(huge_stage, UINT64_MAX, UINT64_MAX, UINT64_MAX,
            INT64_MIN, INT64_MIN);
    assert(log_calls == LPR_BOOT_DIAGNOSTIC_RECORDS);
    assert(lpr_boot_diagnostic_state.records == LPR_BOOT_DIAGNOSTIC_RECORDS);
    lpr_boot_diagnostic_other_tu();
    assert(log_calls == LPR_BOOT_DIAGNOSTIC_RECORDS);
    reset(0, "");
    lpr_process_manifest.linux_pid = 17;
    lpr_boot_diagnostic_emit("excluded-pid", 0, 0, 0, 0, 0);
    assert(!log_calls);
    lpr_linux_current_pid = 2;
    lpr_boot_diagnostic_emit("cached-pid", 0, 0, 0, 0, 0);
    assert(log_calls == 1 && strstr(last_log, "pid=2"));
    assert(lpr_boot_diagnostic_stdio_begin(1, 1));
    assert(lpr_boot_diagnostic_tty_active());
    assert(!lpr_boot_diagnostic_stdio_begin(1, 3));
    lpr_boot_diagnostic_stdio_end(1);
    assert(!lpr_boot_diagnostic_tty_active());
    lpr_boot_diagnostic_fs_begin();
    assert(lpr_boot_diagnostic_fs_active());
    lpr_boot_diagnostic_fs_end(0); /* HELLO and ATTACH are nested bootstrap. */
    lpr_boot_diagnostic_fs_end(1);
    assert(lpr_boot_diagnostic_fs_active());
    lpr_boot_diagnostic_fs_end(1);
    assert(!lpr_boot_diagnostic_fs_active());
    lpr_boot_diagnostic_fs_begin();
    assert(!lpr_boot_diagnostic_fs_active());
    unsigned evaluated = 0;
    lpr_boot_diagnostic_fs_emit("after-second-operation", ++evaluated, 0, 0, 0, 0);
    assert(!evaluated);
    reset(0, "");
    log_result = -22;
    lpr_boot_diagnostic_emit("negative-log-status", 0, 0, 0, -95, -22);
    assert(log_calls == 1 && lpr_boot_diagnostic_state.last_log_status == -22);
#else
    int evaluated = 0;
    lpr_boot_diagnostic_emit("disabled", ++evaluated, ++evaluated, ++evaluated,
        ++evaluated, ++evaluated);
    assert(!lpr_boot_diagnostic_stdio_begin(++evaluated, ++evaluated));
    lpr_boot_diagnostic_stdio_end(++evaluated);
    lpr_boot_diagnostic_read_enter(++evaluated, ++evaluated, ++evaluated, ++evaluated);
    lpr_boot_diagnostic_fs_begin();
    lpr_boot_diagnostic_fs_end(++evaluated);
    lpr_boot_diagnostic_fs_emit("disabled-fs", ++evaluated, ++evaluated,
        ++evaluated, ++evaluated, ++evaluated);
    assert(!lpr_boot_diagnostic_fs_active());
    assert(!evaluated && !log_calls && !snapshot_calls);
#endif
    puts("default stdio: console, headless service, inherited/closed descriptors: PASS");
}

#include "lpr_ipc_mock.h"
int64_t lpr_ipc_message(uint64_t nr, uint64_t fd, uint64_t send, uint64_t receive,
    uint64_t timeout, uint64_t flags, int *committed) {
    return test_lpr_ipc_noncall(nr, fd, send, receive, timeout, flags, committed);
}

int64_t lpr_pacha_syscall4(uint64_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    (void)nr;(void)a;(void)b;(void)c;(void)d;abort();
}

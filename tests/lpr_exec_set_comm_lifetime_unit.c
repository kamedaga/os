// SPDX-License-Identifier: MIT
/* Exercise the production execve orchestration with owned argument storage
 * and mocked native/RPC boundaries, not a guest address-space replacement. */
#include "../userland/personality/linux/runtime/lpr_process/syscalls.c"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lpr_state_t lpr_state;
static jmp_buf replacement;
static char raw_path[64];
static char *owned_path;
static unsigned destroyed, transaction_destroyed, installed, published;
static unsigned local_closed, transaction_committed, closed, killed, native_exited, exec_called;
static int failure, comm_failure;
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
lpr_boot_diagnostic_state_t lpr_boot_diagnostic_state;
static unsigned diagnostic_calls;
static int diagnostic_failure;
#endif

void *lpr_memset(void *p, int value, size_t size) { return memset(p, value, size); }
size_t lpr_strnlen(const char *p, size_t size)
{ size_t n = 0; while (n < size && p[n]) ++n; return n; }
void lpr_linux_process_state_init(void) {}
void lpr_trace_process_event(const char *event, uint64_t a, uint64_t b, int64_t status)
{ (void)event; (void)a; (void)b; (void)status; }

int lpr_exec_values_prepare(filed_exec_path_t *exec, const char *path,
    uint64_t argv, uint64_t envp)
{
    assert(path == raw_path && !argv && !envp);
    owned_path = malloc(strlen(path) + 1);
    assert(owned_path);
    strcpy(owned_path, path);
    exec->path = owned_path;
    exec->allocation = owned_path;
    return 0;
}
void lpr_exec_values_destroy(filed_exec_path_t *exec)
{
    assert(!destroyed && exec->allocation == owned_path);
    free(owned_path);
    owned_path = NULL;
    memset(exec, 0, sizeof(*exec));
    ++destroyed;
}
int lpr_prepare_exec_manifest(filed_exec_path_t *exec, lpr_exec_transaction_t *tx)
{
    assert(exec->path == owned_path && tx->manifest_fd == -1 && !destroyed);
    return failure == 1 ? -LPR_LINUX_ENOMEM : 0;
}
int64_t lpr_filed_exec_self(filed_exec_path_t *exec,
    const lpr_exec_transaction_t *tx, pacha_fd_t *process, pacha_fd_t *thread, pacha_fd_t *bootstrap)
{
    (void)tx;
    assert(!destroyed && !strcmp(exec->path, "/sbin/pacha-service-runtime"));
    /* The later publication must use owned arguments, not the user buffer. */
    strcpy(raw_path, "/wrong/raw/path");
    if (failure == 2) return -LPR_LINUX_ENOENT;
    *process = 41; *thread = 42; *bootstrap = 43;
    return 0;
}
int lpr_install_exec_bootstrap_fd(pacha_fd_t fd)
{
    assert(fd == 43 && !destroyed && !installed);
    ++installed;
    return failure == 3 ? -LPR_LINUX_EIO : 0;
}
int lpr_supervisor_set_comm(const char *path)
{
    assert(installed && !destroyed && path == owned_path && path != raw_path);
    assert(!strcmp(path, "/sbin/pacha-service-runtime"));
    ++published;
    return comm_failure ? -LPR_LINUX_EIO : 0;
}
int64_t lpr_supervisor_call_token(uint32_t op, uint64_t token, pacha_fd_t fd, uint64_t *result)
{
    assert(op == LPRS_OP_PROCESS_EXEC_COMMIT_BEGIN && token == 101 && fd == 41 && !result);
    assert(destroyed == 1 && published == 1);
    return failure == 4 ? -LPR_LINUX_EIO : 0;
}
void lpr_destroy_exec_transaction(lpr_exec_transaction_t *tx)
{ (void)tx; assert(destroyed == (unsigned)(failure != 1)); ++transaction_destroyed; }
int64_t lpr_close_local_state_before_self_exec(lpr_exec_transaction_t *tx)
{ assert(tx && destroyed == 1); ++local_closed; return failure == 5 ? -LPR_LINUX_EIO : 0; }
void lpr_exec_transaction_commit_self(lpr_exec_transaction_t *tx)
{ (void)tx; assert(local_closed == 1 && destroyed == 1); ++transaction_committed; }
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    if (nr == PACHAOS_SYSCALL_PROCESS_EXIT) {
        assert(fd == 127 && failure == 5 && local_closed == 1);
        assert(transaction_destroyed == 1 && killed == 1 && closed == 2);
        assert(!transaction_committed && !exec_called);
        ++native_exited;
        longjmp(replacement, 2);
    }
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE);
    assert(fd == 41 || fd == 42 || fd == 43 || fd == LPR_BOOTSTRAP_FD);
    ++closed; return 0;
}
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t arg)
{
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
    if (nr == PACHAOS_SYSCALL_LOG) {
        assert(arg > 0 && arg <= 256);
        assert(!memcmp((const void *)(uintptr_t)fd, "[LPR_BOOT] ", 11));
        ++diagnostic_calls;
        return diagnostic_failure ? 1 : 0;
    }
#endif
    assert(nr == PACHAOS_SYSCALL_PROCESS_KILL && fd == 41 && arg == 1);
    ++killed; return 0;
}
int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t process, uint64_t thread, uint64_t flags)
{
    assert(nr == PACHAOS_SYSCALL_PROCESS_EXEC_FROM && process == 41 && thread == 42 && !flags);
    assert(destroyed == 1 && transaction_committed == 1);
    ++exec_called;
    if (failure == 6) return 1;
    longjmp(replacement, 1); /* Successful native replacement never returns. */
}
int64_t lpr_pacha_status_to_errno(int64_t status)
{ assert(status == 1); return -LPR_LINUX_EINVAL; }

static void run(int stage, int supervisor, int name_error)
{
    failure = stage; comm_failure = name_error;
    destroyed = transaction_destroyed = installed = published = 0;
    local_closed = transaction_committed = closed = killed = 0;
    native_exited = exec_called = 0;
    lpr_supervisor_enabled = supervisor;
    lpr_supervisor_token = 101;
    lpr_linux_current_pid = 1;
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
    memset(&lpr_boot_diagnostic_state, 0, sizeof(lpr_boot_diagnostic_state));
    diagnostic_calls = 0;
#endif
    strcpy(raw_path, "/sbin/pacha-service-runtime");
    const int replaced = setjmp(replacement);
    if (replaced == 0) {
        int64_t status = lpr_linux_execve((uint64_t)(uintptr_t)raw_path, 0, 0);
        assert(stage && status == (stage == 1 ? -LPR_LINUX_ENOMEM :
            stage == 2 ? -LPR_LINUX_ENOENT : stage == 6 ? -LPR_LINUX_EINVAL : -LPR_LINUX_EIO));
    } else assert((replaced == 1 && stage == 0) || (replaced == 2 && stage == 5));
    assert(destroyed == 1 && owned_path == NULL);
    assert(published == (unsigned)(supervisor && stage != 1 && stage != 2 && stage != 3));
    assert(transaction_destroyed == (unsigned)(stage != 0));
    assert(killed == (unsigned)(stage >= 3));
    assert(closed == (stage == 3 || stage == 6 ? 3u : stage == 4 || stage == 5 ? 2u : 0u));
    assert(native_exited == (unsigned)(stage == 5));
    assert(exec_called == (unsigned)(stage == 0 || stage == 6));
    assert(transaction_committed == exec_called);
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
    assert(diagnostic_calls > 0);
    if (diagnostic_failure)
        assert(diagnostic_calls == 1 && lpr_boot_diagnostic_state.failed &&
            lpr_boot_diagnostic_state.last_log_status == 1);
#endif
}
int main(void)
{
    for (int stage = 0; stage <= 6; ++stage) run(stage, 1, 0);
    run(0, 1, 1); /* Name publication remains best-effort. */
    run(0, 0, 0); /* No supervisor still releases the arguments. */
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
    diagnostic_failure = 1;
    for (int stage = 0; stage <= 6; ++stage) run(stage, 1, 0);
    run(0, 1, 1);
    run(0, 0, 0);
#endif
    puts("exec SET_COMM: owned copied path survives publication; every success/error path frees exactly once PASS");
}

#include "lpr_ipc_mock.h"
int64_t lpr_ipc_message(uint64_t nr, uint64_t fd, uint64_t send, uint64_t receive,
    uint64_t timeout, uint64_t flags, int *committed) {
    return test_lpr_ipc_noncall(nr, fd, send, receive, timeout, flags, committed);
}

int64_t lpr_pacha_syscall4(uint64_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    (void)nr;(void)a;(void)b;(void)c;(void)d;abort();
}

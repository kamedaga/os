// SPDX-License-Identifier: MIT
/* Production bootstrap with mocked capability/RPC operations: diagnostic
 * LOG failure must not alter successful activation or any exit-127 path. */
#include "../userland/personality/linux/runtime/lpr_process/bootstrap_state.c"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

lpr_state_t lpr_state;
static lpr_signal_thread_state_t signal_state;
lpr_signal_thread_state_t *lpr_signal_thread_state_current(void) { return &signal_state; }
static struct { lpr_manifest_t header; lpr_manifest_capability_t cap; } payload;
#define donor payload.header
static uint64_t inherited_native_fd;
static unsigned native_closes, installs;
static lpr_fd_entry_t entries[64];
static lpr_ofd_t ofds[64];
static lpr_backend_record_t backends[64];
static int backend;
static const lpr_fd_install_t install = {.ops_id = LPR_FD_OPS_DEVICE,
    .rights = LPR_FD_RIGHT_DUP, .backend_state = &backend,
    .backend_state_bytes = sizeof(backend)};
static jmp_buf retired;
static unsigned maps, unmaps, closes, activates, commits, pages_destroyed, logs;
static int failure, log_failure, empty_manifest;
void lpr_fd_arrays_init(void)
{
    if (lpr_fd_table_capacity) return;
    lpr_fd_table_init(&lpr_control_fd_table, entries, 64, ofds, 64, backends, 64);
    lpr_fd_table_capacity = 64;
}
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
lpr_boot_diagnostic_state_t lpr_boot_diagnostic_state;
#endif
void *lpr_memset(void *p, int value, size_t size) { return memset(p, value, size); }
void *lpr_memcpy(void *out, const void *in, size_t size) { return memcpy(out, in, size); }
int64_t lpr_pacha_syscall6(uint64_t nr, uint64_t fd, uint64_t offset,
    uint64_t size, uint64_t prot, uint64_t flags, uint64_t hint)
{
    assert(nr == PACHAOS_SYSCALL_MMAP && fd == LPR_BOOTSTRAP_FD && !offset &&
        (size == sizeof(donor) || size == sizeof(payload)) && prot == PACHAOS_PROT_READ &&
        flags == PACHAOS_MMAP_SHARED && !hint);
    ++maps;
    if ((maps == 1 && failure == 1) || (maps == 2 && failure == 4)) return 1;
    return (int64_t)(uintptr_t)&donor;
}
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b)
{
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
    if (nr == PACHAOS_SYSCALL_LOG) {
        assert(b && b <= 256 && !memcmp((const void *)(uintptr_t)a, "[LPR_BOOT] ", 11));
        ++logs; return log_failure ? 1 : 0;
    }
#endif
    assert(nr == PACHAOS_SYSCALL_MUNMAP && a == (uint64_t)(uintptr_t)&donor && (b == sizeof(donor) || b == sizeof(payload)));
    ++unmaps; return 0;
}
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t a)
{
    if (nr == PACHAOS_SYSCALL_PROCESS_EXIT) { assert(a == 127); longjmp(retired, 1); }
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE &&
        (a == LPR_BOOTSTRAP_FD || a == 51));
    ++closes; return 0;
}
int lpr_manifest_validate(const void *memory, uint64_t capacity)
{ assert(memory == &donor && capacity == donor.byte_size); return failure != 5; }
int lpr_install_manifest_fds(const lpr_manifest_t *manifest)
{
    assert(manifest == &donor);
    ++installs;
    if (empty_manifest) return failure != 6;
    lpr_fd_arrays_init();
    assert(lpr_fd_table_soft_limit(&lpr_control_fd_table) == LPR_LINUX_FD_LIMIT);
    /* FD 40 survives a parent soft limit of 1. */
    assert(!lpr_fd_table_install_at(&lpr_control_fd_table, 40, &install));
    assert(!lpr_fd_table_install_at(&lpr_control_fd_table, 0, &install));
    return failure != 6;
}
int64_t lpr_close_native_fd_if_open(uint64_t fd)
{ assert(fd == inherited_native_fd && fd >= 16 && fd < PACHAOS_THREAD_SELF_FD); ++native_closes; return 0; }
int lpr_install_exec_bootstrap_fd(pacha_fd_t fd)
{ assert(fd == LPR_BOOTSTRAP_FD); return failure == 7 ? -LPR_LINUX_EIO : 0; }
uint64_t lpr_linux_unblockable_signal_mask(void) { return 0; }
uint64_t lpr_linux_signal_bit(uint32_t sig) { return UINT64_C(1) << (sig - 1); }
pacha_fd_t lpr_create_standalone_wire_page(void **page)
{ if (failure == 8) return -LPR_LINUX_ENOMEM; *page = &donor; return 52; }
void lpr_destroy_standalone_wire_page(pacha_fd_t fd, void *page)
{ assert(fd == 52 && page == &donor); ++pages_destroyed; }
int64_t lpr_pacha_status_to_errno(int64_t status) { return status; }
int64_t lpr_process_client_activate(uint64_t *counter, int64_t (*normalize)(int64_t),
    pacha_fd_t bootstrap, uint64_t token, pacha_fd_t page_fd, void *page)
{
    assert(counter == &lpr_request_id && normalize == lpr_pacha_status_to_errno &&
        bootstrap == 51 && token == 101 && page_fd == 52 && page == &donor);
    ++activates; return failure == 9 ? -LPR_LINUX_EIO : 0;
}
int64_t lpr_supervisor_call_token(uint32_t op, uint64_t token, pacha_fd_t fd, uint64_t *result)
{
    assert(op == LPRS_OP_PROCESS_EXEC_COMMIT_DONE && token == 101 && fd == -1 && !result);
    ++commits; return failure == 10 ? -LPR_LINUX_EIO : 0;
}
static void run(int stage, int reject_log, int empty, uint64_t native_fd)
{
    inherited_native_fd = native_fd; native_closes = installs = 0;
    failure = stage; log_failure = reject_log; empty_manifest = empty;
    memset(&lpr_state, 0, sizeof(lpr_state));
    lpr_linux_current_pid = 1;
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
    memset(&lpr_boot_diagnostic_state, 0, sizeof(lpr_boot_diagnostic_state));
#endif
    donor = (lpr_manifest_t){.magic = LPR_MANIFEST_MAGIC, .byte_size = sizeof(donor),
        .flags = LPR_MANIFEST_FLAG_SUPERVISOR, .linux_pid = 1,
        .nofile_soft = 1, .nofile_hard = 32,
        .supervisor_token = 101, .supervisor_bootstrap_fd = 51};
    if (native_fd) {
        donor.byte_size = sizeof(payload);
        donor.capability_offset = sizeof(donor); donor.capability_count = 1;
        donor.cwd_handle = 123; donor.cwd_capability_index = 0;
        payload.cap = (lpr_manifest_capability_t){.native_fd = native_fd,
            .flags = LPR_MANIFEST_CAPABILITY_CWD_LEASE};
    }
    if (empty == 1) donor.nofile_soft = donor.nofile_hard = LPR_LINUX_FD_LIMIT;
    if (empty == 2) donor.nofile_soft = 0;
    if (stage == 2) donor.magic = 0;
    if (stage == 3) donor.entry_count = LPR_FD_TABLE_MAX_SIZE + 1u;
    maps = unmaps = closes = activates = commits = pages_destroyed = logs = 0;
    if (!setjmp(retired)) { assert(lpr_load_manifest() == 1 && !stage); }
    else assert(stage && !lpr_manifest_valid);
    assert(maps == (stage >= 1 && stage <= 3 ? 1u : 2u));
    assert(unmaps == (stage == 1 ? 0u : stage <= 4 && stage ? 1u : 2u));
    assert(activates == (unsigned)(!stage || stage >= 9));
    assert(commits == (unsigned)(!stage || stage == 10));
    assert(pages_destroyed == activates);
    if (!stage) {
        uint64_t limits[2];
        assert(!lpr_fd_table_prlimit(&lpr_control_fd_table, 0, limits));
        assert(limits[0] == donor.nofile_soft && limits[1] == donor.nofile_hard);
        lpr_fd_arrays_init(); /* The first later open must not reset inherited limits. */
        assert(!lpr_fd_table_prlimit(&lpr_control_fd_table, 0, limits));
        assert(limits[0] == donor.nofile_soft && limits[1] == donor.nofile_hard);
        uint16_t flags;
        if (!empty) assert(!lpr_fd_table_get_fd_flags(&lpr_control_fd_table, 40, &flags));
        lpr_linux_fd_t fd;
        assert((lpr_fd_table_alloc(&lpr_control_fd_table, 0, &install, &fd) == 0) == (empty == 1));
    }
    if (native_fd && stage == 6) {
        const int valid = native_fd >= 16 && native_fd < PACHAOS_THREAD_SELF_FD;
        assert(installs == (unsigned)valid);
        assert(native_closes == (valid ? 2u : 0u));
    }
    if (!stage && native_fd) assert(lpr_cwd_lease_fd == (pacha_fd_t)native_fd);
    assert(closes == 1); /* failure closes bootstrap; commit failure/success closes handoff */
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
    assert(logs && (!reject_log || logs == 1));
#else
    assert(!logs);
#endif
}
int main(void)
{
    for (int reject_log = 0; reject_log <= 1; ++reject_log)
        for (int stage = 0; stage <= 10; ++stage)
            for (int empty = 0; empty <= 2; ++empty) {
                run(stage, reject_log, empty, 0);
                run(stage, reject_log, empty, UINT32_C(0xfffffffd));
            }
    const uint64_t invalid[] = {15, PACHAOS_THREAD_SELF_FD, UINT32_MAX, UINT64_MAX};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i)
        run(6, 0, 1, invalid[i]);
    puts("bootstrap diagnostics: activation/commit success and ten exit127 failures unchanged with rejected LOG PASS");
}

#include "lpr_ipc_mock.h"
int64_t lpr_ipc_message(uint64_t nr, uint64_t fd, uint64_t send, uint64_t receive,
    uint64_t timeout, uint64_t flags, int *committed) {
    return test_lpr_ipc_noncall(nr, fd, send, receive, timeout, flags, committed);
}

int64_t lpr_pacha_syscall4(uint64_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    (void)nr;(void)a;(void)b;(void)c;(void)d;abort();
}

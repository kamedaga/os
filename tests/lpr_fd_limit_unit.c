#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include "../userland/personality/linux/runtime/lpr_fd/dup_pipe.c"
#include "../userland/personality/linux/runtime/lpr_dispatch.c"

lpr_state_t lpr_state;
static lpr_fd_entry_t entries[8192];
static lpr_ofd_t ofds[8192];
static lpr_backend_record_t backends[8192];
static unsigned growth_attempts;
static int backend;

void lpr_fd_arrays_init(void) {}
int lpr_fd_table_ensure_capacity(uint64_t n)
{ (void)n; growth_attempts++; return -LPR_LINUX_EMFILE; }
int lpr_fd_table_ensure_fd(uint64_t n)
{ return n < 8192 ? 0 : -LPR_LINUX_EMFILE; }
int lpr_control_require_fd(uint64_t n)
{
    uint16_t flags;
    return n <= LPR_LINUX_FD_MAX &&
        !lpr_fd_table_get_fd_flags(&lpr_control_fd_table, n, &flags) ? 0 : -LPR_LINUX_EBADF;
}
int64_t lpr_fd_prepare_dup(uint64_t fd) { return lpr_control_require_fd(fd); }
int64_t lpr_filed_fd_binding(uint64_t fd) { (void)fd; return 0; }
int64_t lpr_backend_finish_drop(const lpr_fd_drop_t *drop) { (void)drop; return 0; }
int64_t lpr_linux_getpid(void) { return 42; }

static void set_limit(uint64_t soft, uint64_t hard)
{
    const uint64_t values[] = {soft, hard};
    assert(!lpr_linux_set_rlimit(LPR_LINUX_RLIMIT_NOFILE, (uintptr_t)values));
}
static void *change_limit(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 10000; i++) set_limit(i & 1 ? 4 : 8, 8192);
    return 0;
}

int main(void)
{
    lpr_fd_table_init(&lpr_control_fd_table, entries, 8192, ofds, 8192, backends, 8192);
    lpr_fd_table_capacity = 8192;
    uint64_t values[2];
    assert(!lpr_linux_copy_out_rlimit(LPR_LINUX_RLIMIT_NOFILE, (uintptr_t)values));
    assert(values[0] == LPR_LINUX_FD_LIMIT && values[1] == LPR_LINUX_FD_LIMIT);
    set_limit(8192, 8192);
    const lpr_fd_install_t install = {.ops_id = LPR_FD_OPS_DEVICE,
        .rights = LPR_FD_RIGHT_DUP, .backend_state = &backend, .backend_state_bytes = sizeof(backend)};
    for (unsigned i = 0; i < 4200; i++) assert(!lpr_fd_table_install_at(&lpr_control_fd_table, i, &install));
    assert(lpr_fd_table_open_count(&lpr_control_fd_table) == 4200);
    set_limit(4, 8192);
    assert(!lpr_control_require_fd(4199));
    assert(lpr_fd_alloc_initialized(&install) == -LPR_LINUX_EMFILE);
    assert(!growth_attempts);
    assert(lpr_linux_dup(3, 0, 0) == -LPR_LINUX_EMFILE);
    assert(lpr_linux_dup(3, 4, 0) == -LPR_LINUX_EINVAL);
    assert(lpr_linux_dup2(3, 4, 0) == -LPR_LINUX_EBADF);
    assert(lpr_linux_dup2(3, UINT64_MAX, 0) == -LPR_LINUX_EBADF);
    assert(lpr_linux_dup2(3, 3, 0) == 3);
    assert(lpr_sys_dup3(3, 3, 0, 0, 0, 0, 0) == -LPR_LINUX_EINVAL);
    assert(lpr_linux_dup2(6000, 6000, 0) == -LPR_LINUX_EBADF);
    lpr_fd_drop_t drop;
    assert(!lpr_fd_table_close(&lpr_control_fd_table, 2, &drop));
    assert(lpr_linux_dup(4199, 0, 1) == 2);
    assert(entries[2].fd_flags == LPR_FD_ENTRY_CLOEXEC);
    assert(lpr_linux_dup2(4199, 1, 0) == 1);
    for (unsigned i = 0; i < 4200; i++) assert(!lpr_fd_table_close(&lpr_control_fd_table, i, &drop));
    assert(!lpr_fd_table_open_count(&lpr_control_fd_table));
    assert(!lpr_fd_table_live_ofd_count(&lpr_control_fd_table));

    set_limit(1, 8192);
    const lpr_fd_install_t pair[] = {install, install};
    lpr_linux_fd_t fds[2];
    assert(lpr_fd_alloc_initialized_batch(pair, 2, fds) == -LPR_LINUX_EMFILE);
    assert(!lpr_fd_table_open_count(&lpr_control_fd_table));
    assert(!lpr_fd_table_live_ofd_count(&lpr_control_fd_table));
    set_limit(2, 8192);
    assert(!lpr_fd_table_stage_batch(&lpr_control_fd_table, pair, 2, fds));
    set_limit(1, 8192);
    assert(lpr_fd_table_publish_batch(&lpr_control_fd_table, fds, 2) != 0);
    assert(entries[0].active == 2 && entries[1].active == 2);
    for (unsigned i = 0; i < 2; i++) assert(!lpr_fd_table_abort_staged(&lpr_control_fd_table, fds[i], &drop));
    /* Encode the parent's policy through the production manifest contract,
     * then model the new exec image's freshly initialized table. */
    lpr_manifest_t manifest;
    lpr_manifest_layout_t layout;
    assert(!lpr_manifest_layout(0, 0, 0, 0, &layout));
    assert(!lpr_manifest_begin(&manifest, sizeof(manifest), &layout, 0, 0, 0, 0));
    assert(!lpr_fd_table_prlimit(&lpr_control_fd_table, 0, values));
    manifest.nofile_soft = values[0]; manifest.nofile_hard = values[1];
    assert(!lpr_manifest_seal(&manifest, sizeof(manifest)));
    assert(lpr_manifest_validate(&manifest, sizeof(manifest)));
    lpr_fd_table_init(&lpr_control_fd_table, entries, 8192, ofds, 8192, backends, 8192);
    values[0] = manifest.nofile_soft; values[1] = manifest.nofile_hard;
    assert(!lpr_fd_table_prlimit(&lpr_control_fd_table, values, 0));
    assert(lpr_fd_alloc_initialized(&install) == 0);
    assert(lpr_fd_alloc_initialized(&install) == -LPR_LINUX_EMFILE);
    assert(!lpr_fd_table_close(&lpr_control_fd_table, 0, &drop));
    manifest.checksum = 0;
    manifest.nofile_soft = manifest.nofile_hard + 1;
    assert(!lpr_manifest_validate(&manifest, sizeof(manifest)));
    manifest.nofile_soft = 0; manifest.nofile_hard = LPR_LINUX_FD_LIMIT + 1;
    assert(!lpr_manifest_validate(&manifest, sizeof(manifest)));
    manifest.nofile_hard = 0;
    manifest.entry_offset = sizeof(manifest) - 16; /* previous header layout */
    assert(!lpr_manifest_validate(&manifest, sizeof(manifest)));
    set_limit(0, 8192);
    assert(lpr_fd_alloc_initialized(&install) == -LPR_LINUX_EMFILE);
    assert(!growth_attempts);
    values[0] = 8193; values[1] = 8192;
    assert(lpr_linux_set_rlimit(LPR_LINUX_RLIMIT_NOFILE, (uintptr_t)values) == -LPR_LINUX_EINVAL);
    values[0] = 1; values[1] = 8193;
    assert(lpr_linux_set_rlimit(LPR_LINUX_RLIMIT_NOFILE, (uintptr_t)values) == -LPR_LINUX_EPERM);
    values[0] = 4; values[1] = 8192;
    assert(!lpr_linux_prlimit64(0, LPR_LINUX_RLIMIT_NOFILE, (uintptr_t)values, (uintptr_t)values));
    assert(values[0] == 0 && values[1] == 8192);
    assert(lpr_fd_table_soft_limit(&lpr_control_fd_table) == 4);

    /* Reusing a generation must never restore a lowered NOFILE policy. */
    lpr_control_fd_table.generation = UINT32_MAX;
    assert(lpr_fd_alloc_initialized(&install) == 0);
    assert(!lpr_fd_table_prlimit(&lpr_control_fd_table, 0, values));
    assert(values[0] == 4 && values[1] == 8192);
    assert(!lpr_fd_table_close(&lpr_control_fd_table, 0, &drop));

    /* Race policy updates against publication/close; all returned numbers
     * must satisfy at least one of the two policies in force. */
    lpr_state.thread_count = 2;
    lpr_fd_table_configure_lock(&lpr_control_fd_table, &lpr_state.thread_count, 0, 0);
    pthread_t thread;
    assert(!pthread_create(&thread, 0, change_limit, 0));
    for (unsigned i = 0; i < 10000; i++) {
        lpr_linux_fd_t fd;
        if (!lpr_fd_table_alloc(&lpr_control_fd_table, 0, &install, &fd)) {
            assert(fd < 8);
            assert(!lpr_fd_table_close(&lpr_control_fd_table, fd, &drop));
        }
    }
    assert(!pthread_join(thread, 0));
    assert(!lpr_fd_table_open_count(&lpr_control_fd_table));
    puts("RLIMIT_NOFILE: 4200 FDs, soft/hard, zero, EMFILE, dup errno, atomic batch and concurrent limit update: PASS");
}

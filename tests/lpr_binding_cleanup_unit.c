/* Exercise real producer binding boundaries and real pinned-close table
 * mutation; RPC/native mocks only supply adoption, failure and a reuse race. */
#include "../userland/personality/linux/runtime/lpr_process/exec.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lpr_state_t lpr_state;
static lpr_fd_entry_t entries[16];
static lpr_ofd_t ofds[16];
static lpr_backend_record_t backends[16];
static lpr_filed_backend_t old_file, replacement;
static int64_t binding_result;
static unsigned binds, leases, closes, drops, reuse, epolls;
static uint64_t closed_handle;

void lpr_fd_arrays_init(void) {}
void lpr_cwd_init(void) {}
void *lpr_memset(void *p, int value, size_t size) { return memset(p, value, size); }
int64_t lpr_pacha_status_to_errno(int64_t status) { return status; }
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{ assert(nr == PACHAOS_SYSCALL_FD_CLOSE && fd == 70); ++leases; return 0; }
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t data)
{ assert(nr == PACHAOS_SYSCALL_FD_GET_INFO && fd == 70); (void)data; return 0; }
int64_t lpr_filed_close_handle(uint64_t handle)
{ assert(handle == 533); ++closes; closed_handle = handle; return 0; }
void lpr_epoll_before_close(uint64_t fd)
{ assert(fd == 3 || fd == 4); ++epolls; }

int64_t lpr_filed_endpoint_ready(void)
{
    ++binds;
    if (reuse) {
        lpr_fd_drop_t drop;
        assert(!lpr_fd_table_close(&lpr_control_fd_table, 3, &drop) && !drop.ready);
        lpr_fd_install_t install = {.ops_id = LPR_FD_OPS_FILED,
            .rights = LPR_FD_RIGHT_DUP, .backend_state = &replacement,
            .backend_state_bytes = sizeof(replacement)};
        if (reuse == 1) assert(!lpr_fd_table_install_at(&lpr_control_fd_table, 3, &install));
        reuse = 0;
    }
    if (!binding_result) { lpr_cwd_handle = 533; old_file.handle = 511; }
    return binding_result;
}

int64_t lpr_backend_finish_drop(const lpr_fd_drop_t *drop)
{
    assert(drop->ready && drop->state == &old_file);
    ++drops;
    const lpr_filed_backend_t *file = drop->state;
    /* Failed adoption must not send a second stale CLOSE or retry binding. */
    assert(file->handle == (binding_result ? 0 : 511));
    assert(file->lease_fd.raw == 70);
    ++leases;
    return 0;
}

static void install_old(void)
{
    lpr_fd_table_init(&lpr_control_fd_table, entries, 16, ofds, 16, backends, 16);
    lpr_fd_table_capacity = 16;
    old_file = (lpr_filed_backend_t){.active = 1, .handle = 112, .lease_fd = {.raw = 70}};
    replacement = (lpr_filed_backend_t){.active = 1, .handle = 900};
    lpr_fd_install_t install = {.ops_id = LPR_FD_OPS_FILED,
        .rights = LPR_FD_RIGHT_DUP, .fd_flags = LPR_FD_ENTRY_CLOEXEC,
        .backend_state = &old_file, .backend_state_bytes = sizeof(old_file)};
    assert(!lpr_fd_table_install_at(&lpr_control_fd_table, 3, &install));
    binds = leases = drops = epolls = 0;
}

static void test_close(void)
{
    const int64_t errors[] = {0, -LPR_LINUX_ENOMEM, -LPR_LINUX_EIO};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        install_old(); binding_result = errors[i];
        assert(lpr_control_close_fd_status(3) == errors[i]);
        assert(!entries[3].active && drops == 1 && leases == 1 && binds == 1 && epolls == 1);
        assert(lpr_control_close_fd_status(3) == -LPR_LINUX_EBADF);
        assert(drops == 1 && binds == 1);
        for (unsigned j = 0; j < 16; ++j) assert(!ofds[j].active);

        install_old();
        lpr_fd_pin_t held;
        assert(!lpr_fd_table_pin(&lpr_control_fd_table, 3, &held));
        assert(lpr_control_close_fd_status(3) == errors[i]);
        assert(!entries[3].active && !drops && !leases && binds == 1);
        assert(old_file.handle == (errors[i] ? 0 : 511));
        lpr_fd_drop_t deferred;
        assert(!lpr_fd_table_unpin(&lpr_control_fd_table, &held, &deferred) && deferred.ready);
        assert(!lpr_backend_finish_drop(&deferred) && drops == 1 && leases == 1 && binds == 1);

        install_old(); reuse = 1;
        assert(lpr_control_close_fd_status(3) == (errors[i] ? errors[i] : -LPR_LINUX_EBADF));
        lpr_fd_pin_t current;
        assert(!lpr_fd_table_pin(&lpr_control_fd_table, 3, &current));
        assert(current.state == &replacement && replacement.handle == 900);
        lpr_fd_drop_t drop;
        assert(!lpr_fd_table_unpin(&lpr_control_fd_table, &current, &drop) && !drop.ready);
        assert(drops == 1 && binds == 1 && leases == 1);

        install_old(); reuse = 2;
        assert(lpr_control_close_fd_status(3) == (errors[i] ? errors[i] : -LPR_LINUX_EBADF));
        assert(!entries[3].active && drops == 1 && binds == 1 && leases == 1);

        if (errors[i]) {
            install_old();
            lpr_linux_fd_t alias;
            assert(!lpr_fd_table_dup(&lpr_control_fd_table, 3, 4, 0, &alias) && alias == 4);
            assert(lpr_control_close_fd_status(3) == errors[i]);
            assert(!entries[3].active && entries[4].active && !drops && !leases);
            assert(old_file.handle == 112); /* Still-live aliases retain authority for later adoption. */
            assert(lpr_control_close_fd_status(4) == errors[i]);
            assert(!entries[4].active && drops == 1 && leases == 1 && binds == 2);
        }
    }
}

static void test_exec_and_cwd(void)
{
    binding_result = 0; lpr_cwd_handle = 313;
    filed_exec_path_t exec = {.path = "/bin/busybox", .dir_handle = 313};
    assert(!lpr_prepare_manifest_filesystem(&exec));
    assert(exec.dir_handle == 533 && old_file.handle == 511);
    uint8_t *wire = NULL; size_t bytes = 0;
    assert(!filed_exec_payload_encode(&exec, &wire, &bytes));
    uint8_t envelope[KB2_FILESYSTEM_REQUEST_SIZE];
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_EXEC_SELF,
        .generation = 1, .sequence = 1, .handle = exec.dir_handle};
    assert(!kb2_fs_request_encode(envelope, sizeof(envelope), sizeof(envelope), &request));
    kb2_fs_request_t decoded_request;
    assert(!kb2_fs_request_decode(envelope, sizeof(envelope), sizeof(envelope), &decoded_request));
    assert(decoded_request.handle == 533);
    filed_exec_path_t decoded;
    assert(!filed_exec_payload_decode(wire, bytes, exec.path, decoded_request.handle, exec.flags, &decoded));
    assert(decoded.dir_handle == 533);
    filed_exec_payload_destroy(&decoded); free(wire);
    exec.dir_handle = 313; binding_result = -LPR_LINUX_ENOMEM;
    assert(lpr_prepare_manifest_filesystem(&exec) == -LPR_LINUX_ENOMEM);
    assert(exec.dir_handle == 313);
    for (unsigned failed = 0; failed < 2; ++failed) {
        binding_result = failed ? -LPR_LINUX_ENOMEM : 0;
        binds = leases = closes = 0; lpr_cwd_handle = lpr_cwd_lease_handle = 313;
        lpr_cwd_lease_fd = 70;
        lpr_close_cwd_state();
        assert(binds == 1 && leases == 1 && closes == !failed);
        assert(!lpr_cwd_handle && !lpr_cwd_lease_handle && lpr_cwd_lease_fd == -1);
        if (!failed) assert(closed_handle == 533);
        lpr_close_cwd_state();
        assert(binds == 1 && leases == 1);
    }
}

int main(void)
{
    test_close(); test_exec_and_cwd();
    puts("LPR_BINDING_CLEANUP_UNIT_PASS");
}

#include "lpr_ipc_mock.h"
int64_t lpr_ipc_message(uint64_t nr, uint64_t fd, uint64_t send, uint64_t receive,
    uint64_t timeout, uint64_t flags, int *committed) {
    return test_lpr_ipc_noncall(nr, fd, send, receive, timeout, flags, committed);
}

int64_t lpr_pacha_syscall4(uint64_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    (void)nr;(void)a;(void)b;(void)c;(void)d;abort();
}

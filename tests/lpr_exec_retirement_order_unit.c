#include "../userland/personality/linux/runtime/lpr_process/exec.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

lpr_state_t lpr_state;
static lpr_fd_entry_t entries[8];
static lpr_ofd_t ofds[8];
static lpr_backend_record_t backends[8];
static lpr_filed_backend_t file;
static lpr_exec_transaction_t *active_transaction;
static unsigned source_dropped, retired, lease_closed;
static int64_t retirement_status;
void *lpr_memset(void *p, int value, size_t size) { return memset(p, value, size); }
void lpr_fd_arrays_init(void) {}
int lpr_runtime_reserved_fd(uint64_t fd) { (void)fd; return 0; }
int lpr_control_fd_active(uint64_t fd)
{ return fd == 3 && entries[3].active; }
void lpr_control_close_fd(uint64_t fd)
{
    assert(fd == 3 && active_transaction->pin_count == 1);
    lpr_fd_drop_t drop;
    assert(!lpr_fd_table_close(&lpr_control_fd_table, fd, &drop) && !drop.ready);
}
int64_t lpr_backend_finish_drop(const lpr_fd_drop_t *drop)
{
    assert(drop->ready && drop->state == &file && !retired && !entries[3].active);
    /* A deferred FILED CLOSE is allowed to create a queue. The last
     * retirement must happen after this operation, never before it. */
    ++source_dropped; return 0;
}
int64_t lpr_filed_session_drop(void)
{
    assert(source_dropped == 1 && active_transaction->pin_count == 0);
    assert(active_transaction->prepared_lease_count == 1 && !lease_closed);
    ++retired; return retirement_status;
}
int64_t lpr_filed_endpoint_ready(void) { assert(0); return -1; }
int64_t lpr_filed_close_handle(uint64_t handle) { (void)handle; assert(0); return -1; }
int64_t lpr_close_native_fd_if_open(uint64_t fd)
{ assert(fd == 61 && retired == 1); ++lease_closed; return 0; }
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{ (void)nr; (void)fd; assert(0); return -1; }
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b)
{ (void)nr; (void)a; (void)b; assert(0); return -1; }
int64_t lpr_supervisor_call_token(uint32_t op, uint64_t token, int fd, uint64_t *result)
{ (void)op; (void)token; (void)fd; (void)result; assert(0); return -1; }

int main(void)
{
    for (unsigned failed = 0; failed < 2; ++failed) {
        lpr_fd_table_init(&lpr_control_fd_table, entries, 8, ofds, 8, backends, 8);
        lpr_fd_table_capacity = 8;
        lpr_fd_install_t install = {.ops_id = LPR_FD_OPS_FILED, .backend_state = &file,
            .backend_state_bytes = sizeof(file)};
        assert(!lpr_fd_table_install_at(&lpr_control_fd_table, 3, &install));
        lpr_fd_pin_t pin;
        assert(!lpr_fd_table_pin(&lpr_control_fd_table, 3, &pin));
        lpr_prepared_lease_t lease = {.fd = 61};
        lpr_exec_transaction_t tx = {.pins = &pin, .pin_count = 1,
            .prepared_leases = &lease, .prepared_lease_count = 1,
            .manifest_fd = -1, .cwd_lease_fd = -1, .supervisor_bootstrap_fd = -1};
        active_transaction = &tx; source_dropped = retired = lease_closed = 0;
        retirement_status = failed ? -LPR_LINUX_EIO : 0;
        assert(lpr_close_local_state_before_self_exec(&tx) == retirement_status);
        assert(source_dropped == 1 && retired == 1 && tx.pin_count == 0);
        if (failed) lpr_destroy_exec_transaction(&tx);
        else lpr_exec_transaction_commit_self(&tx);
        assert(lease_closed == failed && source_dropped == 1 && retired == 1);
    }
    puts("LPR_EXEC_RETIREMENT_ORDER_UNIT_PASS");
}

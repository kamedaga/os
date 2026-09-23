/* Production transaction cleanup with mocked native FD/RPC operations.
 * This tests ownership, not the actual guest exec/address-space replacement. */
#include "../userland/personality/linux/runtime/lpr_process/exec.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

lpr_state_t lpr_state;
static unsigned live[256], cancelled, resumed, unmapped;
static unsigned fail_install_stage;

void *lpr_memset(void *p, int value, size_t bytes) { return memset(p, value, bytes); }

int64_t lpr_pacha_status_to_errno(int64_t status)
{
    assert(status < 0);
    return -LPR_LINUX_EIO;
}

int64_t lpr_supervisor_call_token(uint32_t op, uint64_t token, int transfer, uint64_t *result)
{
    assert(op == LPRS_OP_PROCESS_EXEC_COMMIT_CANCEL && token == 101 && transfer == -1 && !result);
    cancelled++;
    return 0;
}

int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE && fd >= 16 && fd < 256 && live[fd]);
    live[fd] = 0;
    return 0;
}

int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t address, uint64_t bytes)
{
    if (nr == PACHA_FD_SYSCALL_GET_INFO) {
        assert(address >= 16 && address < 256 && live[address]);
        struct pacha_fd_info *info = (struct pacha_fd_info *)(uintptr_t)bytes;
        *info = (struct pacha_fd_info){ .kind = PACHA_FD_KIND_VMO,
            .flags = PACHA_FD_FLAG_PRIVATE,
            .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP |
                PACHA_FD_RIGHT_SET_FLAGS | PACHA_FD_RIGHT_CLOSE };
        return 0;
    }
    assert(nr == PACHAOS_SYSCALL_MUNMAP && address == 0x10000 && bytes == 4096);
    unmapped++;
    return 0;
}

int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t fd, uint64_t op, uint64_t a2, uint64_t a3)
{
    assert(nr == PACHA_FD_SYSCALL_FCNTL && fd >= 16 && fd < 256 && live[fd]);
    if (op == PACHA_FD_FCNTL_DUP) {
        assert(a3 && a2 >= 16 && a2 < 256);
        if (fd == 40 && fail_install_stage == 1) {
            fail_install_stage = 0;
            return -1;
        }
        for (unsigned i = (unsigned)a2; i < 256; i++)
            if (!live[i]) { live[i] = 1; return i; }
        assert(!"mock native table exhausted");
    }
    assert(op == PACHA_FD_FCNTL_SET_FLAGS && fd == LPR_BOOTSTRAP_FD);
    assert(a2 == PACHA_FD_FLAG_PRIVATE && (a3 & PACHA_FD_FLAG_CLOEXEC));
    if (fail_install_stage == 2) { fail_install_stage = 0; return -1; }
    return 0;
}

int64_t lpr_close_native_fd_if_open(uint64_t fd)
{
    return lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, fd);
}

void lpr_file_image_cache_resume(void) { resumed++; }

int lpr_fd_table_unpin(lpr_fd_table_t *table, const lpr_fd_pin_t *pin, lpr_fd_drop_t *drop)
{
    (void)table; (void)pin; (void)drop;
    assert(!"unexpected nonempty FD snapshot");
    return -1;
}

int64_t lpr_backend_finish_drop(const lpr_fd_drop_t *drop)
{
    (void)drop;
    assert(!"unexpected backend drop");
    return -1;
}

int main(void)
{
    lpr_filed_backend_t old_file = { .lease_fd.raw = 60 };
    lpr_filed_backend_t new_file = { .lease_fd.raw = 61 };
    live[60] = live[61] = 1;
    lpr_fork_close_displaced_capabilities(LPR_FD_OPS_FILED, &old_file, &new_file);
    assert(!live[60] && live[61]);
    lpr_fork_close_displaced_capabilities(LPR_FD_OPS_FILED, &new_file, &new_file);
    assert(live[61]);
    lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, 61);
    lpr_drm_backend_t old_drm = { .wait_fd.raw = 60, .lease_fd.raw = 61 };
    lpr_drm_backend_t new_drm = { .wait_fd.raw = 60, .lease_fd.raw = 62 };
    live[60] = live[61] = live[62] = 1;
    lpr_fork_close_displaced_capabilities(LPR_FD_OPS_DRM, &old_drm, &new_drm);
    assert(live[60] && !live[61] && live[62]);
    lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, 60);
    lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, 62);
    lpr_supervisor_token = 101;
    lpr_exec_transaction_t transaction = {
        .manifest_fd = -1, .cwd_lease_fd = -1, .supervisor_bootstrap_fd = 21,
        .supervisor_exec_prepared = 1, .file_image_cache_paused = 1,
    };
    live[21] = 1;
    lpr_destroy_exec_transaction(&transaction);
    assert(cancelled == 1 && resumed == 1 && !live[21]);
    lpr_destroy_exec_transaction(&transaction);
    assert(cancelled == 1 && resumed == 1); /* repeated failure cleanup is harmless */

    transaction = (lpr_exec_transaction_t){
        .manifest_fd = -1, .cwd_lease_fd = -1, .supervisor_bootstrap_fd = 21,
    };
    live[21] = 1;
    lpr_destroy_exec_transaction(&transaction);
    assert(cancelled == 1 && !live[21]); /* fork handoff is not an exec reservation */

    transaction = (lpr_exec_transaction_t){
        .manifest_fd = 22, .manifest = (lpr_manifest_t *)(uintptr_t)0x10000,
        .map_bytes = 4096, .cwd_lease_fd = -1, .supervisor_bootstrap_fd = 21,
        .supervisor_exec_prepared = 1,
    };
    live[21] = live[22] = 1;
    lpr_exec_transaction_commit_self(&transaction);
    assert(live[21] && !live[22] && unmapped == 1 && cancelled == 1);
    assert(transaction.supervisor_exec_prepared && transaction.supervisor_bootstrap_fd == 21);
    /* Native exec failure returns to the old image, which must still own
     * the reservation so its destructor can cancel it. Success never returns. */
    lpr_destroy_exec_transaction(&transaction);
    assert(!live[21] && cancelled == 2 && unmapped == 1);
    for (unsigned stage = 0; stage < 3; stage++) {
        live[40] = live[LPR_BOOTSTRAP_FD] = 1;
        fail_install_stage = stage;
        int status = lpr_install_exec_bootstrap_fd(40);
        assert(live[LPR_BOOTSTRAP_FD] && !live[16]); /* no backup leak */
        if (stage) {
            assert(status == -LPR_LINUX_EIO && live[40]);
            lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, 40);
        } else assert(status == 0 && !live[40]);
        lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, LPR_BOOTSTRAP_FD);
    }
    puts("lpr exec control lifetime: cancellation ownership, fork isolation, handoff retained until exec, repeated cleanup passed");
}

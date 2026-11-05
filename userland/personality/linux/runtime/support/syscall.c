#include "pacha/poll.h"
#include "pacha/native_syscall_retry.h"
#include "syscall.h"
#include "native_fd_capacity.h"
#include "emfile_diag.h"
#include <pacha/status.h>


/* Same native LOG sink and hex fields as lpr_browser_diag, always enabled for
 * this diagnostic. No allocation or LPR syscall wrappers (avoid recursion).
 * Fields: native pid, used, limit, detail. UINT64_MAX means unavailable. */
void lpr_emfile_diag_emit(const char *site, uint64_t used, uint64_t limit,
    uint64_t detail)
{
    uint64_t pid = pacha_native_syscall_retry6(
        PACHAOS_SYSCALL_GETPID, 0, 0, 0, 0, 0, 0).status;
    /* One budget per name across all header instantiations. Lock-free slots
     * cannot leave an inherited locked diagnostic mutex in a fork child. */
    static struct { uint64_t key, budget; } sites[64];
    uint64_t key = UINT64_C(14695981039346656037);
    for (const char *p = site; *p; ++p)
        key = (key ^ (unsigned char)*p) * UINT64_C(1099511628211);
    if (!key) key = 1;
    uint64_t *budget = 0;
    for (unsigned i = 0; i < 64; ++i) {
        uint64_t expected = 0;
        if (__atomic_compare_exchange_n(&sites[i].key, &expected, key, 0,
                __ATOMIC_RELAXED, __ATOMIC_RELAXED) || expected == key) {
            budget = &sites[i].budget;
            break;
        }
    }
    if (!budget) return;
    uint64_t old = __atomic_load_n(budget, __ATOMIC_RELAXED);
    for (;;) {
        uint64_t count = (old >> 32) == (uint32_t)pid ? (uint32_t)old : 0;
        if (count >= 4) return;
        uint64_t next = (pid << 32) | (count + 1);
        if (__atomic_compare_exchange_n(budget, &old, next, 0,
                __ATOMIC_RELAXED, __ATOMIC_RELAXED)) break;
    }
    char line[240];
    unsigned n = 0;
    const char *prefix = "[lpr-browser] EMFILE ";
    while (*prefix) line[n++] = *prefix++;
    while (*site && n < 140) line[n++] = *site++;
    uint64_t values[] = {pid, used, limit, detail};
    for (unsigned i = 0; i < 4; ++i) {
        line[n++] = ' ';
        for (int shift = 60; shift >= 0; shift -= 4)
            line[n++] = "0123456789abcdef"[(values[i] >> shift) & 15];
    }
    line[n++] = '\n';
    (void)pacha_native_syscall_retry6(PACHAOS_SYSCALL_LOG,
        (uintptr_t)line, n, 0, 0, 0, 0);
}

static int64_t lpr_emfile_native_result(uint64_t nr, int64_t status)
{
    if (pacha_kernel_status_to_errno(status) == -PACHA_LINUX_EMFILE) {
        struct pacha_fd_table_info info = {0};
        int64_t query = pacha_native_syscall_retry6(PACHAOS_SYSCALL_FD_TABLE,
            0, (uintptr_t)&info, 0, 0, 0, 0).status;
        lpr_emfile_diag_emit("native-result", query ? UINT64_MAX : info.capacity - info.free_slots,
            query ? UINT64_MAX : info.capacity, (nr << 32) | (uint32_t)status);
    }
    return status;
}

int64_t lpr_pacha_syscall0(uint64_t nr)
{
    lpr_native_fd_prepare(nr, 0);
    return lpr_emfile_native_result(nr, pacha_native_syscall_retry6(nr, 0, 0, 0, 0, 0, 0).status);
}
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t a0)
{
    lpr_native_fd_prepare(nr, 0);
    return lpr_emfile_native_result(nr, pacha_native_syscall_retry6(nr, a0, 0, 0, 0, 0, 0).status);
}
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    if (nr == PACHA_FD_SYSCALL_POLL || nr == PACHA_FD_SYSCALL_WAIT_MANY) return lpr_emfile_native_result(nr, pacha_poll_registers(nr, a0, a1, 0, 0));
    lpr_native_fd_prepare(nr, a1);
    return lpr_emfile_native_result(nr, pacha_native_syscall_retry6(nr, a0, a1, 0, 0, 0, 0).status);
}
int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2)
{
    if (nr == PACHA_FD_SYSCALL_POLL || nr == PACHA_FD_SYSCALL_WAIT_MANY) return lpr_emfile_native_result(nr, pacha_poll_registers(nr, a0, a1, a2, 0));
    lpr_native_fd_prepare(nr, a1);
    return lpr_emfile_native_result(nr, pacha_native_syscall_retry6(nr, a0, a1, a2, 0, 0, 0).status);
}
int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    if (nr == PACHA_FD_SYSCALL_POLL || nr == PACHA_FD_SYSCALL_WAIT_MANY) return lpr_emfile_native_result(nr, pacha_poll_registers(nr, a0, a1, a2, a3));
    lpr_native_fd_prepare(nr, a1);
    return lpr_emfile_native_result(nr, pacha_native_syscall_retry6(nr, a0, a1, a2, a3, 0, 0).status);
}
int64_t lpr_pacha_syscall5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    if (nr == PACHA_FD_SYSCALL_POLL || nr == PACHA_FD_SYSCALL_WAIT_MANY) return lpr_emfile_native_result(nr, pacha_poll_registers(nr, a0, a1, a2, a3));
    lpr_native_fd_prepare(nr, a1);
    return lpr_emfile_native_result(nr, pacha_native_syscall_retry6(nr, a0, a1, a2, a3, a4, 0).status);
}
int64_t lpr_pacha_syscall6(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    if (nr == PACHA_FD_SYSCALL_POLL || nr == PACHA_FD_SYSCALL_WAIT_MANY) return lpr_emfile_native_result(nr, pacha_poll_registers(nr, a0, a1, a2, a3));
    lpr_native_fd_prepare(nr, a1);
    return lpr_emfile_native_result(nr, pacha_native_syscall_retry6(nr, a0, a1, a2, a3, a4, a5).status);
}

/* Named callee-saved register operands make the compiler preserve RBX/R12/R13/R15
 * around the syscall while treating every payload register as an output. */
static long lpr_ipc_registers(uint64_t nr, uint64_t fd, uint64_t flags,
    uint64_t timeout, uint64_t words[4], struct pacha_ipc_extension *extension,
    int *committed) {
    /* Counts above the native limit are rejected before packing: truncating
     * them could turn an invalid FD transfer into a successful empty one. */
    if (extension && (extension->send_count > 19 || extension->receive_capacity > 19)) {
        extension->received_count = 0;
        if (committed) *committed = 0;
        return 1; /* native INVALID */
    }
    for (;;) {
        uint64_t status = nr, result = timeout;
        register uint64_t r10 __asm__("r10") = words[0];
        register uint64_t r8 __asm__("r8") = words[1];
        register uint64_t r9 __asm__("r9") = words[2];
        register uint64_t r12 __asm__("r12") = words[3];
        register uint64_t r13 __asm__("r13") = extension ? extension->send_array : 0;
        register uint64_t rbx __asm__("rbx") = extension ? extension->receive_array : 0;
        register uint64_t r15 __asm__("r15") = extension ?
            extension->send_count | (extension->receive_capacity << 32) : 0;
        __asm__ volatile("syscall"
            : "+a"(status), "+d"(result), "+r"(r10), "+r"(r8), "+r"(r9), "+r"(r12), "+r"(r15)
            : "D"(fd), "S"(flags), "r"(r13), "r"(rbx)
            : "rcx", "r11", "memory");
        if ((long)status == PACHA_SYSCALL_ERR_PAGER_ABSENT) {
            pacha_pager_retry_read(result);
            continue;
        }
        words[0] = r10; words[1] = r8; words[2] = r9; words[3] = r12;
        if (extension) extension->received_count = status == 0 ? r15 : 0;
        if (committed) *committed = (int)result;
        return (long)status;
    }
}
static inline long lpr_pack_ipc(uint64_t nr, uint64_t fd,
    const struct pacha_ipc_msg *send, struct pacha_ipc_msg *receive,
    uint64_t timeout, uint64_t flags, int *committed) {
    uint64_t words[4] = {0, 0, 0, 0};
    struct pacha_ipc_extension extension = {0};
    if (send) {
        words[0] = send->word0; words[1] = send->word1;
        words[2] = send->word2; words[3] = send->word3;
        extension.send_array = (uintptr_t)send->fds;
        extension.send_count = send->fd_count;
    }
    if (receive) {
        extension.receive_array = (uintptr_t)receive->fds;
        extension.receive_capacity = receive->fd_capacity;
    }
    long status = lpr_ipc_registers(nr, fd, flags, timeout, words,
        (extension.send_count || extension.receive_capacity) ? &extension : 0, committed);
    if (receive) {
        receive->word0 = words[0]; receive->word1 = words[1];
        receive->word2 = words[2]; receive->word3 = words[3];
        receive->fd_count = extension.received_count;
    }
    return status;
}


int64_t lpr_ipc_message(uint64_t nr, uint64_t fd, uint64_t send_va,
    uint64_t receive_va, uint64_t timeout, uint64_t flags, int *committed) {
    /* Capacity preparation is userspace policy, before the machine boundary. */
    if (receive_va) lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_RECV, receive_va);
    int64_t status = lpr_pack_ipc(nr, fd, (const void *)(uintptr_t)send_va,
        (void *)(uintptr_t)receive_va, timeout, flags, committed);
    return lpr_emfile_native_result(nr, status);
}

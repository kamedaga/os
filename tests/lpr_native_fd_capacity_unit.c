#include <assert.h>
#include <stdio.h>
#include "../userland/personality/linux/runtime/support/native_fd_capacity.h"

static struct pacha_fd_table_info current;
static unsigned calls;
static uint64_t requested;
static int query_failure, grow_failure;

int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t minimum, uint64_t output)
{
    assert(nr == PACHAOS_SYSCALL_FD_TABLE);
    ++calls;
    requested = minimum;
    if ((!minimum && query_failure) || (minimum && grow_failure)) return 1;
    *(struct pacha_fd_table_info *)(uintptr_t)output = current;
    return 0;
}

int main(void)
{
    /* A receive offering no FD capacity installs nothing: no probe. */
    struct pacha_ipc_msg no_fds = {0}, with_fds = {.fd_capacity = 4};
    current = (struct pacha_fd_table_info){ .capacity = 256, .maximum = 4096, .free_slots = 1 };
    lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_RECV_WAIT, (uint64_t)(uintptr_t)&no_fds);
    lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_RECV, (uint64_t)(uintptr_t)&no_fds);
    lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_CALL, 0);
    assert(calls == 0);
    lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_RECV_WAIT, (uint64_t)(uintptr_t)&with_fds);
    assert(calls == 2 && requested == 512);
    calls = 0;
    current = (struct pacha_fd_table_info){ .capacity = 256, .maximum = 4096, .free_slots = 21 };
    lpr_native_fd_prepare(PACHAOS_SYSCALL_FD_TABLE, 0);
    lpr_native_fd_prepare(PACHAOS_SYSCALL_MMAP, 0);
    lpr_native_fd_prepare(PACHAOS_SYSCALL_FD_CLOSE, 0);
    assert(calls == 0);
    lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_REPLY_EXTRACT, 0);
    assert(calls == 1 && requested == 0);
    current.free_slots = 20;
    calls = 0;
    lpr_native_fd_prepare(PACHAOS_SYSCALL_VMO_CREATE, 0);
    assert(calls == 2 && requested == 512);
    calls = 0;
    current.capacity = 3000;
    lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_RECV, 0);
    assert(calls == 2 && requested == 4096);
    calls = 0;
    current.capacity = 4096;
    lpr_native_fd_prepare(PACHAOS_SYSCALL_THREAD_CREATE, 0);
    assert(calls == 1);
    current.capacity = 256;
    query_failure = 1;
    calls = 0;
    lpr_native_fd_prepare(PACHAOS_SYSCALL_FD_DUP, 0);
    assert(calls == 1);
    query_failure = 0;
    grow_failure = 1;
    calls = 0;
    lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_RECV_WAIT, 0);
    assert(calls == 2); /* No retry loop, even when growth fails. */
    calls = 0;
    current.capacity = 0;
    lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_REPLY_EXTRACT, 0);
    assert(calls == 1);
    calls = 0;
    current.capacity = 256;
    current.maximum = (uint64_t)PACHAOS_THREAD_SELF_FD + 1;
    lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_REPLY_EXTRACT, 0);
    assert(calls == 1);
    grow_failure = 0;
    calls = 0;
    current.capacity = 4096;
    current.maximum = PACHAOS_THREAD_SELF_FD;
    lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_REPLY_EXTRACT, 0);
    assert(calls == 2 && requested == 8192);
    calls = 0;
    current.capacity = UINT64_C(0x80000000);
    lpr_native_fd_prepare(PACHAOS_SYSCALL_VMO_CREATE, 0);
    assert(calls == 2 && requested == PACHAOS_THREAD_SELF_FD);
    puts("LPR_NATIVE_FD_CAPACITY=OK");
    return 0;
}

#include "lpr_ipc_mock.h"
int64_t lpr_ipc_message(uint64_t nr, uint64_t fd, uint64_t send, uint64_t receive,
    uint64_t timeout, uint64_t flags, int *committed) {
    return test_lpr_ipc_noncall(nr, fd, send, receive, timeout, flags, committed);
}

int64_t lpr_pacha_syscall4(uint64_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    (void)nr;(void)a;(void)b;(void)c;(void)d;abort();
}

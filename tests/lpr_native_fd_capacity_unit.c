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
    current = (struct pacha_fd_table_info){ .capacity = 256, .maximum = 4096, .free_slots = 21 };
    lpr_native_fd_prepare(PACHAOS_SYSCALL_FD_TABLE);
    lpr_native_fd_prepare(PACHAOS_SYSCALL_MMAP);
    lpr_native_fd_prepare(PACHAOS_SYSCALL_FD_CLOSE);
    assert(calls == 0);
    lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_CALL);
    assert(calls == 1 && requested == 0);
    current.free_slots = 20;
    calls = 0;
    lpr_native_fd_prepare(PACHAOS_SYSCALL_VMO_CREATE);
    assert(calls == 2 && requested == 512);
    calls = 0;
    current.capacity = 3000;
    lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_RECV);
    assert(calls == 2 && requested == 4096);
    calls = 0;
    current.capacity = 4096;
    lpr_native_fd_prepare(PACHAOS_SYSCALL_THREAD_CREATE);
    assert(calls == 1);
    current.capacity = 256;
    query_failure = 1;
    calls = 0;
    lpr_native_fd_prepare(PACHAOS_SYSCALL_FD_DUP);
    assert(calls == 1);
    query_failure = 0;
    grow_failure = 1;
    calls = 0;
    lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_RECV_WAIT);
    assert(calls == 2); /* No retry loop, even when growth fails. */
    calls = 0;
    current.capacity = 0;
    lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_CALL);
    assert(calls == 1);
    calls = 0;
    current.capacity = 256;
    current.maximum = PACHAOS_FD_TABLE_LIMIT + 1;
    lpr_native_fd_prepare(PACHAOS_SYSCALL_IPC_CALL);
    assert(calls == 1);
    puts("LPR_NATIVE_FD_CAPACITY=OK");
    return 0;
}

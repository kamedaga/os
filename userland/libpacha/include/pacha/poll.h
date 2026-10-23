#ifndef PACHA_POLL_H
#define PACHA_POLL_H
#include <stdint.h>
#include "native_syscall_retry.h"
/* Shared FD_POLL=51 / FD_WAIT_MANY=52 wire record. Minimum is input-only;
 * zero requests one writable byte. revents is output-only. */
struct pacha_pollfd {
    uint64_t fd, events, min_write_bytes, revents;
};
_Static_assert(sizeof(struct pacha_pollfd) == 32, "native pollfd ABI");
/* R10/R8 belong to this syscall's output even though ordinary raw syscalls
 * preserve them. Clear only outputs: write minima survive retries. */
static inline long pacha_poll_registers(uint64_t nr, uint64_t address,
    uint64_t count, uint64_t timeout, uint64_t flags)
{
    struct pacha_pollfd *fds = (void *)(uintptr_t)address;
    const int poll = nr == 51 || nr == 52;
    if (!poll) return pacha_native_syscall_retry6(nr, address, count, timeout, flags, 0, 0).status;
    if (poll && fds && count <= 256)
        for (uint64_t i = 0; i < count; ++i) fds[i].revents = 0;
    uint64_t index = 0, events = 0, status = 0;
    for (;;) {
    register uint64_t input_index __asm__("r10") = flags;
    register uint64_t input_events __asm__("r8") = 0;
    uint64_t rdx = timeout;
#ifdef PACHA_POLL_TEST_INVOKE
    uint64_t test_index = input_index, test_events = input_events;
    status = PACHA_POLL_TEST_INVOKE(nr, address, count, timeout, &test_index, &test_events);
    index = test_index; events = test_events;
#else
    status = nr;
    uint64_t arg0 = address, arg1 = count;
    __asm__ volatile("syscall" : "+a"(status), "+D"(arg0), "+S"(arg1),
        "+d"(rdx), "+r"(input_index), "+r"(input_events)
        :
        : "rcx", "r11", "memory");
    index = input_index; events = input_events;
#endif
    if ((long)status != PACHA_SYSCALL_ERR_PAGER_ABSENT) break;
    pacha_pager_retry_read(rdx);
    }
    if (poll && status == 1 && events && index < count) fds[index].revents = events;
    return (long)status;
}
#endif

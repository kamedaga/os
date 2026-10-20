#ifndef PACHA_POLL_H
#define PACHA_POLL_H
#include <stdint.h>
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
    if (poll && fds && count <= 256)
        for (uint64_t i = 0; i < count; ++i) fds[i].revents = 0;
    register uint64_t index __asm__("r10") = flags;
    register uint64_t events __asm__("r8") = 0;
    uint64_t status;
#ifdef PACHA_POLL_TEST_INVOKE
    uint64_t test_index = index, test_events = events;
    status = PACHA_POLL_TEST_INVOKE(nr, address, count, timeout, &test_index, &test_events);
    index = test_index; events = test_events;
#else
    __asm__ volatile("syscall" : "=a"(status), "+r"(index), "+r"(events)
        : "a"(nr), "D"(address), "S"(count), "d"(timeout)
        : "rcx", "r11", "memory");
#endif
    if (poll && status == 1 && events && index < count) fds[index].revents = events;
    return (long)status;
}
#endif

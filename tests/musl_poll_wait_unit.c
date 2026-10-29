/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../musl/pachaos/syscall/poll_wait.h"

struct mock {
    uint64_t flags[12];
    long statuses[12];
    uint64_t revents[12];
    unsigned calls;
};

static long invoke(void *context, uint64_t address, uint64_t count,
    uint64_t timeout, uint64_t flags)
{
    struct mock *m = context;
    assert(count && count <= 256 && timeout == 10 && m->calls < 12);
    struct pacha_pollfd *fds = (void *)(uintptr_t)address;
    unsigned call = m->calls++;
    m->flags[call] = flags;
    fds[0].revents = m->revents[call];
    return m->statuses[call];
}

int main(void)
{
    struct pachaos_poll_wait_set set = {0}, other_thread = {0};
    struct pacha_pollfd fds[2] = {
        {.fd = 20, .events = 1}, {.fd = 21, .events = 1}};
    struct mock m = {0};
    m.statuses[0] = PACHA_SYSCALL_ERR_NOT_READY;
    m.statuses[1] = 1; m.revents[1] = 1;
    m.statuses[2] = 1; m.revents[2] = 1;
    m.statuses[3] = 1; m.revents[3] = 1;
    m.statuses[4] = PACHA_FD_WAIT_SET_INVALIDATED;
    m.statuses[5] = 1; m.revents[5] = 1;
    m.statuses[6] = 1; m.revents[6] = 1;
    assert(pachaos_poll_wait_retained(&set, fds, 2, 10, invoke, &m) == 2);
    assert(set.valid && m.flags[0] == PACHA_FD_WAIT_SET_REPLACE);
    assert(pachaos_poll_wait_retained(&set, fds, 2, 10, invoke, &m) == 1);
    assert(m.flags[1] == PACHA_FD_WAIT_SET_REUSE && fds[0].revents == 1);
    struct pacha_pollfd swapped[2] = {fds[1], fds[0]};
    assert(pachaos_poll_wait_retained(&set, swapped, 2, 10, invoke, &m) == 1);
    assert(m.flags[2] == PACHA_FD_WAIT_SET_REPLACE);
    swapped[0].min_write_bytes = 8;
    assert(pachaos_poll_wait_retained(&set, swapped, 2, 10, invoke, &m) == 1);
    assert(m.flags[3] == PACHA_FD_WAIT_SET_REPLACE);
    assert(pachaos_poll_wait_retained(&set, swapped, 2, 10, invoke, &m) == 1);
    assert(m.flags[4] == PACHA_FD_WAIT_SET_REUSE &&
        m.flags[5] == PACHA_FD_WAIT_SET_REPLACE);
    assert(pachaos_poll_wait_retained(&other_thread, swapped, 2, 10, invoke, &m) == 1);
    assert(m.flags[6] == PACHA_FD_WAIT_SET_REPLACE);
    puts("PachaOS musl poll retained set PASS");
}

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pacha/ipc.h"

static uint64_t flags_seen[16];
static unsigned calls, invalidate_once, not_ready_once, error_once;

long pacha_syscall4(uint64_t nr, uint64_t address, uint64_t count,
    uint64_t timeout, uint64_t flags)
{
    assert(nr == PACHA_FD_SYSCALL_WAIT_MANY);
    assert(count > 0 && count <= 256);
    assert(calls < sizeof(flags_seen) / sizeof(flags_seen[0]));
    flags_seen[calls++] = flags;
    struct pacha_pollfd *fds = (void *)(uintptr_t)address;
    for (uint64_t i = 0; i < count; ++i) fds[i].revents = 0;
    if (invalidate_once) {
        invalidate_once = 0;
        return PACHA_FD_WAIT_SET_INVALIDATED;
    }
    if (error_once) {
        error_once = 0;
        return PACHA_SYSCALL_ERR_INVALID;
    }
    if (not_ready_once) {
        not_ready_once = 0;
        return PACHA_SYSCALL_ERR_NOT_READY;
    }
    if (timeout == 0) return PACHA_SYSCALL_ERR_NOT_READY;
    fds[count - 1].revents = PACHA_FD_EVENT_READABLE;
    return 1;
}

int main(void)
{
    struct pacha_fd_wait_retained retained = {0};
    struct pacha_pollfd fds[257] = {
        {.fd = 16, .events = PACHA_FD_EVENT_READABLE},
        {.fd = 17, .events = PACHA_FD_EVENT_READABLE},
    };
    assert(pacha_fd_wait_many_retained(&retained, fds, 2, 1) == 1);
    assert(flags_seen[0] == PACHA_FD_WAIT_SET_REPLACE && retained.valid);
    assert(pacha_fd_wait_many_retained(&retained, fds, 2, 1) == 1);
    assert(flags_seen[1] == PACHA_FD_WAIT_SET_REUSE);
    fds[1].min_write_bytes = 4;
    assert(pacha_fd_wait_many_retained(&retained, fds, 2, 1) == 1);
    assert(flags_seen[2] == PACHA_FD_WAIT_SET_REPLACE);
    invalidate_once = 1;
    assert(pacha_fd_wait_many_retained(&retained, fds, 2, 1) == 1);
    assert(flags_seen[3] == PACHA_FD_WAIT_SET_REUSE);
    assert(flags_seen[4] == PACHA_FD_WAIT_SET_REPLACE);
    not_ready_once = 1;
    assert(pacha_fd_wait_many_retained(&retained, fds, 2, UINT64_MAX) == 1);
    assert(flags_seen[5] == PACHA_FD_WAIT_SET_REUSE);
    assert(flags_seen[6] == PACHA_FD_WAIT_SET_REUSE);
    error_once = 1;
    assert(pacha_fd_wait_many_retained(&retained, fds, 2, 1) == PACHA_ERR_INVALID);
    assert(pacha_fd_wait_many_retained(&retained, fds, 2, 0) == PACHA_ERR_NOT_READY);
    assert(flags_seen[8] == PACHA_FD_WAIT_SET_REUSE);
    for (unsigned i = 0; i < 257; ++i)
        fds[i] = (struct pacha_pollfd){.fd = 16 + i, .events = PACHA_FD_EVENT_READABLE};
    const unsigned before = calls;
    assert(pacha_fd_wait_many_retained(&retained, fds, 257, 0) == PACHA_ERR_NOT_READY);
    assert(calls == before + 2);
    for (unsigned i = before; i < calls; ++i) assert(flags_seen[i] == 0);
    assert(pacha_fd_wait_many_retained(NULL, fds, 1, 1) == PACHA_ERR_INVALID);
    struct pacha_fd_wait_retained another = {0};
    fds[0] = (struct pacha_pollfd){.fd = 16, .events = PACHA_FD_EVENT_READABLE};
    fds[1] = (struct pacha_pollfd){.fd = 17, .events = PACHA_FD_EVENT_READABLE};
    assert(pacha_fd_wait_many_retained(&another, fds, 2, 1) == 1);
    assert(flags_seen[11] == PACHA_FD_WAIT_SET_REPLACE);
    assert(pacha_fd_wait_many_retained(&another, fds, 2, 1) == 1);
    assert(flags_seen[12] == PACHA_FD_WAIT_SET_REUSE);
    struct pacha_pollfd first = fds[0]; fds[0] = fds[1]; fds[1] = first;
    assert(pacha_fd_wait_many_retained(&another, fds, 2, 1) == 1);
    assert(flags_seen[13] == PACHA_FD_WAIT_SET_REPLACE);
    fds[0].events = PACHA_FD_EVENT_HANGUP;
    assert(pacha_fd_wait_many_retained(&another, fds, 2, 1) == 1);
    assert(flags_seen[14] == PACHA_FD_WAIT_SET_REPLACE);
    static struct pacha_service_wait_set set;
    assert(pacha_service_wait_init(&set, 16) == 0);
    assert(pacha_service_wait_retained(&set, &another, 1) == 1);
    assert(flags_seen[15] == PACHA_FD_WAIT_SET_REPLACE);
    puts("LIBIPC_RETAINED_WAIT_UNIT=OK");
}

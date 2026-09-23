#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "pacha/ipc.h"
#include "pacha/syscall.h"
static int ready_fd, late_fd, fail_wait, sleeps, calls;
long pacha_syscall4(uint64_t op, uint64_t address, uint64_t count, uint64_t ticks, uint64_t flags)
{
    assert(op == PACHA_FD_SYSCALL_WAIT_MANY && !flags);
    ++calls;
    /* Negative control: the historical code returns this error at 257. */
    if (count > 256 || fail_wait) return PACHA_SYSCALL_ERR_INVALID;
    struct pacha_pollfd *fds = (void *)(uintptr_t)address;
    for (uint64_t i = 0; i < count; ++i) {
        fds[i].revents = fds[i].fd == ready_fd ? PACHA_FD_EVENT_READABLE : 0;
        if (fds[i].revents) return 1;
    }
    if (ticks) { ++sleeps; if (late_fd) ready_fd = late_fd; }
    return PACHA_SYSCALL_ERR_NOT_READY;
}
static void init(struct pacha_service_wait_set *set, unsigned count)
{
    ready_fd = late_fd = fail_wait = sleeps = calls = 0;
    assert(!pacha_service_wait_init(set, 16));
    for (unsigned i = 1; i < count; ++i)
        assert(!pacha_service_wait_add(set, 16 + i, PACHA_FD_EVENT_READABLE));
}
int main(void)
{
    struct pacha_service_wait_set set;
    init(&set, 256); ready_fd = 271;
    assert(pacha_service_wait(&set, PACHA_FD_WAIT_FOREVER) == 1 && calls == 1);
    init(&set, 257); ready_fd = 272;
    assert(pacha_service_wait(&set, PACHA_FD_WAIT_FOREVER) == 1 && !sleeps);
    assert(pacha_service_wait_revents(&set, 272) == PACHA_FD_EVENT_READABLE);
    init(&set, 1024); late_fd = 1039;
    assert(pacha_service_wait(&set, PACHA_FD_WAIT_FOREVER) == 1 && sleeps == 1);
    init(&set, 257);
    assert(pacha_service_wait(&set, 0) == PACHA_ERR_NOT_READY && !sleeps);
    assert(pacha_service_wait(&set, 3) == PACHA_ERR_NOT_READY && sleeps == 3);
    init(&set, 257); fail_wait = 1;
    assert(pacha_service_wait(&set, PACHA_FD_WAIT_FOREVER) == -PACHA_SYSCALL_ERR_INVALID);
    /* The dynamic caller must not silently inherit the convenience set's
     * 1024-element storage limit when it supplies a larger live array. */
    struct pacha_pollfd *dynamic = calloc(1301, sizeof(*dynamic));
    assert(dynamic);
    for (unsigned i = 0; i < 1301; ++i)
        dynamic[i] = (struct pacha_pollfd){.fd = 16 + i, .events = PACHA_FD_EVENT_READABLE};
    ready_fd = 1316; fail_wait = sleeps = calls = 0;
    assert(pacha_fd_wait_many_batched(dynamic, 1301, 0) == 1 && !sleeps);
    assert(dynamic[1300].revents == PACHA_FD_EVENT_READABLE);
    ready_fd = 0; late_fd = 1316;
    assert(pacha_fd_wait_many_batched(dynamic, 1301, PACHA_FD_WAIT_FOREVER) == 1 && sleeps == 1);
    free(dynamic);
    puts("service wait native batch bounds and late-event delivery: PASS");
}

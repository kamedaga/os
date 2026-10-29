/* SPDX-License-Identifier: MIT */
#include "host.h"

long ph_wait_many_retained(struct ph_wait_retained *cache,
    struct pacha_pollfd *fds, size_t count, uint64_t timeout) {
    PH_CHECK(cache && fds && count && count <= 256);
    int same = cache->valid && cache->count == count;
    for (size_t i = 0; same && i < count; ++i)
        same = cache->items[i].fd == (uint64_t)fds[i].fd &&
            cache->items[i].events == fds[i].events &&
            cache->items[i].minimum == fds[i].min_write_bytes;
    long result = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
        (uintptr_t)fds, count, timeout,
        same ? PACHA_FD_WAIT_SET_REUSE : PACHA_FD_WAIT_SET_REPLACE);
    if (result == PACHA_FD_WAIT_SET_INVALIDATED && same) {
        same = 0;
        result = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
            (uintptr_t)fds, count, timeout, PACHA_FD_WAIT_SET_REPLACE);
    }
    if (!same) {
        size_t ready = 0;
        for (size_t i = 0; i < count; ++i) ready += fds[i].revents != 0;
        cache->valid = (result > 0 && (size_t)result == ready) ||
            (result == PACHA_SYSCALL_ERR_NOT_READY && !ready);
        if (cache->valid) {
            cache->count = count;
            for (size_t i = 0; i < count; ++i) {
                cache->items[i].fd = (uint64_t)fds[i].fd;
                cache->items[i].events = fds[i].events;
                cache->items[i].minimum = fds[i].min_write_bytes;
            }
        }
    }
    return result;
}

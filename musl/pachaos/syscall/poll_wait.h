/* SPDX-License-Identifier: MIT */
#ifndef PACHAOS_NATIVE_POLL_WAIT_H
#define PACHAOS_NATIVE_POLL_WAIT_H

#include <stddef.h>
#include <stdint.h>
#include "../../../userland/libpacha/include/pacha/abi.h"
#include "../../../userland/libpacha/include/pacha/poll.h"

enum { PACHAOS_POLL_WAIT_MAX = 256 };

struct pachaos_poll_wait_set {
    size_t count;
    int valid;
    struct { uint64_t fd, events, minimum; } items[PACHAOS_POLL_WAIT_MAX];
};

typedef long (*pachaos_poll_wait_call)(void *context, uint64_t address,
    uint64_t count, uint64_t timeout, uint64_t flags);

static inline long pachaos_poll_wait_retained(struct pachaos_poll_wait_set *set,
    struct pacha_pollfd *fds, size_t count, uint64_t timeout,
    pachaos_poll_wait_call call, void *context)
{
    int same = set->valid && set->count == count;
    for (size_t i = 0; same && i < count; ++i)
        same = set->items[i].fd == fds[i].fd &&
            set->items[i].events == fds[i].events &&
            set->items[i].minimum == fds[i].min_write_bytes;
    for (size_t i = 0; i < count; ++i) fds[i].revents = 0;
    long status = call(context, (uint64_t)(uintptr_t)fds, count, timeout,
        same ? PACHA_FD_WAIT_SET_REUSE : PACHA_FD_WAIT_SET_REPLACE);
    if (status == PACHA_FD_WAIT_SET_INVALIDATED && same) {
        same = 0;
        for (size_t i = 0; i < count; ++i) fds[i].revents = 0;
        status = call(context, (uint64_t)(uintptr_t)fds, count, timeout,
            PACHA_FD_WAIT_SET_REPLACE);
    }
    if (!same) {
        size_t ready = 0;
        for (size_t i = 0; i < count; ++i) ready += fds[i].revents != 0;
        set->valid = (status > 0 && (size_t)status == ready) ||
            (status == PACHA_SYSCALL_ERR_NOT_READY && ready == 0);
        if (set->valid) {
            set->count = count;
            for (size_t i = 0; i < count; ++i) {
                set->items[i].fd = fds[i].fd;
                set->items[i].events = fds[i].events;
                set->items[i].minimum = fds[i].min_write_bytes;
            }
        }
    }
    return status;
}

#endif

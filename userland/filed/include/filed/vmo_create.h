#pragma once

#include "pacha/abi.h"
#include "pacha/ipc.h"

/* VMO creation has no published object on failure, unlike an IPC request
 * which must never be replayed. Grow an exhausted descriptor table before
 * treating allocation failure as memory pressure and evicting file data.
 * The success path needs no extra syscall or cached capacity bookkeeping. */
static inline int filed_vmo_create(uint64_t size, uint64_t rights, uint32_t flags)
{
    const int fd = pacha_vmo_create(size, rights, flags);
    if (fd != PACHA_ERR_ALLOC) return fd;

    struct pacha_fd_table_info info = {0};
    if (pacha_fd_table(0, &info) != 0 || info.free_slots != 0 ||
        info.capacity == 0 || info.capacity >= info.maximum ||
        info.maximum > PACHA_FD_TABLE_LIMIT)
        return fd;
    const uint64_t old_capacity = info.capacity;
    const uint64_t target = info.capacity > info.maximum / 2
        ? info.maximum : info.capacity * 2;
    if (pacha_fd_table(target, &info) != 0 ||
        info.capacity <= old_capacity || info.free_slots == 0)
        return fd;

    /* One bounded retry: a concurrent allocation or actual memory shortage
     * still returns the original operation's normal error to its caller. */
    return pacha_vmo_create(size, rights, flags);
}

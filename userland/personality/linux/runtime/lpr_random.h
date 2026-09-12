#ifndef LPR_RANDOM_H
#define LPR_RANDOM_H

#include "lpr_error.h"
#include "support/syscall.h"
#include <pachaos/abi.h>
#include <stdint.h>

static inline int64_t lpr_linux_getrandom(uint64_t buf, uint64_t count, uint64_t flags)
{
    /* GRND_NONBLOCK changes only waiting. The existing native generator is
     * immediately available. Other entropy modes are not implemented here. */
    if ((flags & ~1ull) != 0 || count > INT64_MAX) return -LPR_LINUX_EINVAL;
    if (count == 0) return 0;
    if (buf == 0 || count > UINT64_MAX - buf) return -LPR_LINUX_EFAULT;
    uint64_t done = 0;
    while (done < count) {
        const uint64_t chunk = count - done > 4096 ? 4096 : count - done;
        const int64_t got = lpr_pacha_syscall3(PACHAOS_SYSCALL_GETRANDOM, buf + done, chunk, 0);
        if (got < 0) {
            if (done != 0) return (int64_t)done;
            if (got == -PACHAOS_SYSCALL_ERR_INVALID) return -LPR_LINUX_EINVAL;
            if (got == -PACHAOS_SYSCALL_ERR_MAP) return -LPR_LINUX_EFAULT;
            return -LPR_LINUX_EIO;
        }
        if ((uint64_t)got > chunk) return done != 0 ? (int64_t)done : -LPR_LINUX_EIO;
        done += (uint64_t)got;
        if ((uint64_t)got < chunk) break;
    }
    return (int64_t)done;
}

#endif

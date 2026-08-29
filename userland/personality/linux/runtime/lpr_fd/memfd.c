#include "memfd.h"

#include <pacha/status.h>

int64_t lpr_memfd_add_seals(uint8_t *state, uint64_t seals)
{
    if (state == 0) {
        return -LPR_LINUX_EFAULT;
    }
    if ((*state & LPR_FILED_FD_MEMFD) == 0 ||
        (seals & ~(uint64_t)LPR_FILED_FD_SEALS) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    if ((*state & LPR_FILED_FD_ALLOW_SEALING) == 0 ||
        ((*state & LPR_LINUX_F_SEAL_SEAL) != 0 &&
            (seals & ~(uint64_t)(*state & LPR_FILED_FD_SEALS)) != 0)) {
        return -LPR_LINUX_EPERM;
    }
    *state |= (uint8_t)seals;
    return 0;
}

int lpr_memfd_write_is_sealed(uint8_t state)
{
    return (state & LPR_FILED_FD_MEMFD) != 0 &&
        (state & (LPR_LINUX_F_SEAL_WRITE |
                  LPR_LINUX_F_SEAL_FUTURE_WRITE)) != 0;
}

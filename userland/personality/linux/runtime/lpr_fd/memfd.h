#ifndef LPR_FD_MEMFD_H
#define LPR_FD_MEMFD_H

#include <stdint.h>

enum {
    LPR_LINUX_F_SEAL_SEAL = 0x01u,
    LPR_LINUX_F_SEAL_SHRINK = 0x02u,
    LPR_LINUX_F_SEAL_GROW = 0x04u,
    LPR_LINUX_F_SEAL_WRITE = 0x08u,
    LPR_LINUX_F_SEAL_FUTURE_WRITE = 0x10u,
    LPR_FILED_FD_ALLOW_SEALING = 0x40u,
    LPR_FILED_FD_MEMFD = 0x80u,
};

#define LPR_FILED_FD_SEALS 0x1fu

int64_t lpr_memfd_add_seals(uint8_t *state, uint64_t seals);
int lpr_memfd_write_is_sealed(uint8_t state);

#endif

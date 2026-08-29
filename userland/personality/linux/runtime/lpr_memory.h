#ifndef LPR_MEMORY_H
#define LPR_MEMORY_H

#include <stdint.h>

int64_t lpr_linux_brk(uint64_t requested);
int64_t lpr_linux_mremap(
    uint64_t old_address,
    uint64_t old_size,
    uint64_t new_size,
    uint64_t flags,
    uint64_t new_address);

#endif

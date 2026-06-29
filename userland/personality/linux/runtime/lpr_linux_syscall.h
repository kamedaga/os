#ifndef LPR_LINUX_SYSCALL_H
#define LPR_LINUX_SYSCALL_H

#include <personality/linux_lpr.h>

int64_t lpr_dispatch_syscall(uint64_t nr,
                             uint64_t a0,
                             uint64_t a1,
                             uint64_t a2,
                             uint64_t a3,
                             uint64_t a4,
                             uint64_t a5);

#endif

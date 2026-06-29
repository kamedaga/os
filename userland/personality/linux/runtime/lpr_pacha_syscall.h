#ifndef LPR_PACHA_SYSCALL_H
#define LPR_PACHA_SYSCALL_H

#include <stdint.h>

int64_t lpr_pacha_syscall0(uint64_t nr);
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t a0);
int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2);

#endif

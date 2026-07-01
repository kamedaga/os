#ifndef LPR_SUPPORT_SYSCALL_H
#define LPR_SUPPORT_SYSCALL_H

#include <stdint.h>

int64_t lpr_pacha_syscall0(uint64_t nr);
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t a0);
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1);
int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2);
int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
int64_t lpr_pacha_syscall5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int64_t lpr_pacha_syscall6(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

#endif

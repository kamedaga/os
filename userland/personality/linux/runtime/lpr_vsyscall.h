#ifndef LPR_VSYSCALL_H
#define LPR_VSYSCALL_H
#include <stdint.h>

uintptr_t lpr_vsyscall_fault_target(uint64_t vector, uint64_t error, uint64_t rip);

#endif

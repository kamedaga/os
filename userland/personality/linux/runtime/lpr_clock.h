#ifndef LPR_CLOCK_H
#define LPR_CLOCK_H
#include <stdint.h>

/* 0 only for an accepted sample in the kernel monotonic domain; all doubt
 * returns 1 and leaves the caller on its existing native clock syscall. */
int lpr_clock_monotonic(uint64_t *nanoseconds);
void lpr_clock_after_fork_child(void);
void lpr_clock_fork_lock(void);
void lpr_clock_fork_unlock(void);
int64_t lpr_clock_copy_timespec(void *output, uint64_t seconds, uint64_t nanos);
uintptr_t lpr_clock_copy_fault_target(uint64_t vector, uint64_t error, uint64_t rip);
#endif

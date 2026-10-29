#ifndef LPR_MLOCK_H
#define LPR_MLOCK_H
#include <stdint.h>

int64_t lpr_linux_mlock(uint64_t address,uint64_t length,uint64_t limit);
int64_t lpr_linux_munlock(uint64_t address,uint64_t length);
/* Serialize application mapping replacement with the small set of locked
 * intervals. Successful begin owns the lock and reserves one possible split. */
int64_t lpr_mlock_vm_begin(void);
void lpr_mlock_vm_end(void);
void lpr_mlock_forget(uint64_t address,uint64_t length);
int lpr_mlock_overlaps(uint64_t address,uint64_t length);
volatile uint32_t *lpr_mlock_lock_address(void);
void lpr_mlock_fork_lock(void);
void lpr_mlock_after_fork_child(void);
#endif

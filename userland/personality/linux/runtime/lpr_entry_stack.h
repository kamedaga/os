/* SPDX-License-Identifier: MIT */
#ifndef LPR_ENTRY_STACK_H
#define LPR_ENTRY_STACK_H
#define LPR_ENTRY_BOOTSTRAP_BYTES 65536
#ifndef __ASSEMBLER__
#include <stdint.h>
struct lpr_entry_stack_result { uintptr_t stack; uint64_t release_bootstrap; };
struct lpr_entry_stack_result lpr_entry_stack_prepare(uintptr_t captured_stack);
uintptr_t lpr_entry_stack_acquire_bootstrap(void);
void lpr_entry_stack_release_bootstrap(void);
void lpr_entry_stack_initialize_current(void);
void lpr_entry_stack_after_fork_child(void);
void lpr_entry_stack_async_range(uint64_t *base, uint64_t *size);
#endif
#endif

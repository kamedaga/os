/* SPDX-License-Identifier: MIT */
#ifndef PH_TAIL_TRACE_H
#define PH_TAIL_TRACE_H
#include "arch/x86_64/fp_entry.h"
#ifdef PH_TAIL_TRACE
KOBOX_GPR_LEAF uint64_t ph_tail_begin(void);
KOBOX_GPR_LEAF void ph_tail_end(unsigned kind, uintptr_t object, uint64_t begin);
void ph_tail_console(const char *text, size_t length);
#else
static inline uint64_t ph_tail_begin(void) { return 0; }
static inline void ph_tail_end(unsigned kind, uintptr_t object, uint64_t begin)
{ (void)kind; (void)object; (void)begin; }
static inline void ph_tail_console(const char *text, size_t length)
{ (void)text; (void)length; }
#endif
#endif

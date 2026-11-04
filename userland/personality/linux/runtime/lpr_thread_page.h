/* SPDX-License-Identifier: MIT */
#ifndef LPR_THREAD_PAGE_H
#define LPR_THREAD_PAGE_H
#ifndef __ASSEMBLER__
#include <stdint.h>
extern const volatile unsigned char *lpr_thread_fast_page;
void lpr_thread_page_initialize(void);
void lpr_thread_page_after_fork_child(void);
void lpr_thread_page_fork_lock(void);
void lpr_thread_page_fork_unlock(void);
uint64_t lpr_thread_page_read(void); /* 0 means GETTID fallback; never initializes. */
#endif
#endif

#ifndef LPR_WAIT_TRACE_H
#define LPR_WAIT_TRACE_H

#include <stdint.h>

/* Diagnostic-only user timeline. The normal runtime emits no records. */
#if defined(LPR_WAIT_TRACE) && LPR_WAIT_TRACE
void lpr_wait_trace_record(uint32_t stage, uint64_t nr, uint64_t value);
void lpr_wait_trace_dump(void);
#else
#define lpr_wait_trace_record(stage, nr, value) ((void)0)
#define lpr_wait_trace_dump() ((void)0)
#endif

#endif

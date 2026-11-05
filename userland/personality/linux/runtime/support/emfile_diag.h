#pragma once
#include <stdint.h>

/* Optional to standalone host fixtures; the runtime provides the
 * implementation in syscall.c. Each return site has its own PID-aware budget. */
void lpr_emfile_diag_emit(const char *site, uint64_t used, uint64_t limit,
    uint64_t detail) __attribute__((weak));
#define lpr_emfile_diag(site, used, limit, detail) do { \
    if (lpr_emfile_diag_emit) \
        lpr_emfile_diag_emit(site, used, limit, detail); \
} while (0)

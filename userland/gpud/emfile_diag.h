/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

/* Standalone ledger host tests need no native transport. */
void gpud_emfile_diag_emit(const char *site, uint64_t used, uint64_t limit,
    uint64_t detail) __attribute__((weak));
#define gpud_emfile_diag(site, used, limit, detail) do { \
    static unsigned emfile_seen; \
    if (emfile_seen < 4 && gpud_emfile_diag_emit) { \
        ++emfile_seen; \
        gpud_emfile_diag_emit(site, used, limit, detail); \
    } \
} while (0)

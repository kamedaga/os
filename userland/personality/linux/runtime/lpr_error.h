#pragma once

#include <stdint.h>

void lpr_errconv_record(
    uint64_t domain,
    uint64_t op,
    uint64_t stage,
    int64_t status,
    int64_t raw_status,
    uint64_t request_id,
    uint64_t fd_count,
    uint64_t subject,
    uint64_t child_token,
    const char *text);

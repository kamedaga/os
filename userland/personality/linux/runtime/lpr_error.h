#pragma once

#include <stdint.h>

enum lpr_error_domain {
    LPR_ERROR_DOMAIN_KERNEL = 1,
    LPR_ERROR_DOMAIN_FILED = 2,
    LPR_ERROR_DOMAIN_TERMD = 3,
    LPR_ERROR_DOMAIN_LPRS = 4,
};

enum lpr_error_stage {
    LPR_ERROR_STAGE_CHILD_RPC_CALL = 3,
    LPR_ERROR_STAGE_CHILD_RPC_RECV = 4,
    LPR_ERROR_STAGE_REPLY_MAGIC = 5,
    LPR_ERROR_STAGE_CHILD_STATUS = 6,
};

void lpr_trace_error_record(
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

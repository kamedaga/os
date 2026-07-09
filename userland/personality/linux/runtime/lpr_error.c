#include "lpr_error.h"

#include <pacha/trace.h>

#include <stdint.h>

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
    const char *text)
{
    pacha_trace6(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_GENERIC_ERROR,
        PACHA_TRACE_CLASS_ERROR,
        domain,
        op,
        stage,
        (uint64_t)status,
        (uint64_t)raw_status,
        request_id);
    pacha_trace6(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_GENERIC_ERROR,
        PACHA_TRACE_CLASS_ERROR,
        fd_count,
        subject,
        child_token,
        text != 0 ? pacha_trace_name_id(text) : 0,
        0,
        0);
}

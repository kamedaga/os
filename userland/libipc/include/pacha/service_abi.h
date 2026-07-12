#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    PACHA_SERVICE_ABI_VERSION = 2u,
    PACHA_SERVICE_REQUEST_MAGIC = 0x3251455256434150ull,
    PACHA_SERVICE_REPLY_MAGIC = 0x3259505256434150ull,

    PACHA_SERVICE_HEADER_BYTES = 64u,
    PACHA_SERVICE_PAGE_BYTES = 8192u,

    PACHA_SERVICE_ID_FILED = 1u,
    PACHA_SERVICE_ID_STORAGE = 2u,
    PACHA_SERVICE_ID_TERMD = 3u,
    PACHA_SERVICE_ID_DRMD = 4u,
    PACHA_SERVICE_ID_LPRS = 5u,
    PACHA_SERVICE_ID_LPR_CLIENT = 6u,

    PACHA_SERVICE_FLAG_PAGE_PAYLOAD = 1u << 0,
    PACHA_SERVICE_FLAG_FD_PAYLOAD = 1u << 1,
    PACHA_SERVICE_FLAG_DIAGNOSTIC = 1u << 2,

    PACHA_SERVICE_STATUS_OK = 0,

    PACHA_SERVICE_ERROR_NONE = 0u,
    PACHA_SERVICE_ERROR_ABI = 1u,
    PACHA_SERVICE_ERROR_LPR_TRANSLATION = 2u,
    PACHA_SERVICE_ERROR_FILED_VFS = 3u,
    PACHA_SERVICE_ERROR_FILED_EXEC = 4u,
    PACHA_SERVICE_ERROR_STORAGE_BACKEND = 5u,
    PACHA_SERVICE_ERROR_TERMD_TTY = 6u,
    PACHA_SERVICE_ERROR_DRMD_DRM = 7u,
    PACHA_SERVICE_ERROR_INPUTD_INPUT = 8u,
    PACHA_SERVICE_ERROR_DYNAMIC_LOADER = 9u,
};

typedef struct pacha_service_envelope {
    uint64_t magic;
    uint32_t abi_version;
    uint32_t service_id;
    uint32_t op;
    uint32_t flags;
    uint64_t request_id;
    uint64_t trace_id;
    union {
        struct {
            uint32_t payload_size;
            uint32_t fd_count;
        };
        /* Service boundary status: 0 or a negative Linux errno from pacha/status.h. */
        int64_t status;
    };
    union {
        struct {
            uint64_t reserved0;
            uint64_t reserved1;
        };
        struct {
            uint32_t error_domain;
            uint32_t reply_payload_size;
            uint64_t result;
        };
    };
} pacha_service_envelope_t;

static inline int pacha_service_request_is_valid(
    const pacha_service_envelope_t *header,
    uint32_t service_id)
{
    return header != NULL &&
        header->magic == PACHA_SERVICE_REQUEST_MAGIC &&
        header->abi_version == PACHA_SERVICE_ABI_VERSION &&
        header->service_id == service_id &&
        header->payload_size <= PACHA_SERVICE_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES;
}

static inline void pacha_service_reply_init(
    pacha_service_envelope_t *header,
    const pacha_service_envelope_t *request,
    int64_t status,
    uint32_t error_domain,
    uint64_t result,
    uint32_t payload_size)
{
    if (header == NULL) {
        return;
    }
    header->magic = PACHA_SERVICE_REPLY_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = request != NULL ? request->service_id : 0;
    header->op = request != NULL ? request->op : 0;
    header->flags = request != NULL ? request->flags : 0;
    header->request_id = request != NULL ? request->request_id : 0;
    header->trace_id = request != NULL ? request->trace_id : 0;
    header->status = status;
    header->error_domain = status == 0 ? PACHA_SERVICE_ERROR_NONE : error_domain;
    header->reply_payload_size = payload_size;
    header->result = result;
}

_Static_assert(sizeof(pacha_service_envelope_t) == PACHA_SERVICE_HEADER_BYTES,
    "pacha_service_envelope size");
_Static_assert(offsetof(pacha_service_envelope_t, payload_size) == 40,
    "pacha_service_envelope request payload_size offset");
_Static_assert(offsetof(pacha_service_envelope_t, status) == 40,
    "pacha_service_envelope reply status offset");
_Static_assert(offsetof(pacha_service_envelope_t, reply_payload_size) == 52,
    "pacha_service_envelope reply payload_size offset");

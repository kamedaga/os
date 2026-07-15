#pragma once

#include <stdint.h>

#include "pacha/service_abi.h"
#include "drmd/kms_abi.h"

enum {
    DRMD_SERVICE_ID = PACHA_SERVICE_ID_DRMD,

    DRMD_OP_HELLO = 0u,
    DRMD_OP_OPEN_CARD = 1u,
    DRMD_OP_HANDLE_CLOSE = 2u,
    DRMD_OP_HANDLE_DUP = 3u,
    DRMD_OP_HANDLE_IOCTL = 4u,
    DRMD_OP_HANDLE_MMAP = 5u,
    DRMD_OP_HANDLE_READ = 6u,
    DRMD_OP_HANDLE_POLL = 7u,
    DRMD_OP_PRIME_EXPORT = 8u,
    DRMD_OP_PRIME_IMPORT = 9u,
    DRMD_OP_PRIME_IMPORT_SYNC_FILE = 10u,
    DRMD_OP_PRIME_RELEASE = 11u,
    DRMD_OP_PRIME_ACQUIRE = 12u,

    DRMD_PAGE_BYTES = PACHA_SERVICE_PAGE_BYTES,
    DRMD_IOCTL_DATA_BYTES = 3072u,
    DRMD_EVENT_READ_BYTES = 512u,
    DRMD_VERSION_NAME_BYTES = 64u,
    DRMD_VERSION_DATE_BYTES = 32u,
    DRMD_VERSION_DESC_BYTES = 128u,
};

typedef struct drmd_open_request {
    uint64_t card_index;
    uint64_t flags;
} drmd_open_request_t;

typedef struct drmd_handle_request {
    uint64_t handle;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
} drmd_handle_request_t;

typedef struct drmd_ioctl_request {
    uint64_t handle;
    uint64_t request;
    uint64_t arg_size;
    uint64_t data_size;
    uint8_t data[DRMD_IOCTL_DATA_BYTES];
} drmd_ioctl_request_t;

typedef struct drmd_mmap_request {
    uint64_t handle;
    uint64_t length;
    uint64_t prot;
    uint64_t flags;
    uint64_t offset;
} drmd_mmap_request_t;

typedef struct drmd_read_request {
    uint64_t handle;
    uint64_t capacity;
    uint64_t data_size;
    uint8_t data[DRMD_EVENT_READ_BYTES];
} drmd_read_request_t;

typedef struct drmd_prime_export_request {
    uint64_t handle;
    uint32_t gem_handle;
    uint32_t flags;
} drmd_prime_export_request_t;

typedef struct drmd_prime_import_request {
    uint64_t handle;
    uint64_t token;
    uint64_t size;
    uint32_t flags;
    uint32_t reserved0;
} drmd_prime_import_request_t;

typedef struct drmd_prime_token_request {
    uint64_t token;
} drmd_prime_token_request_t;

typedef struct drmd_version_wire {
    int32_t major;
    int32_t minor;
    int32_t patchlevel;
    uint32_t reserved0;
    uint64_t name_capacity;
    uint64_t date_capacity;
    uint64_t desc_capacity;
    uint64_t name_length;
    uint64_t date_length;
    uint64_t desc_length;
    char name[DRMD_VERSION_NAME_BYTES];
    char date[DRMD_VERSION_DATE_BYTES];
    char desc[DRMD_VERSION_DESC_BYTES];
} drmd_version_wire_t;

_Static_assert(sizeof(drmd_open_request_t) == 16, "drmd_open_request size");
_Static_assert(sizeof(drmd_handle_request_t) == 32, "drmd_handle_request size");
_Static_assert(sizeof(drmd_ioctl_request_t) == 3104, "drmd_ioctl_request size");
_Static_assert(sizeof(drmd_mmap_request_t) == 40, "drmd_mmap_request size");
_Static_assert(sizeof(drmd_read_request_t) == 536, "drmd_read_request size");
_Static_assert(sizeof(drmd_prime_export_request_t) == 16, "drmd_prime_export_request size");
_Static_assert(sizeof(drmd_prime_import_request_t) == 32, "drmd_prime_import_request size");
_Static_assert(sizeof(drmd_prime_token_request_t) == 8, "drmd_prime_token_request size");
_Static_assert(sizeof(drmd_version_wire_t) == 288, "drmd_version_wire size");

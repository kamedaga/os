#pragma once

#include <stdint.h>

#include "pacha/service_abi.h"
#include "gpud/drm_kms_abi.h"
#include "gpud/drm_virtgpu_abi.h"

enum {
    GPUD_DRM_SERVICE_ID = PACHA_SERVICE_ID_GPUD_DRM,

    GPUD_DRM_OP_HELLO = 0u,
    GPUD_DRM_OP_OPEN_NODE = 1u,
    GPUD_DRM_OP_HANDLE_CLOSE = 2u,
    GPUD_DRM_OP_HANDLE_DUP = 3u,
    GPUD_DRM_OP_HANDLE_IOCTL = 4u,
    GPUD_DRM_OP_HANDLE_MMAP = 5u,
    GPUD_DRM_OP_HANDLE_READ = 6u,
    GPUD_DRM_OP_HANDLE_POLL = 7u,
    GPUD_DRM_OP_PRIME_EXPORT = 8u,
    GPUD_DRM_OP_PRIME_IMPORT = 9u,
    GPUD_DRM_OP_PRIME_IMPORT_SYNC_FILE = 10u,
    GPUD_DRM_OP_PRIME_RELEASE = 11u,
    GPUD_DRM_OP_PRIME_ACQUIRE = 12u,

    GPUD_DRM_PAGE_BYTES = PACHA_SERVICE_PAGE_BYTES,
    GPUD_DRM_IOCTL_DATA_BYTES = 3072u,
    GPUD_DRM_EVENT_READ_BYTES = 512u,
    GPUD_DRM_VERSION_NAME_BYTES = 64u,
    GPUD_DRM_VERSION_DATE_BYTES = 32u,
    GPUD_DRM_VERSION_DESC_BYTES = 128u,
    GPUD_DRM_IOCTL_AUX_MAX_BYTES = 64u * 1024u * 1024u,

    /* FD order: page, optional aux, optional input, optional output, reply. */
    GPUD_DRM_IOCTL_FD_INPUT_WAIT = 1u << 0,
    GPUD_DRM_IOCTL_FD_OUTPUT_NOTIFY = 1u << 1,
    GPUD_DRM_IOCTL_FD_MASK = GPUD_DRM_IOCTL_FD_INPUT_WAIT |
        GPUD_DRM_IOCTL_FD_OUTPUT_NOTIFY,
};

typedef struct gpud_drm_open_request {
    uint64_t device_minor;
    uint64_t flags;
} gpud_drm_open_request_t;

typedef struct gpud_drm_handle_request {
    uint64_t handle;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
} gpud_drm_handle_request_t;

typedef struct gpud_drm_ioctl_request {
    uint64_t handle;
    uint64_t request;
    uint64_t arg_size;
    uint64_t data_size;
    uint64_t aux_size;
    uint32_t fd_flags;
    uint32_t reserved0;
    uint8_t data[GPUD_DRM_IOCTL_DATA_BYTES];
} gpud_drm_ioctl_request_t;

typedef struct gpud_drm_mmap_request {
    uint64_t handle;
    uint64_t length;
    uint64_t prot;
    uint64_t flags;
    uint64_t offset;
} gpud_drm_mmap_request_t;

typedef struct gpud_drm_read_request {
    uint64_t handle;
    uint64_t capacity;
    uint64_t data_size;
    uint8_t data[GPUD_DRM_EVENT_READ_BYTES];
} gpud_drm_read_request_t;

typedef struct gpud_drm_prime_export_request {
    uint64_t handle;
    uint32_t gem_handle;
    uint32_t flags;
} gpud_drm_prime_export_request_t;

typedef struct gpud_drm_prime_import_request {
    uint64_t handle;
    uint64_t token;
    uint64_t size;
    uint32_t flags;
    uint32_t reserved0;
} gpud_drm_prime_import_request_t;

typedef struct gpud_drm_prime_token_request {
    uint64_t token;
} gpud_drm_prime_token_request_t;

typedef struct gpud_drm_version_wire {
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
    char name[GPUD_DRM_VERSION_NAME_BYTES];
    char date[GPUD_DRM_VERSION_DATE_BYTES];
    char desc[GPUD_DRM_VERSION_DESC_BYTES];
} gpud_drm_version_wire_t;

_Static_assert(sizeof(gpud_drm_open_request_t) == 16, "gpud_drm_open_request size");
_Static_assert(sizeof(gpud_drm_kms_connector_wire_t) <= GPUD_DRM_IOCTL_DATA_BYTES,
    "DRM connector wire must fit in an ioctl page");
_Static_assert(sizeof(gpud_drm_handle_request_t) == 32, "gpud_drm_handle_request size");
_Static_assert(sizeof(gpud_drm_ioctl_request_t) == 3120, "gpud_drm_ioctl_request size");
_Static_assert(sizeof(gpud_drm_mmap_request_t) == 40, "gpud_drm_mmap_request size");
_Static_assert(sizeof(gpud_drm_read_request_t) == 536, "gpud_drm_read_request size");
_Static_assert(sizeof(gpud_drm_prime_export_request_t) == 16, "gpud_drm_prime_export_request size");
_Static_assert(sizeof(gpud_drm_prime_import_request_t) == 32, "gpud_drm_prime_import_request size");
_Static_assert(sizeof(gpud_drm_prime_token_request_t) == 8, "gpud_drm_prime_token_request size");
_Static_assert(sizeof(gpud_drm_version_wire_t) == 288, "gpud_drm_version_wire size");

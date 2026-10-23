/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <kobox2/filesystem.h>
#include "pacha/service_abi.h"
#define FILED_LEASE_MAGIC UINT64_C(0x31455341454c4446)
/* One dev filesystem catalog, no legacy opcode translation. The host-specific
 * service and VMO policies below are not Linux filesystem flags. */
enum {
    FILED_SERVICE_ID = PACHA_SERVICE_ID_FILED,
    FILED_FILE_VMO_WRITE = 1u << 0,
    FILED_FILE_VMO_EXEC = 1u << 1,
    /* FILE_VMO response.service: the returned FD is a pager VMO. */
    FILED_FILE_VMO_PAGER = 1u,
    FILED_SERVICE_NETD_SOCKET = 1,
    FILED_SERVICE_TERMD_TTY = 2,
    FILED_SERVICE_GPUD_DRM = 3,
    FILED_SERVICE_INPUTD_INPUT = 4,
};

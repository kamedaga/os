#pragma once

#include <stdint.h>

#include "pacha/service_abi.h"

/* Reply word1 is 0 or a negative Linux errno. */

enum {
    KOBOXD_ENDPOINT_CONTROL = 1,
    KOBOXD_ENDPOINT_BLOCK = 2,
    KOBOXD_ENDPOINT_FS_BACKEND = 3,
    KOBOXD_ENDPOINT_EVENT = 4,
    KOBOXD_ENDPOINT_FILED = 5,

    KOBOXD_CONTROL_GET_ENDPOINT = 0u,
    KOBOXD_BLOCK_IDENTIFY = 0u,
};

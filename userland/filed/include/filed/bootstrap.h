#pragma once

#include <stdint.h>

#include "pacha/abi.h"

enum {
    FILED_BOOTSTRAP_MAGIC = 0x3144544f4f424446ull,
};

typedef struct filed_bootstrap {
    uint64_t magic;
    uint64_t fs_backend_fd;
    uint64_t flags;
} filed_bootstrap_t;

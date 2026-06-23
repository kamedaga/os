#pragma once

#include <stdint.h>

enum {
    FILED_SMOKE_BOOTSTRAP_MAGIC = 0x314b4d53444c4946ull,
};

typedef struct filed_smoke_bootstrap {
    uint64_t magic;
    uint64_t public_endpoint_fd;
    uint64_t flags;
    uint64_t reserved0;
} filed_smoke_bootstrap_t;

#pragma once

#include "storage/stack.h"

#include <stdint.h>
#include <string.h>

enum {
    STORAGE_SEED0ROOT_BOOTSTRAP_MAGIC = 0x305254424f4f5453ull,
    STORAGE_FILED_BOOTSTRAP_MAGIC = 0x3150474b42584f4bull,
};

typedef struct storage_module_image_desc {
    char name[STORAGE_STACK_MODULE_NAME_BYTES];
    uint64_t image_fd;
    uint64_t image_size;
} storage_module_image_desc_t;

typedef struct storage_seed0root_bootstrap {
    uint64_t magic;
    uint64_t device_fd;
    uint64_t ready_channel_fd;
    uint64_t service_ready_channel_fd;
    uint64_t filed_image_fd;
    uint64_t filed_image_size;
    uint64_t module_count;
    storage_module_image_desc_t modules[STORAGE_STACK_MODULE_CAPACITY];
} storage_seed0root_bootstrap_t;

typedef struct storage_filed_bootstrap {
    uint64_t magic;
    uint64_t device_fd;
    uint64_t control_fd;
    uint64_t module_count;
    storage_module_image_desc_t modules[STORAGE_STACK_MODULE_CAPACITY];
} storage_filed_bootstrap_t;

_Static_assert(sizeof(storage_module_image_desc_t) == 80,
    "storage module descriptor private ABI");
_Static_assert(sizeof(storage_seed0root_bootstrap_t) == 696,
    "storage seed0root bootstrap private ABI");
_Static_assert(sizeof(storage_filed_bootstrap_t) == 672,
    "storage filed bootstrap private ABI");

static inline int storage_module_table_matches_manifest(
    const storage_module_image_desc_t *modules,
    uint64_t module_count)
{
    if (modules == NULL || module_count != STORAGE_STACK_MODULE_COUNT) {
        return 0;
    }
    for (uint64_t i = 0; i < module_count; ++i) {
        if (strncmp(
                modules[i].name,
                storage_stack_modules[i].name,
                STORAGE_STACK_MODULE_NAME_BYTES) != 0 ||
            modules[i].name[STORAGE_STACK_MODULE_NAME_BYTES - 1u] != '\0')
        {
            return 0;
        }
    }
    return 1;
}

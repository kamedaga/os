#pragma once

#include <stdint.h>

typedef struct drmd_property_metadata {
    uint32_t flags;
    uint32_t count_values;
    uint64_t values[2];
    char name[32];
} drmd_property_metadata_t;

int drmd_atomic_property_metadata(
    uint32_t property_id,
    drmd_property_metadata_t *out);

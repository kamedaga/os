#pragma once

#include <stdint.h>

enum {
    STORAGE_SEED0ROOT_BOOTSTRAP_MAGIC = 0x315254424f4f5453ull,
    STORAGE_FILED_BOOTSTRAP_MAGIC = 0x3250474b42584f4bull,
};

typedef struct storage_seed0root_bootstrap {
    uint64_t magic;
    uint64_t device_fd;
    uint64_t ready_channel_fd;
    uint64_t service_ready_channel_fd;
    uint64_t filed_image_fd;
    uint64_t filed_image_size;
    /* The archive is transferred read-only so filed can start its sandbox
     * before any filesystem RPC exists; v1 module handoffs are not retained. */
    uint64_t bootfs_fd;
    uint64_t bootfs_size;
    uint8_t root_uuid[16];
    uint64_t sandbox_ram_bytes;
    uint64_t queue_size;
    uint64_t queue_arena_bytes;
    /* Installed roots use the same GOP-backed terminal as the live system.
     * These are descriptive values; the display device capability arrives
     * independently in the root handoff and remains the mapping authority. */
    uint64_t power_channel_fd;
    uint64_t framebuffer_paddr;
    uint64_t framebuffer_size;
    uint64_t framebuffer_width;
    uint64_t framebuffer_height;
    uint64_t framebuffer_pitch;
} storage_seed0root_bootstrap_t;

typedef struct storage_filed_bootstrap {
    uint64_t magic;
    uint64_t device_fd;
    uint64_t control_fd;
    uint64_t unix_path_fd;
    uint64_t bootfs_fd;
    uint64_t bootfs_size;
    uint8_t root_uuid[16];
    uint64_t sandbox_ram_bytes;
    uint64_t queue_size;
    uint64_t queue_arena_bytes;
} storage_filed_bootstrap_t;

_Static_assert(sizeof(storage_seed0root_bootstrap_t) == 152,
    "storage seed0root bootstrap private ABI");
_Static_assert(sizeof(storage_filed_bootstrap_t) == 88,
    "storage filed bootstrap private ABI");

static inline int storage_root_uuid_valid(const uint8_t uuid[16])
{
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 16; ++i) nonzero |= uuid[i];
    return nonzero != 0;
}

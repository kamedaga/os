#pragma once

#include <stdint.h>

/* Private handoff from seed0boot to seed0root for the RAM-only session.
 * It names existing capabilities; the framebuffer address is descriptive,
 * never an authority to map arbitrary physical memory. */
enum {
    PACHA_LIVE_ROOT_BOOTSTRAP_MAGIC = 0x3154524f4f524d52ull,
    PACHA_LIVE_ROOT_BOOTSTRAP_VERSION = 2,
    PACHA_LIVE_POWER_REQUEST_MAGIC = 0x5257515041434841ull,
    PACHA_LIVE_POWER_OFF = 1,
    PACHA_LIVE_POWER_REBOOT = 2,
};

typedef struct pacha_live_root_bootstrap {
    uint64_t magic;
    uint64_t version;
    uint64_t filed_endpoint_fd;
    uint64_t unix_path_fd;
    uint64_t root_handoff_fd;
    uint64_t power_channel_fd;
    uint64_t framebuffer_paddr;
    uint64_t framebuffer_size;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
} pacha_live_root_bootstrap_t;

_Static_assert(sizeof(pacha_live_root_bootstrap_t) == 88,
    "live root bootstrap size");
